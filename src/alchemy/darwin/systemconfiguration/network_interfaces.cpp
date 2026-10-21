#include "alchemy/gui/value_provider.hpp"

#include "alchemy/darwin/kit/export_fill/export_fill.hpp"

#include <cstdio>

// SystemConfiguration's network interfaces for Chrome's images, cut from
// darwin/coreservices/services.cpp (install_services calls it in place):
// SCNetworkInterfaceCopyAll lists the host adapters under their getifaddrs
// names with their MAC addresses.
namespace alchemy::gui::values {
namespace {

using fill::system_configuration;

} // namespace

void Provider::install_network_interfaces() {
    const auto interface_class = cls(system_configuration, "__SCNetworkInterface", "NSObject");
    object_properties(interface_class, {"name", "address"});
    raw(system_configuration, "_SCNetworkInterfaceCopyAll", [this, interface_class](CpuState& s, GuestMemory&) {
        std::vector<Address> objects;
        for (const auto& entry : export_fill::host_interfaces()) {
            if (entry.loopback) continue;   // SCNetworkInterfaceCopyAll lists no loopback
            const auto object = create(interface_class);
            const auto name = cf.string(entry.name);
            store(object, "name", name);
            release(name);
            if (entry.mac.size() == 6) {
                char text[18];
                std::snprintf(text, sizeof(text), "%02x:%02x:%02x:%02x:%02x:%02x", entry.mac[0], entry.mac[1], entry.mac[2],
                              entry.mac[3], entry.mac[4], entry.mac[5]);
                const auto address = cf.string(text);
                store(object, "address", address);
                release(address);
            }
            objects.push_back(object);
        }
        s.x[0] = cf.array(objects);
        for (const auto object : objects) release(object);
    });
    raw(system_configuration, "_SCNetworkInterfaceGetBSDName", [this](CpuState& s, GuestMemory&) { s.x[0] = s.x[0] ? stored(s.x[0], "name") : 0; });
    raw(system_configuration, "_SCNetworkInterfaceGetHardwareAddressString", [this](CpuState& s, GuestMemory&) {
        s.x[0] = s.x[0] ? stored(s.x[0], "address") : 0;
    });
}

} // namespace alchemy::gui::values
