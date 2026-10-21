#include "alchemy/darwin/foundation/headless_shells.hpp"

#include "alchemy/core_foundation.hpp"
#include "alchemy/engine_adapter.hpp"
#include "alchemy/objc_runtime.hpp"
#include "alchemy/process_context.hpp"

#include <bit>
#include <initializer_list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

// alchemy.exe only (darwin/foundation/headless_shells.hpp): NSUserDefaults,
// volatile. Search order: argument domain (-key value pairs from argv), the
// application domain written by set*, then registerDefaults:. Nothing is
// persisted: -synchronize answers NO, as on a failed preferences write. The
// class itself is registered by foundation/class_shells_and_events.cpp in both
// builds; in alchemy-fx corefoundation/cf_preferences.cpp owns these methods.
namespace alchemy::foundation_shells {
namespace {

struct UserDefaults : std::enable_shared_from_this<UserDefaults> {
    UserDefaults(ProcessContext& process, ObjcRuntime& objc, CoreFoundation& cf) : context(process), objc(objc), cf(cf) {}
    ProcessContext& context;
    ObjcRuntime& objc;
    CoreFoundation& cf;
    std::mutex mutex;
    std::uint64_t defaults_object{};
    std::map<std::string, std::uint64_t> argument_domain, application_domain, registration_domain;

    std::uint64_t guest_call(std::string_view library, std::string_view symbol, std::initializer_list<std::uint64_t> arguments) {
        const auto found = context.resolve(library, symbol);
        if (!found) throw std::runtime_error("Alchemy installer frameworks: missing " + std::string(symbol));
        return EngineAdapter(context.calling_engine()).call(found->address, std::span(arguments.begin(), arguments.size()));
    }
    std::uint64_t retain(std::uint64_t object) { return object ? guest_call(objc_library, "_objc_retain", {object}) : 0; }
    void release(std::uint64_t object) { if (object) static_cast<void>(guest_call(objc_library, "_objc_release", {object})); }
    std::uint64_t constant_string(std::string_view text) {
        const auto object = cf.string(text);
        if (const auto record = context.objects().find(object)) record->immortal = true;
        return object;
    }
    std::string key(std::uint64_t object) {
        if (!object || cf.kind(object) != ObjectKind::string) throw std::runtime_error("Alchemy NSUserDefaults: key is not a string");
        return cf.utf8(object);
    }
    std::uint64_t lookup(const std::string& name) {
        std::lock_guard lock(mutex);
        for (const auto* domain : {&argument_domain, &application_domain, &registration_domain})
            if (const auto found = domain->find(name); found != domain->end()) return found->second;
        return 0;
    }
    void store(const std::string& name, std::uint64_t value) {
        value = retain(value);
        std::uint64_t previous{};
        {
            std::lock_guard lock(mutex);
            auto& slot = application_domain[name];
            previous = slot;
            slot = value;
            if (!value) application_domain.erase(name);
        }
        release(previous);
    }
    std::optional<double> numeric(std::uint64_t value) {
        if (!value) return std::nullopt;
        const auto kind = cf.kind(value);
        if (kind == ObjectKind::string) {
            const auto text = cf.utf8(value);
            if (text == "YES" || text == "yes" || text == "true") return 1.0;
            if (text == "NO" || text == "no" || text == "false") return 0.0;
            try { return std::stod(text); } catch (...) { return 0.0; }
        }
        return cf.number_value(value);
    }
    void method(std::string selector, transmute::GuestCallHandler handler, bool class_method = false, std::string types = "@@:") {
        const auto cls = objc.find_class("NSUserDefaults");
        if (!cls) throw std::runtime_error("Alchemy installer frameworks: missing class NSUserDefaults");
        static_cast<void>(objc.add_native_method(cls, std::move(selector),
            [owner = shared_from_this(), handler = std::move(handler)](transmute::CpuState& s, transmute::GuestMemory& m) { handler(s, m); },
            class_method, std::move(types)));
    }
    void install() {
        const auto arguments = context.arguments;
        for (std::size_t index = 1; index + 1 < arguments.size(); ++index) {
            const auto& name = arguments[index];
            if (name.size() < 2 || name[0] != '-' || name.find('=') != std::string::npos) continue;
            argument_domain[name.substr(1)] = constant_string(arguments[index + 1]);
            ++index;
        }
        method("standardUserDefaults", [this](transmute::CpuState& s, transmute::GuestMemory&) {
            std::lock_guard lock(mutex);
            if (!defaults_object) {
                defaults_object = objc.allocate(objc.find_class("NSUserDefaults"));
                if (const auto record = context.objects().find(defaults_object)) record->immortal = true;
            }
            s.x[0] = defaults_object;
        }, true);
        method("objectForKey:", [this](transmute::CpuState& s, transmute::GuestMemory&) { s.x[0] = lookup(key(s.x[2])); }, false, "@@:@");
        for (const auto* name : {"stringForKey:", "arrayForKey:", "dictionaryForKey:", "dataForKey:"}) {
            const auto wanted = std::string(name) == "stringForKey:" ? ObjectKind::string
                : std::string(name) == "arrayForKey:" ? ObjectKind::array
                : std::string(name) == "dictionaryForKey:" ? ObjectKind::dictionary : ObjectKind::data;
            method(name, [this, wanted](transmute::CpuState& s, transmute::GuestMemory&) {
                const auto value = lookup(key(s.x[2]));
                s.x[0] = value && cf.kind(value) == wanted ? value : 0;
            }, false, "@@:@");
        }
        method("boolForKey:", [this](transmute::CpuState& s, transmute::GuestMemory&) {
            s.x[0] = numeric(lookup(key(s.x[2]))).value_or(0.0) != 0.0;
        }, false, "c@:@");
        method("integerForKey:", [this](transmute::CpuState& s, transmute::GuestMemory&) {
            s.x[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(numeric(lookup(key(s.x[2]))).value_or(0.0)));
        }, false, "q@:@");
        method("doubleForKey:", [this](transmute::CpuState& s, transmute::GuestMemory&) {
            s.v[0] = {std::bit_cast<std::uint64_t>(numeric(lookup(key(s.x[2]))).value_or(0.0)), 0};
        }, false, "d@:@");
        method("setObject:forKey:", [this](transmute::CpuState& s, transmute::GuestMemory&) { store(key(s.x[3]), s.x[2]); }, false, "v@:@@");
        method("removeObjectForKey:", [this](transmute::CpuState& s, transmute::GuestMemory&) { store(key(s.x[2]), 0); }, false, "v@:@");
        method("setBool:forKey:", [this](transmute::CpuState& s, transmute::GuestMemory&) {
            const auto value = cf.boolean((s.x[2] & 0xff) != 0);
            store(key(s.x[3]), value);
        }, false, "v@:c@");
        method("setInteger:forKey:", [this](transmute::CpuState& s, transmute::GuestMemory&) {
            const auto value = cf.integer(static_cast<std::int64_t>(s.x[2]));
            store(key(s.x[3]), value);
            context.objects().release(value);
        }, false, "v@:q@");
        method("registerDefaults:", [this](transmute::CpuState& s, transmute::GuestMemory&) {
            if (!s.x[2]) return;
            for (const auto& [name, value] : cf.dictionary_entries(s.x[2])) {
                const auto text = key(name);
                const auto kept = retain(value);
                std::uint64_t previous{};
                {
                    std::lock_guard lock(mutex);
                    auto& slot = registration_domain[text];
                    previous = slot;
                    slot = kept;
                }
                release(previous);
            }
        }, false, "v@:@");
        method("synchronize", [](transmute::CpuState& s, transmute::GuestMemory&) { s.x[0] = 0; }, false, "c@:");
    }
};

} // namespace

void install_user_defaults_shell(ProcessContext& process, ObjcRuntime& objc, CoreFoundation& cf) {
    std::make_shared<UserDefaults>(process, objc, cf)->install();
}

} // namespace alchemy::foundation_shells
