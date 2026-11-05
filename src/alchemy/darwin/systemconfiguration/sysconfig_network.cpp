#include "alchemy/darwin/network/network_internal.hpp"
#include "alchemy/darwin/systemconfiguration/system_configuration.hpp"

#include "alchemy/darwin/kit/export_fill/export_fill.hpp"
#include "alchemy/host/net.hpp"
#include "alchemy/host/system.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

// SystemConfiguration's network configuration, read-only, from the host's
// adapters (host::net):
//   - SCNetworkInterface: CopyAll lists the Ethernet, Wi-Fi, cellular and PPP
//     adapters (no loopback or tunnels) under their getifaddrs names, with the
//     MAC address, the interface type and the adapter's display name.
//   - SCPreferences: the "preferences.plist" session (prefsID NULL) answers a
//     configuration built from the adapters: NetworkServices (one service per
//     adapter, its Windows adapter GUID as the service ID, with Interface,
//     IPv4 (DHCP, or Manual with the current addresses), IPv6 and DNS
//     entities), one current set "Automatic" with every service and the
//     IPv4 service order (the primary adapter first), and System (computer
//     and local host names). Other prefsIDs read as empty files. Values can
//     be set in the session, but committing them needs configd's write
//     access, which an unprivileged process does not have: CommitChanges
//     with changes, and Lock, answer FALSE with kSCStatusAccessError.
//   - SCNetworkService / SCNetworkSet / SCNetworkProtocol over that session.
//   - SCCopyLastError: SCError() as a CFError in the SystemConfiguration domain.
namespace alchemy::gui::network {
namespace {

constexpr std::string_view lib = sysconfig::library;
constexpr std::string_view cf_library = alchemy::core_foundation_library;
constexpr std::string_view current_set = "A1C4E3D2-5B6F-4A70-8E91-3C2D1B0A9F87";   // the one set's ID

// A host adapter with the guest's interface name (getifaddrs).
struct Adapter {
    std::string name;
    host::net::Interface host;
};

std::vector<Adapter> adapters() {
    std::vector<Adapter> result;
    const auto host = host::net::interfaces();
    if (!host) return result;
    for (const auto& guest : export_fill::host_interfaces()) {
        if (guest.loopback) continue;
        const auto found = std::find_if(host->begin(), host->end(), [&](const auto& item) { return item.index == guest.index; });
        if (found == host->end()) continue;
        if (found->kind == host::net::InterfaceKind::tunnel || found->kind == host::net::InterfaceKind::loopback) continue;
        result.push_back({guest.name, *found});
    }
    return result;
}

// kSCNetworkInterfaceType* and the preferences' Hardware value.
const char* interface_type(host::net::InterfaceKind kind) {
    switch (kind) {
    case host::net::InterfaceKind::wireless: return "IEEE80211";
    case host::net::InterfaceKind::ppp: return "PPP";
    case host::net::InterfaceKind::cellular: return "WWAN";
    default: return "Ethernet";
    }
}
const char* hardware(host::net::InterfaceKind kind) {
    switch (kind) {
    case host::net::InterfaceKind::wireless: return "AirPort";
    case host::net::InterfaceKind::ppp: return "PPP";
    case host::net::InterfaceKind::cellular: return "WWAN";
    default: return "Ethernet";
    }
}
std::string display_name(const Adapter& adapter) {
    if (!adapter.host.display_name.empty()) return adapter.host.display_name;
    return adapter.host.kind == host::net::InterfaceKind::wireless ? "Wi-Fi" : "Ethernet";
}
std::string mac_text(const std::vector<std::uint8_t>& bytes) {
    if (bytes.size() != 6) return {};
    char text[18];
    std::snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5]);
    return text;
}
std::string mask_text(unsigned prefix) {
    const auto mask = prefix == 0 ? 0U : 0xffffffffU << (32 - std::min(prefix, 32U));
    return std::to_string(mask >> 24) + "." + std::to_string((mask >> 16) & 255) + "." + std::to_string((mask >> 8) & 255) + "." +
           std::to_string(mask & 255);
}

// ---- guest objects ------------------------------------------------------------------------------------------

struct InterfaceState final : ObjectPayload {
    Address name{}, address{}, type{}, display{};   // +1 strings (address 0 without a MAC)
};
struct PrefsState final : ObjectPayload {
    std::mutex mutex;
    Address values{};          // +1 CFMutableDictionary
    bool network{};            // the network configuration ("preferences.plist")
    bool changed{};
};
struct ServiceState final : ObjectPayload {
    Address prefs{}, id{};     // +1
    std::mutex mutex;
    Address interface{};       // +1, made on first GetInterface
};
struct SetState final : ObjectPayload {
    Address prefs{}, id{};     // +1
};
struct ProtocolState final : ObjectPayload {
    Address service{}, type{};   // +1
};

class Network : public std::enable_shared_from_this<Network> {
public:
    explicit Network(std::shared_ptr<Bridge> bridge) : b(std::move(bridge)) {}
    std::shared_ptr<Bridge> b;
    Address interface_class{}, prefs_class{}, service_class{}, set_class{}, protocol_class{};

    template <class T>
    std::shared_ptr<T> state(Address object, const char* what) {
        auto value = object ? b->objc.find_host_state<T>(object) : nullptr;
        if (!value) security::fail(std::string("expected an ") + what);
        return value;
    }
    template <class T>
    Address make(Address cls, std::shared_ptr<T> value) {
        const auto object = b->objc.allocate(cls);
        static_cast<void>(b->objc.host_state<T>(object, [&] { return value; }));
        return object;
    }

    // +1 CF values with their parts released.
    Address str(std::string_view text) { return b->string(text); }
    Address list(std::vector<Address> items) {
        const auto value = b->array(items);
        b->release_all(items);
        return value;
    }
    Address strings(const std::vector<std::string>& items) {
        std::vector<Address> values;
        for (const auto& item : items) values.push_back(str(item));
        return list(std::move(values));
    }
    Address dict(std::vector<std::pair<std::string, Address>> entries) {
        std::vector<std::pair<Address, Address>> pairs;
        std::vector<Address> owned;
        for (auto& [key, value] : entries) {
            pairs.emplace_back(str(key), value);
            owned.push_back(pairs.back().first);
            owned.push_back(value);
        }
        const auto result = b->dictionary(pairs);
        b->release_all(owned);
        return result;
    }
    // CFDictionaryGetValue by a host key (no retain); 0 when `dictionary` is not one.
    Address get(Address dictionary, std::string_view key) {
        if (!dictionary || !b->is_dictionary(dictionary)) return 0;
        const auto name = str(key);
        const auto value = b->lookup(dictionary, name);
        b->release(name);
        return value;
    }

    Address make_interface(const Adapter& adapter) {
        auto value = std::make_shared<InterfaceState>();
        value->name = str(adapter.name);
        if (const auto text = mac_text(adapter.host.hardware); !text.empty()) value->address = str(text);
        value->type = str(interface_type(adapter.host.kind));
        value->display = str(display_name(adapter));
        return make(interface_class, value);
    }

    // The network configuration as a mutable dictionary (+1).
    Address network_preferences() {
        const auto all = adapters();
        const auto primary = host::net::best_interface(host::net::Address::ipv4({8, 8, 8, 8}));
        std::vector<std::pair<std::string, Address>> services, links;
        std::vector<std::string> order;
        for (const auto& adapter : all) {
            const auto id = sysconfig::service_id(adapter.host.id.empty() ? adapter.name : adapter.host.id);
            std::vector<std::pair<std::string, Address>> ipv4;
            if (adapter.host.dhcp) {
                ipv4.emplace_back("ConfigMethod", str("DHCP"));
            } else {
                std::vector<std::string> addresses, masks;
                for (const auto& assigned : adapter.host.addresses)
                    if (assigned.address.family == host::net::Family::ipv4) {
                        addresses.push_back(host::net::host_text(assigned.address));
                        masks.push_back(mask_text(assigned.prefix_length));
                    }
                ipv4.emplace_back("ConfigMethod", str("Manual"));
                if (!addresses.empty()) { ipv4.emplace_back("Addresses", strings(addresses)); ipv4.emplace_back("SubnetMasks", strings(masks)); }
                for (const auto& gateway : adapter.host.gateways)
                    if (gateway.family == host::net::Family::ipv4) { ipv4.emplace_back("Router", str(host::net::host_text(gateway))); break; }
            }
            const auto interface = dict({{"DeviceName", str(adapter.name)}, {"Hardware", str(hardware(adapter.host.kind))},
                                         {"Type", str(interface_type(adapter.host.kind))}, {"UserDefinedName", str(display_name(adapter))}});
            services.emplace_back(id, dict({{"UserDefinedName", str(display_name(adapter))}, {"Interface", interface},
                                            {"IPv4", dict(std::move(ipv4))}, {"IPv6", dict({{"ConfigMethod", str("Automatic")}})},
                                            {"DNS", dict({})}}));
            links.emplace_back(id, dict({{"__LINK__", str("/NetworkServices/" + id)}}));
            if (primary && *primary == adapter.host.index) order.insert(order.begin(), id);
            else order.push_back(id);
        }
        const auto name = host::system::host_name().value_or({});
        const auto set = dict({{"UserDefinedName", str("Automatic")},
                               {"Network", dict({{"Service", dict(std::move(links))},
                                                 {"Global", dict({{"IPv4", dict({{"ServiceOrder", strings(order)}})}})}})}});
        const auto system = dict({{"System", dict({{"ComputerName", str(name)}, {"ComputerNameEncoding", b->number(0x08000100)}})},
                                  {"Network", dict({{"HostNames", dict({{"LocalHostName", str(name)}})}})}});
        const auto immutable = dict({{"NetworkServices", dict(std::move(services))},
                                     {"Sets", dict({{std::string(current_set), set}})},
                                     {"CurrentSet", str("/Sets/" + std::string(current_set))}, {"System", system}});
        const auto value = mutable_copy(immutable);
        b->release(immutable);
        return value;
    }
    // A mutable copy of the top level (SetValue / RemoveValue change top-level keys only).
    Address mutable_copy(Address value) { return b->call(cf_library, "_CFDictionaryCreateMutableCopy", {0, 0, value}); }
    void reload(PrefsState& value) {
        Address fresh{};
        if (value.network) {
            fresh = network_preferences();
        } else {   // a file that does not exist: no keys
            const auto empty = dict({});
            fresh = mutable_copy(empty);
            b->release(empty);
        }
        const auto previous = std::exchange(value.values, fresh);
        value.changed = false;
        b->release(previous);
    }

    // SCPreferencesPathGetValue: "/a/b" through nested dictionaries, following __LINK__ paths.
    Address path_value(Address values, std::string_view path, int depth = 0) {
        if (path.empty() || path[0] != '/' || depth > 8) return 0;
        Address node = values;
        std::size_t at = 1;
        while (at < path.size()) {
            const auto next = path.find('/', at);
            const auto part = path.substr(at, next == std::string_view::npos ? std::string_view::npos : next - at);
            if (!part.empty()) {
                node = get(node, part);
                if (!node) return 0;
                if (const auto link = get(node, "__LINK__"))
                    if (const auto target = b->maybe_text(link)) node = path_value(values, *target, depth + 1);
                if (!node) return 0;
            }
            if (next == std::string_view::npos) break;
            at = next + 1;
        }
        return node;
    }
    Address prefs_values(Address prefs) {
        const auto value = state<PrefsState>(prefs, "SCPreferencesRef");
        std::lock_guard lock(value->mutex);
        return value->values;
    }
    // The service's dictionary in its session (no retain).
    Address service_entity(const ServiceState& service) {
        return path_value(prefs_values(service.prefs), "/NetworkServices/" + b->text(service.id));
    }
    Address make_service(Address prefs, std::string_view id) {
        auto value = std::make_shared<ServiceState>();
        value->prefs = b->retain(prefs);
        value->id = str(id);
        return make(service_class, value);
    }
};

void fail_with(CpuState& s, std::int32_t status) {
    sysconfig::set_last_status(status);
    s.x[0] = 0;
}

} // namespace

void install_system_configuration_network(const std::shared_ptr<Bridge>& b) {
    const auto self = std::make_shared<Network>(b);
    static std::shared_ptr<Network> keep;
    keep = self;
    const auto install = [b](std::string name, transmute::GuestCallHandler handler) {
        if (!b->exported(lib, name)) b->function(lib, name, std::move(handler));
    };
    const auto constant = [b](std::string_view name, std::string_view value) {
        const auto symbol = "_" + std::string(name);
        if (!b->exported(lib, symbol)) static_cast<void>(b->export_string(lib, symbol, value));
    };
    for (const auto& [name, value] : std::initializer_list<std::pair<const char*, const char*>>{
             {"kSCNetworkInterfaceTypeEthernet", "Ethernet"}, {"kSCNetworkInterfaceTypeIEEE80211", "IEEE80211"},
             {"kSCNetworkInterfaceTypePPP", "PPP"}, {"kSCNetworkInterfaceTypeWWAN", "WWAN"}, {"kSCNetworkInterfaceTypeBond", "Bond"},
             {"kSCNetworkInterfaceTypeVLAN", "VLAN"}, {"kSCNetworkInterfaceTypeBluetooth", "Bluetooth"},
             {"kSCNetworkInterfaceTypeModem", "Modem"}, {"kSCNetworkInterfaceTypeFireWire", "FireWire"},
             {"kSCNetworkInterfaceTypeSerial", "Serial"}, {"kSCNetworkInterfaceTypeIrDA", "IrDA"}, {"kSCNetworkInterfaceType6to4", "6to4"},
             {"kSCNetworkInterfaceTypeL2TP", "L2TP"}, {"kSCNetworkInterfaceTypePPTP", "PPTP"}, {"kSCNetworkInterfaceTypeIPSec", "IPSec"},
             {"kSCNetworkInterfaceTypeIPv4", "IPv4"}, {"kSCNetworkProtocolTypeIPv4", "IPv4"}, {"kSCNetworkProtocolTypeIPv6", "IPv6"},
             {"kSCNetworkProtocolTypeDNS", "DNS"}, {"kSCNetworkProtocolTypeProxies", "Proxies"},
             {"kSCNetworkProtocolTypeSMB", "SMB"}, {"kSCPrefNetworkServices", "NetworkServices"}, {"kSCPrefSets", "Sets"},
             {"kSCPrefCurrentSet", "CurrentSet"}, {"kSCPrefSystem", "System"}, {"kSCPropUserDefinedName", "UserDefinedName"},
             {"kSCPropNetInterfaceDeviceName", "DeviceName"}, {"kSCPropNetInterfaceType", "Type"},
             {"kSCPropNetInterfaceHardware", "Hardware"}, {"kSCPropNetIPv4ConfigMethod", "ConfigMethod"},
             {"kSCPropNetIPv6ConfigMethod", "ConfigMethod"}, {"kSCValNetIPv4ConfigMethodDHCP", "DHCP"},
             {"kSCValNetIPv4ConfigMethodManual", "Manual"}, {"kSCValNetIPv6ConfigMethodAutomatic", "Automatic"},
             {"kCFErrorDomainSystemConfiguration", "com.apple.SystemConfiguration"}})
        constant(name, value);

    const auto define = [&](const char* name, security::TypeId id) {
        const auto cls = b->define_class(name, b->object_class, lib);
        b->register_type_id(cls, id);
        return cls;
    };
    self->interface_class = define("_AlchemySCNetworkInterface", security::TypeId::sc_network_interface);
    self->prefs_class = define("_AlchemySCPreferences", security::TypeId::sc_preferences);
    self->service_class = define("_AlchemySCNetworkService", security::TypeId::sc_network_service);
    self->set_class = define("_AlchemySCNetworkSet", security::TypeId::sc_network_set);
    self->protocol_class = define("_AlchemySCNetworkProtocol", security::TypeId::sc_network_protocol);
    for (const auto& [name, id] : std::initializer_list<std::pair<const char*, security::TypeId>>{
             {"_SCNetworkInterfaceGetTypeID", security::TypeId::sc_network_interface}, {"_SCPreferencesGetTypeID", security::TypeId::sc_preferences},
             {"_SCNetworkServiceGetTypeID", security::TypeId::sc_network_service}, {"_SCNetworkSetGetTypeID", security::TypeId::sc_network_set},
             {"_SCNetworkProtocolGetTypeID", security::TypeId::sc_network_protocol}})
        install(name, [type = id](CpuState& s, GuestMemory&) { s.x[0] = static_cast<Address>(type); });

    // dealloc: each payload's +1 references.
    const auto on_dealloc = [self](Address cls, std::function<std::vector<Address>(Address)> owned) {
        self->b->method(cls, "dealloc", [self, cls, owned](CpuState& s, GuestMemory&) {
            self->b->release_all(owned(s.x[0]));
            static_cast<void>(self->b->objc.send_super(s.x[0], cls, self->b->objc.selector("dealloc")));
        }, false, "v@:");
    };
    on_dealloc(self->interface_class, [self](Address object) {
        const auto value = self->b->objc.find_host_state<InterfaceState>(object);
        return value ? std::vector<Address>{std::exchange(value->name, 0), std::exchange(value->address, 0), std::exchange(value->type, 0),
                                            std::exchange(value->display, 0)} : std::vector<Address>{};
    });
    on_dealloc(self->prefs_class, [self](Address object) {
        const auto value = self->b->objc.find_host_state<PrefsState>(object);
        if (!value) return std::vector<Address>{};
        std::lock_guard lock(value->mutex);
        return std::vector<Address>{std::exchange(value->values, 0)};
    });
    on_dealloc(self->service_class, [self](Address object) {
        const auto value = self->b->objc.find_host_state<ServiceState>(object);
        if (!value) return std::vector<Address>{};
        std::lock_guard lock(value->mutex);
        return std::vector<Address>{std::exchange(value->interface, 0), std::exchange(value->id, 0), std::exchange(value->prefs, 0)};
    });
    on_dealloc(self->set_class, [self](Address object) {
        const auto value = self->b->objc.find_host_state<SetState>(object);
        return value ? std::vector<Address>{std::exchange(value->id, 0), std::exchange(value->prefs, 0)} : std::vector<Address>{};
    });
    on_dealloc(self->protocol_class, [self](Address object) {
        const auto value = self->b->objc.find_host_state<ProtocolState>(object);
        return value ? std::vector<Address>{std::exchange(value->type, 0), std::exchange(value->service, 0)} : std::vector<Address>{};
    });

    // ---- SCNetworkInterface ------------------------------------------------------------------------------
    install("_SCNetworkInterfaceCopyAll", [self](CpuState& s, GuestMemory&) {
        std::vector<Address> objects;
        for (const auto& adapter : adapters()) objects.push_back(self->make_interface(adapter));
        s.x[0] = self->list(std::move(objects));
    });
    const auto interface_field = [self](Address InterfaceState::* field) {
        return [self, field](CpuState& s, GuestMemory&) {
            const auto value = s.x[0] ? self->b->objc.find_host_state<InterfaceState>(s.x[0]) : nullptr;
            s.x[0] = value ? (*value).*field : 0;
        };
    };
    install("_SCNetworkInterfaceGetBSDName", interface_field(&InterfaceState::name));
    install("_SCNetworkInterfaceGetHardwareAddressString", interface_field(&InterfaceState::address));
    install("_SCNetworkInterfaceGetInterfaceType", interface_field(&InterfaceState::type));
    install("_SCNetworkInterfaceGetLocalizedDisplayName", interface_field(&InterfaceState::display));
    // The interface types that can be layered on it: none are configurable here (NULL).
    install("_SCNetworkInterfaceGetSupportedInterfaceTypes", [](CpuState& s, GuestMemory&) { s.x[0] = 0; });

    // ---- SCPreferences -----------------------------------------------------------------------------------
    // SCPreferencesRef SCPreferencesCreate(CFAllocatorRef, CFStringRef name, CFStringRef prefsID)
    const auto create = [self](CpuState& s, GuestMemory&) {
        auto value = std::make_shared<PrefsState>();
        const auto id = s.x[2] ? self->b->maybe_text(s.x[2]) : std::optional<std::string>{};
        value->network = !id || *id == "preferences.plist" || id->ends_with("/SystemConfiguration/preferences.plist");
        self->reload(*value);
        s.x[0] = self->make(self->prefs_class, value);
    };
    install("_SCPreferencesCreate", create);
    install("_SCPreferencesCreateWithAuthorization", create);   // (allocator, name, prefsID, authorization)
    install("_SCPreferencesGetValue", [self](CpuState& s, GuestMemory&) {
        const auto values = self->prefs_values(s.x[0]);
        const auto value = s.x[1] ? self->b->lookup(values, s.x[1]) : 0;
        if (!value) sysconfig::set_last_status(sysconfig::status_no_key);
        s.x[0] = value;
    });
    install("_SCPreferencesPathGetValue", [self](CpuState& s, GuestMemory&) {
        const auto path = self->b->maybe_text(s.x[1]);
        const auto value = path ? self->path_value(self->prefs_values(s.x[0]), *path) : 0;
        if (!value || !self->b->is_dictionary(value)) { fail_with(s, sysconfig::status_no_key); return; }
        s.x[0] = value;
    });
    install("_SCPreferencesCopyKeyList", [self](CpuState& s, GuestMemory&) {
        std::vector<Address> keys;
        for (const auto& [key, value] : self->b->cf.dictionary_entries(self->prefs_values(s.x[0]))) keys.push_back(self->b->retain(key));
        s.x[0] = self->list(std::move(keys));
    });
    install("_SCPreferencesSetValue", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<PrefsState>(s.x[0], "SCPreferencesRef");
        if (!s.x[1] || !s.x[2] || !self->b->is_string(s.x[1])) { fail_with(s, sysconfig::status_invalid_argument); return; }
        std::lock_guard lock(value->mutex);
        static_cast<void>(self->b->call(cf_library, "_CFDictionarySetValue", {value->values, s.x[1], s.x[2]}));
        value->changed = true;
        s.x[0] = 1;
    });
    install("_SCPreferencesRemoveValue", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<PrefsState>(s.x[0], "SCPreferencesRef");
        std::lock_guard lock(value->mutex);
        if (!s.x[1] || !self->b->lookup(value->values, s.x[1])) { fail_with(s, sysconfig::status_no_key); return; }
        static_cast<void>(self->b->call(cf_library, "_CFDictionaryRemoveValue", {value->values, s.x[1]}));
        value->changed = true;
        s.x[0] = 1;
    });
    install("_SCPreferencesCommitChanges", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<PrefsState>(s.x[0], "SCPreferencesRef");
        bool changed{};
        { std::lock_guard lock(value->mutex); changed = value->changed; }
        if (changed) { fail_with(s, sysconfig::status_access_error); return; }   // writing the configuration needs root
        s.x[0] = 1;
    });
    install("_SCPreferencesApplyChanges", [self](CpuState& s, GuestMemory&) {
        static_cast<void>(self->state<PrefsState>(s.x[0], "SCPreferencesRef"));
        s.x[0] = 1;   // nothing was committed, so nothing is pending
    });
    install("_SCPreferencesLock", [self](CpuState& s, GuestMemory&) {
        static_cast<void>(self->state<PrefsState>(s.x[0], "SCPreferencesRef"));
        fail_with(s, sysconfig::status_access_error);
    });
    install("_SCPreferencesUnlock", [self](CpuState& s, GuestMemory&) {
        static_cast<void>(self->state<PrefsState>(s.x[0], "SCPreferencesRef"));
        fail_with(s, sysconfig::status_need_lock);   // never locked
    });
    install("_SCPreferencesSynchronize", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<PrefsState>(s.x[0], "SCPreferencesRef");
        std::lock_guard lock(value->mutex);
        self->reload(*value);
    });

    // ---- SCNetworkService / SCNetworkSet / SCNetworkProtocol ----------------------------------------------
    install("_SCNetworkServiceCopyAll", [self](CpuState& s, GuestMemory&) {
        const auto services = self->get(self->prefs_values(s.x[0]), "NetworkServices");
        std::vector<Address> objects;
        for (const auto& [key, value] : self->b->cf.dictionary_entries(services))
            if (const auto id = self->b->maybe_text(key)) objects.push_back(self->make_service(s.x[0], *id));
        s.x[0] = self->list(std::move(objects));
    });
    install("_SCNetworkServiceGetServiceID", [self](CpuState& s, GuestMemory&) { s.x[0] = self->state<ServiceState>(s.x[0], "SCNetworkServiceRef")->id; });
    install("_SCNetworkServiceGetName", [self](CpuState& s, GuestMemory&) {
        s.x[0] = self->get(self->service_entity(*self->state<ServiceState>(s.x[0], "SCNetworkServiceRef")), "UserDefinedName");
    });
    install("_SCNetworkServiceGetEnabled", [self](CpuState& s, GuestMemory&) {
        const auto entity = self->service_entity(*self->state<ServiceState>(s.x[0], "SCNetworkServiceRef"));
        s.x[0] = entity && !self->get(entity, "__INACTIVE__") ? 1 : 0;
    });
    install("_SCNetworkServiceGetInterface", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<ServiceState>(s.x[0], "SCNetworkServiceRef");
        {
            std::lock_guard lock(value->mutex);
            if (value->interface) { s.x[0] = value->interface; return; }
        }
        const auto name = self->b->maybe_text(self->get(self->get(self->service_entity(*value), "Interface"), "DeviceName"));
        Address object{};
        if (name)
            for (const auto& adapter : adapters())
                if (adapter.name == *name) { object = self->make_interface(adapter); break; }
        std::lock_guard lock(value->mutex);
        if (value->interface) self->b->release(object);
        else value->interface = object;
        s.x[0] = value->interface;
    });
    // SCNetworkProtocolRef SCNetworkServiceCopyProtocol(SCNetworkServiceRef, CFStringRef protocolType)
    install("_SCNetworkServiceCopyProtocol", [self](CpuState& s, GuestMemory&) {
        const auto service = self->state<ServiceState>(s.x[0], "SCNetworkServiceRef");
        const auto type = self->b->maybe_text(s.x[1]);
        if (!type || !self->get(self->service_entity(*service), *type)) { fail_with(s, sysconfig::status_no_key); return; }
        auto value = std::make_shared<ProtocolState>();
        value->service = self->b->retain(s.x[0]);
        value->type = self->str(*type);
        s.x[0] = self->make(self->protocol_class, value);
    });
    install("_SCNetworkProtocolGetProtocolType", [self](CpuState& s, GuestMemory&) { s.x[0] = self->state<ProtocolState>(s.x[0], "SCNetworkProtocolRef")->type; });
    install("_SCNetworkProtocolGetEnabled", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<ProtocolState>(s.x[0], "SCNetworkProtocolRef");
        const auto entity = self->get(self->service_entity(*self->state<ServiceState>(value->service, "SCNetworkServiceRef")), self->b->text(value->type));
        s.x[0] = entity && !self->get(entity, "__INACTIVE__") ? 1 : 0;
    });
    install("_SCNetworkProtocolGetConfiguration", [self](CpuState& s, GuestMemory&) {
        const auto value = self->state<ProtocolState>(s.x[0], "SCNetworkProtocolRef");
        s.x[0] = self->get(self->service_entity(*self->state<ServiceState>(value->service, "SCNetworkServiceRef")), self->b->text(value->type));
    });
    install("_SCNetworkSetCopyCurrent", [self](CpuState& s, GuestMemory&) {
        const auto values = self->prefs_values(s.x[0]);
        const auto path = self->b->maybe_text(self->get(values, "CurrentSet"));
        if (!path || !path->starts_with("/Sets/") || !self->path_value(values, *path)) { fail_with(s, sysconfig::status_no_key); return; }
        auto value = std::make_shared<SetState>();
        value->prefs = self->b->retain(s.x[0]);
        value->id = self->str(path->substr(6));
        s.x[0] = self->make(self->set_class, value);
    });
    const auto set_entity = [self](const SetState& set) { return self->path_value(self->prefs_values(set.prefs), "/Sets/" + self->b->text(set.id)); };
    install("_SCNetworkSetGetSetID", [self](CpuState& s, GuestMemory&) { s.x[0] = self->state<SetState>(s.x[0], "SCNetworkSetRef")->id; });
    install("_SCNetworkSetGetName", [self, set_entity](CpuState& s, GuestMemory&) {
        s.x[0] = self->get(set_entity(*self->state<SetState>(s.x[0], "SCNetworkSetRef")), "UserDefinedName");
    });
    install("_SCNetworkSetCopyServices", [self, set_entity](CpuState& s, GuestMemory&) {
        const auto set = self->state<SetState>(s.x[0], "SCNetworkSetRef");
        std::vector<Address> objects;
        for (const auto& [key, value] : self->b->cf.dictionary_entries(self->get(self->get(set_entity(*set), "Network"), "Service")))
            if (const auto id = self->b->maybe_text(key); id && self->path_value(self->prefs_values(set->prefs), "/NetworkServices/" + *id))
                objects.push_back(self->make_service(set->prefs, *id));
        s.x[0] = self->list(std::move(objects));
    });

    // CFErrorRef SCCopyLastError(void)
    install("_SCCopyLastError", [self](CpuState& s, GuestMemory&) {
        const auto status = sysconfig::last_status();
        s.x[0] = self->b->error("com.apple.SystemConfiguration", status, status ? "SystemConfiguration error" : "Success!");
    });
}

} // namespace alchemy::gui::network
