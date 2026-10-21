#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/darwin/corefoundation/cf_plist.hpp"

#include "alchemy/path.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

// Property list data (CFPropertyListCreateData/CreateWithData and the older
// XML calls), CFPreferences and NSUserDefaults.
//
// Preferences live where macOS keeps them, inside the guest home directory:
//   ~/Library/Preferences/<domain>.plist                  current user, any host
//   ~/Library/Preferences/ByHost/<domain>.<UUID>.plist    current user, this host
//   /Library/Preferences/<domain>.plist                   any user (read-only:
//                                                         the virtual identity is
//                                                         not an administrator)
// kCFPreferencesAnyApplication is the ".GlobalPreferences" domain and
// kCFPreferencesCurrentApplication the main bundle identifier (else the
// executable name). Files are binary property lists, as cfprefsd writes them;
// both encodings are read. Values are cached per domain and written by the
// synchronize calls; an unmodified domain is re-read from disk then.
namespace alchemy::export_fill {
namespace {

std::filesystem::path host_path(ProcessContext& process, const std::string& guest) {
    return process.map_guest_path ? process.map_guest_path(guest) : utf8_path(guest);
}

std::string environment_value(const ProcessContext& process, std::string_view name) {
    for (const auto& entry : process.environment)
        if (entry.size() > name.size() && entry.compare(0, name.size(), name) == 0 && entry[name.size()] == '=') return entry.substr(name.size() + 1);
    return {};
}

struct Domain {
    std::filesystem::path file;
    Plist values{Plist::Kind::dictionary};
    bool loaded{}, dirty{}, writable{true};
};

class Preferences {
public:
    Preferences(ProcessContext& process, ObjcRuntime& objc, CoreFoundation& cf) : process_(process), objc_(objc), cf_(cf) {}

    // Resolves (application, user, host) CF strings to a domain.
    Domain& domain(std::uint64_t application, std::uint64_t user, std::uint64_t host) {
        return domain(application_name(application), is(user, "kCFPreferencesAnyUser"), is(host, "kCFPreferencesCurrentHost"));
    }
    Domain& domain(const std::string& application, bool any_user, bool current_host) {
        const auto key = application + (any_user ? "|any" : "|user") + (current_host ? "|host" : "|any");
        std::lock_guard lock(mutex_);
        auto& value = domains_[key];
        if (value.file.empty()) {
            std::string directory = any_user ? std::string("/Library/Preferences") : home() + "/Library/Preferences";
            const auto name = current_host ? "ByHost/" + application + "." + host_machine_uuid() + ".plist" : application + ".plist";
            value.file = host_path(process_, directory + "/" + name);
            value.writable = !any_user;
        }
        load(value, false);
        return value;
    }
    std::mutex& mutex() { return mutex_; }
    static void load(Domain& value, bool force) {
        if (value.loaded && !force) return;
        value.loaded = true;
        std::ifstream file(value.file, std::ios::binary);
        if (!file) { if (!value.dirty) value.values = Plist{Plist::Kind::dictionary}; return; }
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        auto parsed = plist_parse(bytes);
        value.values = parsed && parsed->kind == Plist::Kind::dictionary ? std::move(*parsed) : Plist{Plist::Kind::dictionary};
    }
    // Writes a modified domain; re-reads an unmodified one. False when the
    // domain cannot be written.
    static bool synchronize(Domain& value) {
        if (!value.dirty) { load(value, true); return true; }
        if (!value.writable) return false;
        std::error_code error;
        std::filesystem::create_directories(value.file.parent_path(), error);
        const auto bytes = plist_to_binary(value.values);
        const auto temporary = value.file.string() + ".alchemy-tmp";
        {
            std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
            if (!file || !file.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) return false;
        }
        std::filesystem::rename(temporary, value.file, error);
        if (error) { std::filesystem::remove(temporary, error); return false; }
        value.dirty = false;
        return true;
    }
    std::string application_name(std::uint64_t application) {
        if (!application) throw std::runtime_error("Alchemy CFPreferences: NULL application");
        const auto name = cf_.utf8(application);
        if (name == "kCFPreferencesAnyApplication") return ".GlobalPreferences";
        if (name == "kCFPreferencesCurrentApplication") return current_application();
        return name;
    }
    std::string current_application() {
        if (const auto bundle_class = objc_.find_class("NSBundle")) {
            const auto main = objc_.send(bundle_class, "mainBundle").x0;
            const auto identifier = main ? objc_.send(main, "bundleIdentifier").x0 : 0;
            if (identifier) return cf_.utf8(identifier);
        }
        auto name = guest_path_string(process_.executable);
        const auto slash = name.rfind('/');
        return slash == std::string::npos ? name : name.substr(slash + 1);
    }
    std::string home() {
        auto value = environment_value(process_, "HOME");
        if (value.empty()) {
            const auto profile = environment_value(process_, "USERPROFILE");
            value = profile.empty() ? std::string("/") : guest_path_string(utf8_path(profile));
        }
        while (value.size() > 1 && value.back() == '/') value.pop_back();
        return value;
    }
    bool is(std::uint64_t string, std::string_view name) { return string && cf_.utf8(string) == name; }

    // The CFPreferencesCopyAppValue search list.
    std::vector<Domain*> search(const std::string& application) {
        std::vector<Domain*> list;
        for (const auto& [app, any_user, current_host] : std::array<std::tuple<std::string, bool, bool>, 6>{{
                 {application, false, true}, {application, false, false}, {".GlobalPreferences", false, true},
                 {".GlobalPreferences", false, false}, {application, true, false}, {".GlobalPreferences", true, false}}})
            list.push_back(&domain(app, any_user, current_host));
        return list;
    }

private:
    ProcessContext& process_;
    ObjcRuntime& objc_;
    CoreFoundation& cf_;
    std::mutex mutex_;
    std::map<std::string, Domain> domains_;
};

// NSUserDefaults state: suites searched after the application domain, and the
// registration domain.
struct Defaults : ObjectPayload {
    std::string suite;                 // initWithSuiteName: (empty: the application)
    std::vector<std::string> suites;   // addSuiteNamed:
    Plist registration{Plist::Kind::dictionary};
    Plist arguments{Plist::Kind::dictionary};
};

std::optional<double> numeric(const Plist& value) {
    switch (value.kind) {
    case Plist::Kind::integer: return static_cast<double>(value.integer);
    case Plist::Kind::real: return value.real;
    case Plist::Kind::boolean: return value.boolean ? 1.0 : 0.0;
    case Plist::Kind::string: {
        const char* text = value.text.c_str();
        char* end{};
        const auto parsed = std::strtod(text, &end);
        if (end == text) return std::nullopt;
        return parsed;
    }
    default: return std::nullopt;
    }
}

} // namespace

void install_cf_preferences(Provider& p, ObjcRuntime& objc, CoreFoundation& cf) {
    auto& process = p.process();
    const std::string cf_library(core_foundation_library);
    const auto function = [&](const char* name, transmute::GuestCallHandler handler) {
        if (provided(process, cf_library, name)) return;
        static_cast<void>(process.install_function(name, std::move(handler), cf_library));
    };
    const auto data_object = [&process, &cf](std::span<const std::uint8_t> bytes) {
        const auto storage = process.allocate_guest(std::max<std::size_t>(bytes.size(), 1));
        if (!bytes.empty()) process.main_engine().write_bytes(storage, bytes);
        std::uint64_t data{};
        try { data = cf.invoke("CFDataCreate", std::array<std::uint64_t, 3>{0, storage, bytes.size()}); }
        catch (...) { process.release_guest(storage); throw; }
        process.release_guest(storage);
        return data;
    };
    const auto data_bytes = [&process, &cf](std::uint64_t data) {
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(cf.invoke("CFDataGetLength", std::array<std::uint64_t, 1>{data})));
        if (!bytes.empty()) process.main_engine().read_bytes(cf.invoke("CFDataGetBytePtr", std::array<std::uint64_t, 1>{data}), bytes);
        return bytes;
    };

    // ---- property list data ----
    // CFDataRef CFPropertyListCreateData(alloc, CFPropertyListRef, CFPropertyListFormat, options, CFErrorRef*)
    // XML and binary; the OpenStep format cannot be written (as on macOS).
    function("_CFPropertyListCreateData", [&process, &cf, data_object](CpuState& s, GuestMemory& m) {
        if (s.x[4]) m.write64(s.x[4], 0);
        if (s.x[0]) throw std::runtime_error("Alchemy CoreFoundation: custom allocators are unsupported");
        if (!s.x[1] || (s.x[2] != plist_xml_format && s.x[2] != plist_binary_format)) { s.x[0] = 0; return; }
        const auto value = plist_from_guest(process, cf, s.x[1]);
        if (s.x[2] == plist_xml_format) {
            const auto text = plist_to_xml(value);
            s.x[0] = data_object(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
        } else s.x[0] = data_object(plist_to_binary(value));
    });
    // CFDataRef CFPropertyListCreateXMLData(alloc, CFPropertyListRef)
    function("_CFPropertyListCreateXMLData", [&process, &cf, data_object](CpuState& s, GuestMemory&) {
        const auto text = plist_to_xml(plist_from_guest(process, cf, s.x[1]));
        s.x[0] = data_object(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    });
    // CFPropertyListRef CFPropertyListCreateWithData(alloc, CFDataRef, options, CFPropertyListFormat*, CFErrorRef*)
    function("_CFPropertyListCreateWithData", [&process, &cf, data_bytes](CpuState& s, GuestMemory& m) {
        if (s.x[4]) m.write64(s.x[4], 0);
        if (!s.x[1]) { s.x[0] = 0; return; }
        std::uint64_t format{};
        const auto parsed = plist_parse(data_bytes(s.x[1]), &format);
        if (!parsed) { s.x[0] = 0; return; }
        if (s.x[3]) m.write64(s.x[3], format);
        s.x[0] = plist_to_guest(process, cf, *parsed, s.x[2] & 3);
    });
    // CFPropertyListRef CFPropertyListCreateFromXMLData(alloc, CFDataRef, options, CFStringRef* errorString)
    function("_CFPropertyListCreateFromXMLData", [&process, &cf, data_bytes](CpuState& s, GuestMemory& m) {
        if (s.x[3]) m.write64(s.x[3], 0);
        const auto parsed = s.x[1] ? plist_parse(data_bytes(s.x[1])) : std::nullopt;
        if (!parsed && s.x[3]) m.write64(s.x[3], cf.string("Alchemy: the data is not a property list"));
        s.x[0] = parsed ? plist_to_guest(process, cf, *parsed, s.x[2] & 3) : 0;
    });
    // CFPropertyListRef CFPropertyListCreateDeepCopy(alloc, CFPropertyListRef, options)
    function("_CFPropertyListCreateDeepCopy", [&process, &cf](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[1] ? plist_to_guest(process, cf, plist_from_guest(process, cf, s.x[1]), s.x[2] & 3) : 0;
    });
    // Boolean CFPropertyListIsValid(CFPropertyListRef, CFPropertyListFormat)
    function("_CFPropertyListIsValid", [&process, &cf](CpuState& s, GuestMemory&) {
        try { static_cast<void>(plist_from_guest(process, cf, s.x[0])); s.x[0] = 1; }
        catch (const std::runtime_error&) { s.x[0] = 0; }
    });

    // ---- CFPreferences ----
    auto preferences = std::make_shared<Preferences>(process, objc, cf);
    // CFPropertyListRef CFPreferencesCopyValue(CFStringRef key, app, user, host)
    function("_CFPreferencesCopyValue", [&process, &cf, preferences](CpuState& s, GuestMemory&) {
        auto& domain = preferences->domain(s.x[1], s.x[2], s.x[3]);
        std::lock_guard lock(preferences->mutex());
        const auto* value = domain.values.find(cf.utf8(s.x[0]));
        s.x[0] = value ? plist_to_guest(process, cf, *value) : 0;
    });
    // CFPropertyListRef CFPreferencesCopyAppValue(CFStringRef key, CFStringRef app)
    function("_CFPreferencesCopyAppValue", [&process, &cf, preferences](CpuState& s, GuestMemory&) {
        const auto key = cf.utf8(s.x[0]);
        for (auto* domain : preferences->search(preferences->application_name(s.x[1]))) {
            std::lock_guard lock(preferences->mutex());
            if (const auto* value = domain->values.find(key)) { s.x[0] = plist_to_guest(process, cf, *value); return; }
        }
        s.x[0] = 0;
    });
    // Boolean CFPreferencesGetAppBooleanValue(key, app, Boolean* keyExistsAndHasValidFormat)
    // CFIndex CFPreferencesGetAppIntegerValue(key, app, Boolean* keyExistsAndHasValidFormat)
    for (const bool boolean : {true, false}) {
        function(boolean ? "_CFPreferencesGetAppBooleanValue" : "_CFPreferencesGetAppIntegerValue",
                 [&cf, preferences, boolean](CpuState& s, GuestMemory& m) {
            const auto key = cf.utf8(s.x[0]);
            std::optional<double> number;
            bool found = false;
            for (auto* domain : preferences->search(preferences->application_name(s.x[1]))) {
                std::lock_guard lock(preferences->mutex());
                if (const auto* value = domain->values.find(key)) {
                    found = true;
                    if (boolean && value->kind == Plist::Kind::string) {
                        auto text = value->text;
                        std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                        if (text == "yes" || text == "true") number = 1;
                        else if (text == "no" || text == "false") number = 0;
                        else number = numeric(*value);
                    } else number = numeric(*value);
                    break;
                }
            }
            if (s.x[2]) m.write8(s.x[2], found && number ? 1 : 0);
            s.x[0] = !number ? 0 : boolean ? (*number != 0 ? 1 : 0) : static_cast<std::uint64_t>(static_cast<std::int64_t>(*number));
        });
    }
    // CFArrayRef CFPreferencesCopyKeyList(app, user, host): NULL when the domain is empty.
    function("_CFPreferencesCopyKeyList", [&process, &cf, preferences](CpuState& s, GuestMemory&) {
        auto& domain = preferences->domain(s.x[0], s.x[1], s.x[2]);
        std::vector<std::uint64_t> keys;
        {
            std::lock_guard lock(preferences->mutex());
            for (const auto& [key, value] : domain.values.entries) { static_cast<void>(value); keys.push_back(cf.string(key)); }
        }
        if (keys.empty()) { s.x[0] = 0; return; }
        s.x[0] = cf.array(keys);
        for (const auto key : keys) process.objects().release(key);
    });
    // void CFPreferencesSetValue(key, value, app, user, host); NULL removes the key.
    const auto set_value = [&process, &cf, preferences](Domain& domain, std::uint64_t key, std::uint64_t value) {
        const auto name = cf.utf8(key);
        std::optional<Plist> converted;
        if (value) converted = plist_from_guest(process, cf, value);
        std::lock_guard lock(preferences->mutex());
        if (converted) domain.values.set(name, std::move(*converted));
        else domain.values.erase(name);
        domain.dirty = true;
    };
    function("_CFPreferencesSetValue", [preferences, set_value](CpuState& s, GuestMemory&) {
        set_value(preferences->domain(s.x[2], s.x[3], s.x[4]), s.x[0], s.x[1]);
        s.x[0] = 0;
    });
    // void CFPreferencesSetAppValue(key, value, app): current user, any host.
    function("_CFPreferencesSetAppValue", [preferences, set_value](CpuState& s, GuestMemory&) {
        set_value(preferences->domain(preferences->application_name(s.x[2]), false, false), s.x[0], s.x[1]);
        s.x[0] = 0;
    });
    // void CFPreferencesSetMultiple(CFDictionaryRef keysToSet, CFArrayRef keysToRemove, app, user, host)
    function("_CFPreferencesSetMultiple", [&cf, preferences, set_value](CpuState& s, GuestMemory&) {
        auto& domain = preferences->domain(s.x[2], s.x[3], s.x[4]);
        if (s.x[0]) for (const auto& [key, value] : cf.dictionary_entries(s.x[0])) set_value(domain, key, value);
        if (s.x[1]) for (const auto key : cf.array_values(s.x[1])) set_value(domain, key, 0);
        s.x[0] = 0;
    });
    // Boolean CFPreferencesSynchronize(app, user, host)
    function("_CFPreferencesSynchronize", [preferences](CpuState& s, GuestMemory&) {
        auto& domain = preferences->domain(s.x[0], s.x[1], s.x[2]);
        std::lock_guard lock(preferences->mutex());
        s.x[0] = Preferences::synchronize(domain) ? 1 : 0;
    });
    // Boolean CFPreferencesAppSynchronize(app)
    function("_CFPreferencesAppSynchronize", [preferences](CpuState& s, GuestMemory&) {
        auto& domain = preferences->domain(preferences->application_name(s.x[0]), false, false);
        std::lock_guard lock(preferences->mutex());
        s.x[0] = Preferences::synchronize(domain) ? 1 : 0;
    });

    // ---- NSUserDefaults ----
    // A volatile NSUserDefaults from another provider (the installer
    // frameworks' headless shell) is superseded: these methods are a superset
    // backed by the persistent CFPreferences domains, so every selector of
    // the class is replaced and no state is split between two stores.
    auto defaults_class = objc.find_class("NSUserDefaults");
    if (!defaults_class) defaults_class = objc.register_native_class("NSUserDefaults", objc.find_class("NSObject"), 16);
    for (const auto* symbol : {"_OBJC_CLASS_$_NSUserDefaults", "_OBJC_METACLASS_$_NSUserDefaults"}) {
        if (process.resolve(cf_library, symbol)) continue;
        const auto address = std::string_view(symbol).starts_with("_OBJC_CLASS") ? defaults_class : process.main_engine().read64(defaults_class);
        process.install_export(cf_library, symbol, {address, ExportKind::data});
    }
    const auto method = [&objc, defaults_class](const char* name, transmute::GuestCallHandler handler, bool class_method = false,
                                                const char* types = "@@:") {
        static_cast<void>(objc.add_native_method(defaults_class, name, std::move(handler), class_method, types));
    };
    const auto autorelease = [p](std::uint64_t object) { return object ? p.call(objc_library, "_objc_autorelease", {object}) : 0; };
    const auto state = [&objc](std::uint64_t object) { return objc.host_state<Defaults>(object); };
    // The value for key in the NSUserDefaults search list: arguments, the
    // application (or suite), added suites, the global domain, registration.
    const auto lookup = [preferences, state](std::uint64_t object, const std::string& key) -> std::optional<Plist> {
        const auto defaults = state(object);
        if (const auto* value = defaults->arguments.find(key)) return *value;
        std::vector<std::string> names{defaults->suite.empty() ? preferences->current_application() : defaults->suite};
        names.insert(names.end(), defaults->suites.begin(), defaults->suites.end());
        names.push_back(".GlobalPreferences");
        for (const auto& name : names) {
            for (const bool current_host : {true, false}) {
                auto& domain = preferences->domain(name, false, current_host);
                std::lock_guard lock(preferences->mutex());
                if (const auto* value = domain.values.find(key)) return *value;
            }
        }
        if (const auto* value = defaults->registration.find(key)) return *value;
        return std::nullopt;
    };
    const auto application_domain = [preferences, state](std::uint64_t object) -> Domain& {
        const auto defaults = state(object);
        return preferences->domain(defaults->suite.empty() ? preferences->current_application() : defaults->suite, false, false);
    };
    const auto store = [preferences, application_domain](std::uint64_t object, const std::string& key, std::optional<Plist> value) {
        auto& domain = application_domain(object);
        std::lock_guard lock(preferences->mutex());
        if (value) domain.values.set(key, std::move(*value));
        else domain.values.erase(key);
        domain.dirty = true;
    };
    // -[NSUserDefaults init] parses NSArgumentDomain from "-key value" pairs.
    const auto initialize = [&process, state](std::uint64_t object) {
        auto defaults = state(object);
        const auto& arguments = process.arguments;
        for (std::size_t index = 1; index + 1 < arguments.size(); ++index) {
            const auto& argument = arguments[index];
            if (argument.size() < 2 || argument[0] != '-' || argument[1] == '-') continue;
            const auto& text = arguments[index + 1];
            auto parsed = plist_parse(std::span(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
            Plist value;
            if (parsed) value = std::move(*parsed);
            else { value.kind = Plist::Kind::string; value.text = text; }
            defaults->arguments.set(argument.substr(1), std::move(value));
            ++index;
        }
    };
    auto standard = std::make_shared<std::pair<std::mutex, std::uint64_t>>();
    method("standardUserDefaults", [&objc, defaults_class, standard, initialize](CpuState& s, GuestMemory&) {
        std::lock_guard lock(standard->first);
        if (!standard->second) {
            standard->second = objc.allocate(defaults_class);
            initialize(standard->second);
        }
        s.x[0] = standard->second;
    }, true);
    method("resetStandardUserDefaults", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, true, "v@:");
    method("init", [initialize](CpuState& s, GuestMemory&) { initialize(s.x[0]); });
    method("initWithSuiteName:", [&cf, state, initialize](CpuState& s, GuestMemory&) {
        initialize(s.x[0]);
        if (s.x[2]) state(s.x[0])->suite = cf.utf8(s.x[2]);
    }, false, "@@:@");
    method("addSuiteNamed:", [&cf, state](CpuState& s, GuestMemory&) {
        if (s.x[2]) state(s.x[0])->suites.push_back(cf.utf8(s.x[2]));
        s.x[0] = 0;
    }, false, "v@:@");
    method("removeSuiteNamed:", [&cf, state](CpuState& s, GuestMemory&) {
        if (s.x[2]) { auto& list = state(s.x[0])->suites; list.erase(std::remove(list.begin(), list.end(), cf.utf8(s.x[2])), list.end()); }
        s.x[0] = 0;
    }, false, "v@:@");
    method("objectForKey:", [&process, &cf, lookup, autorelease](CpuState& s, GuestMemory&) {
        const auto value = s.x[2] ? lookup(s.x[0], cf.utf8(s.x[2])) : std::nullopt;
        s.x[0] = value ? autorelease(plist_to_guest(process, cf, *value)) : 0;
    }, false, "@@:@");
    const auto typed = [&process, &cf, lookup, autorelease](Plist::Kind kind) {
        return [&process, &cf, lookup, autorelease, kind](CpuState& s, GuestMemory&) {
            const auto value = s.x[2] ? lookup(s.x[0], cf.utf8(s.x[2])) : std::nullopt;
            s.x[0] = value && value->kind == kind ? autorelease(plist_to_guest(process, cf, *value)) : 0;
        };
    };
    method("arrayForKey:", typed(Plist::Kind::array), false, "@@:@");
    method("dictionaryForKey:", typed(Plist::Kind::dictionary), false, "@@:@");
    method("dataForKey:", typed(Plist::Kind::data), false, "@@:@");
    method("stringForKey:", [&process, &cf, lookup, autorelease](CpuState& s, GuestMemory&) {
        auto value = s.x[2] ? lookup(s.x[0], cf.utf8(s.x[2])) : std::nullopt;
        if (value && (value->kind == Plist::Kind::integer || value->kind == Plist::Kind::real)) {
            const auto text = value->kind == Plist::Kind::integer ? std::to_string(value->integer) : std::to_string(value->real);
            value = Plist{};
            value->text = text;
        }
        s.x[0] = value && value->kind == Plist::Kind::string ? autorelease(plist_to_guest(process, cf, *value)) : 0;
    }, false, "@@:@");
    method("stringArrayForKey:", [&process, &cf, lookup, autorelease](CpuState& s, GuestMemory&) {
        const auto value = s.x[2] ? lookup(s.x[0], cf.utf8(s.x[2])) : std::nullopt;
        const bool strings = value && value->kind == Plist::Kind::array &&
                             std::all_of(value->items.begin(), value->items.end(), [](const Plist& item) { return item.kind == Plist::Kind::string; });
        s.x[0] = strings ? autorelease(plist_to_guest(process, cf, *value)) : 0;
    }, false, "@@:@");
    const auto number_for = [&cf, lookup](CpuState& s) -> std::optional<double> {
        const auto value = s.x[2] ? lookup(s.x[0], cf.utf8(s.x[2])) : std::nullopt;
        return value ? numeric(*value) : std::nullopt;
    };
    method("integerForKey:", [number_for](CpuState& s, GuestMemory&) {
        s.x[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(number_for(s).value_or(0)));
    }, false, "q@:@");
    method("doubleForKey:", [number_for](CpuState& s, GuestMemory&) {
        s.v[0] = {std::bit_cast<std::uint64_t>(number_for(s).value_or(0)), 0};
    }, false, "d@:@");
    method("floatForKey:", [number_for](CpuState& s, GuestMemory&) {
        s.v[0] = {std::bit_cast<std::uint32_t>(static_cast<float>(number_for(s).value_or(0))), 0};
    }, false, "f@:@");
    method("boolForKey:", [&cf, lookup](CpuState& s, GuestMemory&) {
        const auto value = s.x[2] ? lookup(s.x[0], cf.utf8(s.x[2])) : std::nullopt;
        bool result = false;
        if (value && value->kind == Plist::Kind::string) {
            auto text = value->text;
            std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            result = text == "yes" || text == "y" || text == "true" || numeric(*value).value_or(0) != 0;
        } else if (value) result = numeric(*value).value_or(0) != 0;
        s.x[0] = result ? 1 : 0;
    }, false, "B@:@");
    method("setObject:forKey:", [&process, &cf, store](CpuState& s, GuestMemory&) {
        if (!s.x[3]) throw std::runtime_error("Alchemy NSUserDefaults: nil key");
        store(s.x[0], cf.utf8(s.x[3]), s.x[2] ? std::optional(plist_from_guest(process, cf, s.x[2])) : std::nullopt);
        s.x[0] = 0;
    }, false, "v@:@@");
    method("removeObjectForKey:", [&cf, store](CpuState& s, GuestMemory&) {
        if (s.x[2]) store(s.x[0], cf.utf8(s.x[2]), std::nullopt);
        s.x[0] = 0;
    }, false, "v@:@");
    method("setInteger:forKey:", [&cf, store](CpuState& s, GuestMemory&) {
        Plist value{Plist::Kind::integer};
        value.integer = static_cast<std::int64_t>(s.x[2]);
        store(s.x[0], cf.utf8(s.x[3]), value);
        s.x[0] = 0;
    }, false, "v@:q@");
    method("setBool:forKey:", [&cf, store](CpuState& s, GuestMemory&) {
        Plist value{Plist::Kind::boolean};
        value.boolean = (s.x[2] & 0xff) != 0;
        store(s.x[0], cf.utf8(s.x[3]), value);
        s.x[0] = 0;
    }, false, "v@:B@");
    method("setDouble:forKey:", [&cf, store](CpuState& s, GuestMemory&) {
        Plist value{Plist::Kind::real};
        value.real = std::bit_cast<double>(s.v[0][0]);
        store(s.x[0], cf.utf8(s.x[2]), value);
        s.x[0] = 0;
    }, false, "v@:d@");
    method("setFloat:forKey:", [&cf, store](CpuState& s, GuestMemory&) {
        Plist value{Plist::Kind::real};
        value.real = std::bit_cast<float>(static_cast<std::uint32_t>(s.v[0][0]));
        store(s.x[0], cf.utf8(s.x[2]), value);
        s.x[0] = 0;
    }, false, "v@:f@");
    method("registerDefaults:", [&process, &cf, state](CpuState& s, GuestMemory&) {
        if (s.x[2]) {
            const auto values = plist_from_guest(process, cf, s.x[2]);
            auto defaults = state(s.x[0]);
            for (const auto& [key, value] : values.entries) defaults->registration.set(key, value);
        }
        s.x[0] = 0;
    }, false, "v@:@");
    method("synchronize", [preferences, application_domain](CpuState& s, GuestMemory&) {
        auto& domain = application_domain(s.x[0]);
        std::lock_guard lock(preferences->mutex());
        s.x[0] = Preferences::synchronize(domain) ? 1 : 0;
    }, false, "B@:");
    method("objectIsForcedForKey:", [](CpuState& s, GuestMemory&) { s.x[0] = 0; }, false, "B@:@");   // no managed preferences
    method("dictionaryRepresentation", [&process, &cf, preferences, state, autorelease](CpuState& s, GuestMemory&) {
        const auto defaults = state(s.x[0]);
        Plist merged = defaults->registration;
        std::vector<std::string> names{".GlobalPreferences"};
        names.insert(names.end(), defaults->suites.rbegin(), defaults->suites.rend());
        names.push_back(defaults->suite.empty() ? preferences->current_application() : defaults->suite);
        for (const auto& name : names) {
            for (const bool current_host : {false, true}) {
                auto& domain = preferences->domain(name, false, current_host);
                std::lock_guard lock(preferences->mutex());
                for (const auto& [key, value] : domain.values.entries) merged.set(key, value);
            }
        }
        for (const auto& [key, value] : defaults->arguments.entries) merged.set(key, value);
        s.x[0] = autorelease(plist_to_guest(process, cf, merged));
    });
    method("persistentDomainForName:", [&process, &cf, preferences, autorelease](CpuState& s, GuestMemory&) {
        if (!s.x[2]) { s.x[0] = 0; return; }
        const auto name = cf.utf8(s.x[2]);
        auto& domain = preferences->domain(name, false, false);
        std::lock_guard lock(preferences->mutex());
        // The Dock's domain exists in every macOS account (the Dock writes it
        // at login); no Dock runs here, so it reads as empty, not absent.
        const bool always_present = name == "com.apple.dock";
        s.x[0] = domain.values.entries.empty() && !always_present ? 0 : autorelease(plist_to_guest(process, cf, domain.values));
    }, false, "@@:@");
    method("setPersistentDomain:forName:", [&process, &cf, preferences](CpuState& s, GuestMemory&) {
        auto value = plist_from_guest(process, cf, s.x[2]);
        auto& domain = preferences->domain(cf.utf8(s.x[3]), false, false);
        std::lock_guard lock(preferences->mutex());
        domain.values = std::move(value);
        domain.dirty = true;
        static_cast<void>(Preferences::synchronize(domain));
        s.x[0] = 0;
    }, false, "v@:@@");
    method("removePersistentDomainForName:", [&cf, preferences](CpuState& s, GuestMemory&) {
        auto& domain = preferences->domain(cf.utf8(s.x[2]), false, false);
        std::lock_guard lock(preferences->mutex());
        domain.values = Plist{Plist::Kind::dictionary};
        domain.dirty = true;
        static_cast<void>(Preferences::synchronize(domain));
        s.x[0] = 0;
    }, false, "v@:@");
}

} // namespace alchemy::export_fill
