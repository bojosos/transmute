#pragma once

#include "alchemy/host/net.hpp"

#include <string>
#include <vector>

namespace alchemy::libsystem {

// The Darwin names of the host's network interfaces, one rule for every libSystem interface that names them
// (getifaddrs in net/libsystem_net.cpp, if_nametoindex / if_indextoname / if_nameindex in net/sockets.cpp; the
// SystemConfiguration bridge follows getifaddrs): lo0 for the loopback, utun<n> for tunnels and point-to-point
// links, en<n> for the rest, numbered over the interfaces that are up and then those that are down, each in the
// host's order. The result is parallel to `adapters`.
inline std::vector<std::string> darwin_interface_names(const std::vector<host::net::Interface>& adapters) {
    std::vector<std::string> names(adapters.size());
    unsigned ethernet = 0, tunnel = 0;
    for (const bool up : {true, false}) {
        for (std::size_t index = 0; index < adapters.size(); ++index) {
            const auto& adapter = adapters[index];
            if (adapter.up != up) continue;
            if (adapter.kind == host::net::InterfaceKind::loopback) names[index] = "lo0";
            else if (adapter.kind == host::net::InterfaceKind::tunnel || adapter.kind == host::net::InterfaceKind::ppp)
                names[index] = "utun" + std::to_string(tunnel++);
            else names[index] = "en" + std::to_string(ethernet++);
        }
    }
    return names;
}

} // namespace alchemy::libsystem
