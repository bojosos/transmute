#include "alchemy/darwin/kit/fwgaps/fwgaps.hpp"
#include "alchemy/darwin/kit/export_fill/export_fill.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

// CFPlugIn (alchemy-fx), as CFPlugIn.h documents it. A CFPlugInRef is the
// CFBundle of the plug-in. CFPlugInCreate reads the bundle's Info.plist:
// CFPlugInFactories (factory UUID -> function name) and CFPlugInTypes (type
// UUID -> factory UUIDs) register statically; with CFPlugInDynamicRegistration
// the executable is loaded and its CFPlugInDynamicRegisterFunction (default
// CFPlugInDynamicRegister) is called with the plug-in to register itself.
// Factories and types are one process-wide registry. CFPlugInInstanceCreate
// loads the plug-in's executable when needed and calls the factory function
// (allocator, type UUID); the instance counts that CFPlugInAddInstanceForFactory
// and RemoveInstanceForFactory keep let a load-on-demand plug-in unload when the
// last instance goes. Also CFUUIDCreateFromString, CFBundleGetPlugIn and
// CFBundleGetTypeID.
namespace alchemy::gui::fill {
namespace {

using Uuid = std::array<std::uint8_t, 16>;

[[noreturn]] void fail(const std::string& why) { throw std::runtime_error("Alchemy CFPlugIn: " + why); }

struct Factory {
    Address plugin{};          // the plug-in bundle (not retained: plug-ins live for the process), or 0
    Address function{};        // the factory function, once known
    std::string name;          // its name in the plug-in's executable
    std::uint64_t instances{};
};

struct PlugIns final : public Provider {
    using Provider::Provider;
    std::mutex mutex;
    std::map<Uuid, Factory> factories;
    std::map<Uuid, std::vector<Uuid>> types;      // type -> factories
    std::map<Address, bool> plugins;               // plug-in bundle -> load on demand

    std::optional<Uuid> uuid_of(Address object) {
        const auto record = object ? process.objects().find(object) : nullptr;
        const auto payload = record ? std::dynamic_pointer_cast<export_fill::CfUuid>(record->payload) : nullptr;
        if (!payload) return std::nullopt;
        return payload->bytes;
    }
    Uuid require_uuid(Address object, const char* what) {
        const auto value = uuid_of(object);
        if (!value) fail(std::string(what) + " is not a CFUUID");
        return *value;
    }
    static std::optional<Uuid> parse(std::string_view text) {
        Uuid bytes{};
        std::size_t count = 0;
        int high = -1;
        for (const char c : text) {
            if (c == '-') continue;
            const int digit = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
            if (digit < 0 || count >= 16) return std::nullopt;
            if (high < 0) high = digit;
            else { bytes[count++] = static_cast<std::uint8_t>(high * 16 + digit); high = -1; }
        }
        if (count != 16 || high >= 0) return std::nullopt;
        return bytes;
    }
    Address make_uuid(const Uuid& bytes) {   // +1
        Address low{}, high{};
        std::memcpy(&low, bytes.data(), 8);
        std::memcpy(&high, bytes.data() + 8, 8);
        return call(core_foundation, "_CFUUIDCreateFromUUIDBytes", {0, low, high});
    }
    Address info_value(Address bundle, std::string_view key) {
        const auto name = cf.string(key);
        const auto value = call(core_foundation, "_CFBundleGetValueForInfoDictionaryKey", {bundle, name});
        release(name);
        return value;
    }
    bool truthy(Address value) {
        if (!value) return false;
        if (cf.kind(value) == ObjectKind::string) {
            const auto text = cf.utf8(value);
            return text == "YES" || text == "yes" || text == "true" || text == "1";
        }
        return (call(core_foundation, "_CFBooleanGetValue", {value}) & 0xff) != 0;
    }
    // The factory's function, loading its plug-in when needed (0 when it cannot be found).
    Address function_of(const Uuid& factory) {
        Address plugin{}, function{};
        std::string name;
        {
            std::lock_guard lock(mutex);
            const auto found = factories.find(factory);
            if (found == factories.end()) return 0;
            plugin = found->second.plugin;
            function = found->second.function;
            name = found->second.name;
        }
        if (function) return function;
        if (!plugin || name.empty()) return 0;
        const auto symbol = cf.string(name);
        function = call(core_foundation, "_CFBundleGetFunctionPointerForName", {plugin, symbol});
        release(symbol);
        if (!function) return 0;
        std::lock_guard lock(mutex);
        if (const auto found = factories.find(factory); found != factories.end()) found->second.function = function;
        return function;
    }
    void register_static(Address plugin) {
        const auto factory_map = info_value(plugin, "CFPlugInFactories");
        if (factory_map && cf.kind(factory_map) == ObjectKind::dictionary)
            for (const auto& [key, value] : cf.dictionary_entries(factory_map)) {
                const auto factory = cf.kind(key) == ObjectKind::string ? parse(cf.utf8(key)) : std::nullopt;
                if (!factory || cf.kind(value) != ObjectKind::string) continue;
                std::lock_guard lock(mutex);
                factories.try_emplace(*factory, Factory{plugin, 0, cf.utf8(value), 0});
            }
        const auto type_map = info_value(plugin, "CFPlugInTypes");
        if (type_map && cf.kind(type_map) == ObjectKind::dictionary)
            for (const auto& [key, value] : cf.dictionary_entries(type_map)) {
                const auto type = cf.kind(key) == ObjectKind::string ? parse(cf.utf8(key)) : std::nullopt;
                if (!type || cf.kind(value) != ObjectKind::array) continue;
                for (const auto item : cf.array_values(value)) {
                    const auto factory = cf.kind(item) == ObjectKind::string ? parse(cf.utf8(item)) : std::nullopt;
                    if (!factory) continue;
                    std::lock_guard lock(mutex);
                    auto& list = types[*type];
                    if (std::find(list.begin(), list.end(), *factory) == list.end()) list.push_back(*factory);
                }
            }
    }
    Address factories_for(const Uuid& type, std::optional<Address> plugin) {   // +1 CFArray of CFUUIDs
        std::vector<Uuid> matches;
        {
            std::lock_guard lock(mutex);
            if (const auto found = types.find(type); found != types.end())
                for (const auto& factory : found->second) {
                    const auto entry = factories.find(factory);
                    if (entry == factories.end()) continue;
                    if (plugin && entry->second.plugin != *plugin) continue;
                    matches.push_back(factory);
                }
        }
        std::vector<Address> values;
        for (const auto& factory : matches) values.push_back(make_uuid(factory));
        const auto array = cf.array(values);
        for (const auto value : values) release(value);
        return array;
    }
    void install();
};

void PlugIns::install() {
    // CFUUIDRef CFUUIDCreateFromString(CFAllocatorRef, CFStringRef uuidStr): NULL for text that is not a UUID.
    raw(core_foundation, "_CFUUIDCreateFromString", [this](CpuState& s, GuestMemory&) {
        const auto bytes = s.x[1] && cf.kind(s.x[1]) == ObjectKind::string ? parse(cf.utf8(s.x[1])) : std::nullopt;
        s.x[0] = bytes ? make_uuid(*bytes) : 0;
    });
    // CFPlugInRef CFPlugInCreate(CFAllocatorRef, CFURLRef plugInURL)
    raw(core_foundation, "_CFPlugInCreate", [this](CpuState& s, GuestMemory&) {
        const auto plugin = s.x[1] ? call(core_foundation, "_CFBundleCreate", {s.x[0], s.x[1]}) : 0;
        if (!plugin) { s.x[0] = 0; return; }
        bool known{};
        {
            std::lock_guard lock(mutex);
            known = plugins.contains(plugin);
            if (!known) plugins[plugin] = true;
        }
        if (!known) {
            retain(plugin);   // the registry's reference: plug-ins stay for the process
            register_static(plugin);
            if (truthy(info_value(plugin, "CFPlugInDynamicRegistration"))) {
                const auto custom = info_value(plugin, "CFPlugInDynamicRegisterFunction");
                const auto name = custom && cf.kind(custom) == ObjectKind::string && cf.length(custom) ? retain(custom) : cf.string("CFPlugInDynamicRegister");
                const auto function = call(core_foundation, "_CFBundleGetFunctionPointerForName", {plugin, name});
                release(name);
                if (!function) fail("the plug-in's dynamic registration function is missing");
                static_cast<void>(bridge->call_address(function, std::array<Address, 1>{plugin}));
            }
        }
        s.x[0] = plugin;
    });
    raw(core_foundation, "_CFPlugInGetBundle", [](CpuState&, GuestMemory&) {});   // the plug-in is its bundle: x0 unchanged
    // CFGetTypeID of a CFBundle (an NSBundle); a CFPlugIn is a CFBundle.
    if (const auto bundle_class = objc.find_class("NSBundle")) register_type(bundle_class, TypeId::bundle);
    constant(core_foundation, "_CFBundleGetTypeID", static_cast<Address>(TypeId::bundle));
    constant(core_foundation, "_CFPlugInGetTypeID", static_cast<Address>(TypeId::bundle));
    // CFPlugInRef CFBundleGetPlugIn(CFBundleRef): the bundle when CFPlugInCreate made it a plug-in.
    raw(core_foundation, "_CFBundleGetPlugIn", [this](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        if (!plugins.contains(s.x[0])) s.x[0] = 0;
    });
    raw(core_foundation, "_CFPlugInSetLoadOnDemand", [this](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        if (const auto found = plugins.find(s.x[0]); found != plugins.end()) found->second = (s.x[1] & 0xff) != 0;
    });
    raw(core_foundation, "_CFPlugInIsLoadOnDemand", [this](CpuState& s, GuestMemory&) {
        std::lock_guard lock(mutex);
        const auto found = plugins.find(s.x[0]);
        s.x[0] = found != plugins.end() && found->second ? 1 : 0;
    });
    // CFArrayRef CFPlugInFindFactoriesForPlugInType(CFUUIDRef typeUUID)
    raw(core_foundation, "_CFPlugInFindFactoriesForPlugInType", [this](CpuState& s, GuestMemory&) {
        s.x[0] = factories_for(require_uuid(s.x[0], "the type"), std::nullopt);
    });
    // CFArrayRef CFPlugInFindFactoriesForPlugInTypeInPlugIn(CFUUIDRef typeUUID, CFPlugInRef plugIn)
    raw(core_foundation, "_CFPlugInFindFactoriesForPlugInTypeInPlugIn", [this](CpuState& s, GuestMemory&) {
        s.x[0] = factories_for(require_uuid(s.x[0], "the type"), s.x[1]);
    });
    // void* CFPlugInInstanceCreate(CFAllocatorRef, CFUUIDRef factoryUUID, CFUUIDRef typeUUID): what the factory returns.
    raw(core_foundation, "_CFPlugInInstanceCreate", [this](CpuState& s, GuestMemory&) {
        const auto function = function_of(require_uuid(s.x[1], "the factory"));
        s.x[0] = function ? bridge->call_address(function, std::array<Address, 2>{s.x[0], s.x[2]}) : 0;
    });
    // Boolean CFPlugInRegisterFactoryFunction(CFUUIDRef factoryUUID, CFPlugInFactoryFunction func)
    raw(core_foundation, "_CFPlugInRegisterFactoryFunction", [this](CpuState& s, GuestMemory&) {
        const auto factory = require_uuid(s.x[0], "the factory");
        std::lock_guard lock(mutex);
        s.x[0] = factories.try_emplace(factory, Factory{0, s.x[1], {}, 0}).second ? 1 : 0;
    });
    // Boolean CFPlugInRegisterFactoryFunctionByName(CFUUIDRef factoryUUID, CFPlugInRef plugIn, CFStringRef functionName)
    raw(core_foundation, "_CFPlugInRegisterFactoryFunctionByName", [this](CpuState& s, GuestMemory&) {
        const auto factory = require_uuid(s.x[0], "the factory");
        if (!s.x[1] || !s.x[2]) { s.x[0] = 0; return; }
        std::lock_guard lock(mutex);
        s.x[0] = factories.try_emplace(factory, Factory{s.x[1], 0, cf.utf8(s.x[2]), 0}).second ? 1 : 0;
    });
    raw(core_foundation, "_CFPlugInUnregisterFactory", [this](CpuState& s, GuestMemory&) {
        const auto factory = require_uuid(s.x[0], "the factory");
        std::lock_guard lock(mutex);
        s.x[0] = factories.erase(factory) ? 1 : 0;
        for (auto& [type, list] : types) std::erase(list, factory);
    });
    // Boolean CFPlugInRegisterPlugInType(CFUUIDRef factoryUUID, CFUUIDRef typeUUID)
    raw(core_foundation, "_CFPlugInRegisterPlugInType", [this](CpuState& s, GuestMemory&) {
        const auto factory = require_uuid(s.x[0], "the factory"), type = require_uuid(s.x[1], "the type");
        std::lock_guard lock(mutex);
        auto& list = types[type];
        if (std::find(list.begin(), list.end(), factory) == list.end()) list.push_back(factory);
        s.x[0] = 1;
    });
    raw(core_foundation, "_CFPlugInUnregisterPlugInType", [this](CpuState& s, GuestMemory&) {
        const auto factory = require_uuid(s.x[0], "the factory"), type = require_uuid(s.x[1], "the type");
        std::lock_guard lock(mutex);
        const auto found = types.find(type);
        s.x[0] = found != types.end() && std::erase(found->second, factory) ? 1 : 0;
    });
    raw(core_foundation, "_CFPlugInAddInstanceForFactory", [this](CpuState& s, GuestMemory&) {
        const auto factory = require_uuid(s.x[0], "the factory");
        std::lock_guard lock(mutex);
        if (const auto found = factories.find(factory); found != factories.end()) ++found->second.instances;
    });
    // The last instance of a load-on-demand plug-in's factories unloads its executable.
    raw(core_foundation, "_CFPlugInRemoveInstanceForFactory", [this](CpuState& s, GuestMemory&) {
        const auto factory = require_uuid(s.x[0], "the factory");
        Address unload{};
        {
            std::lock_guard lock(mutex);
            const auto found = factories.find(factory);
            if (found == factories.end() || !found->second.instances) return;
            --found->second.instances;
            const auto plugin = found->second.plugin;
            const auto on_demand = plugin && plugins.contains(plugin) && plugins[plugin];
            const auto idle = std::none_of(factories.begin(), factories.end(), [&](const auto& entry) {
                return entry.second.plugin == plugin && entry.second.instances;
            });
            if (on_demand && idle) {
                unload = plugin;
                for (auto& [uuid, entry] : factories) if (entry.plugin == plugin) entry.function = 0;   // looked up again after reloading
            }
        }
        if (unload) static_cast<void>(call(core_foundation, "_CFBundleUnloadExecutable", {unload}));
    });
}

} // namespace

void install_cf_plugins(InstallContext& context) {
    std::make_shared<PlugIns>(context)->install();
}

} // namespace alchemy::gui::fill
