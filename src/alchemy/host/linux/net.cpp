// Linux host backend, area "net": BSD sockets.
//
// Sockets are descriptors (Handle = descriptor + 1), non-blocking and
// close-on-exec. export_socket hands a socket to this same process only: a
// descriptor for another process has to travel over a UNIX socket
// (SCM_RIGHTS), which the macOS layer's process launcher does not do yet.
#include "alchemy/host/linux/linux.hpp"
#include "alchemy/host/net.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>

namespace alchemy::host::net {

namespace {

using linux_backend::fd_of;
using linux_backend::handle_of;
using linux_backend::last_error;

int native_family(Family family) {
    switch (family) {
    case Family::ipv4: return AF_INET;
    case Family::ipv6: return AF_INET6;
    case Family::local: return AF_UNIX;
    default: return AF_UNSPEC;
    }
}

int native_type(SocketType type) {
    switch (type) {
    case SocketType::stream: return SOCK_STREAM;
    case SocketType::datagram: return SOCK_DGRAM;
    default: return 0;
    }
}

SocketType socket_type_of(int type) {
    return type == SOCK_STREAM ? SocketType::stream : type == SOCK_DGRAM ? SocketType::datagram : SocketType::unspecified;
}

socklen_t to_native(const Address& address, sockaddr_storage& out) {
    std::memset(&out, 0, sizeof(out));
    switch (address.family) {
    case Family::ipv4: {
        auto& in = reinterpret_cast<sockaddr_in&>(out);
        in.sin_family = AF_INET;
        in.sin_port = htons(address.port);
        std::memcpy(&in.sin_addr, address.bytes.data(), 4);
        return sizeof(in);
    }
    case Family::ipv6: {
        auto& in6 = reinterpret_cast<sockaddr_in6&>(out);
        in6.sin6_family = AF_INET6;
        in6.sin6_port = htons(address.port);
        in6.sin6_flowinfo = htonl(address.flow_info);
        in6.sin6_scope_id = address.scope_id;
        std::memcpy(&in6.sin6_addr, address.bytes.data(), 16);
        return sizeof(in6);
    }
    case Family::local: {
        auto& un = reinterpret_cast<sockaddr_un&>(out);
        un.sun_family = AF_UNIX;
        const auto length = std::min(address.path.size(), sizeof(un.sun_path) - 1);
        std::memcpy(un.sun_path, address.path.data(), length);
        return static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + length + (length ? 1 : 0));
    }
    default: return 0;
    }
}

Address from_native(const sockaddr* address, socklen_t length) {
    Address result;
    if (!address) return result;
    switch (address->sa_family) {
    case AF_INET: {
        const auto* in = reinterpret_cast<const sockaddr_in*>(address);
        result.family = Family::ipv4;
        result.port = ntohs(in->sin_port);
        std::memcpy(result.bytes.data(), &in->sin_addr, 4);
        break;
    }
    case AF_INET6: {
        const auto* in6 = reinterpret_cast<const sockaddr_in6*>(address);
        result.family = Family::ipv6;
        result.port = ntohs(in6->sin6_port);
        result.flow_info = ntohl(in6->sin6_flowinfo);
        result.scope_id = in6->sin6_scope_id;
        std::memcpy(result.bytes.data(), &in6->sin6_addr, 16);
        break;
    }
    case AF_UNIX: {
        const auto* un = reinterpret_cast<const sockaddr_un*>(address);
        result.family = Family::local;
        const auto bytes = length > offsetof(sockaddr_un, sun_path) ? length - offsetof(sockaddr_un, sun_path) : 0;
        result.path.assign(un->sun_path, strnlen(un->sun_path, std::min<std::size_t>(bytes, sizeof(un->sun_path))));
        break;
    }
    default: break;
    }
    return result;
}

int native_flags(std::uint32_t flags) {
    int result = MSG_NOSIGNAL;
    if (flags & message_out_of_band) result |= MSG_OOB;
    if (flags & message_peek) result |= MSG_PEEK;
    if (flags & message_dont_route) result |= MSG_DONTROUTE;
    return result;
}

struct NativeOption {
    int level, name;
};
std::optional<NativeOption> native_option(Option option) {
    switch (option) {
    case Option::debug: return NativeOption{SOL_SOCKET, SO_DEBUG};
    case Option::accepting: return NativeOption{SOL_SOCKET, SO_ACCEPTCONN};
    case Option::reuse_address: return NativeOption{SOL_SOCKET, SO_REUSEADDR};
    case Option::keep_alive: return NativeOption{SOL_SOCKET, SO_KEEPALIVE};
    case Option::dont_route: return NativeOption{SOL_SOCKET, SO_DONTROUTE};
    case Option::broadcast: return NativeOption{SOL_SOCKET, SO_BROADCAST};
    case Option::out_of_band_inline: return NativeOption{SOL_SOCKET, SO_OOBINLINE};
    case Option::send_buffer: return NativeOption{SOL_SOCKET, SO_SNDBUF};
    case Option::receive_buffer: return NativeOption{SOL_SOCKET, SO_RCVBUF};
    case Option::tcp_no_delay: return NativeOption{IPPROTO_TCP, TCP_NODELAY};
    case Option::tcp_keep_idle: return NativeOption{IPPROTO_TCP, TCP_KEEPIDLE};
    case Option::tcp_keep_interval: return NativeOption{IPPROTO_TCP, TCP_KEEPINTVL};
    case Option::tcp_keep_count: return NativeOption{IPPROTO_TCP, TCP_KEEPCNT};
    case Option::ip_type_of_service: return NativeOption{IPPROTO_IP, IP_TOS};
    case Option::ip_time_to_live: return NativeOption{IPPROTO_IP, IP_TTL};
    case Option::ipv6_only: return NativeOption{IPPROTO_IPV6, IPV6_V6ONLY};
    }
    return std::nullopt;
}

Error resolver_error(int code) {
    switch (code) {
    case EAI_NONAME: return Error::not_found;
    case EAI_AGAIN: return Error::would_block;
    case EAI_NODATA: return Error::address_not_available;
    case EAI_BADFLAGS: return Error::invalid_argument;
    case EAI_FAMILY: return Error::address_family;
    case EAI_SOCKTYPE: return Error::socket_type;
    case EAI_SERVICE: return Error::protocol_not_supported;
    case EAI_MEMORY: return Error::no_memory;
    case EAI_FAIL: return Error::io;
    case EAI_SYSTEM: return last_error();
    default: return Error::other;
    }
}

std::string read_line(const std::string& path) {
    std::ifstream file(path);
    std::string line;
    std::getline(file, line);
    return line;
}

std::uint16_t poll_events(std::uint16_t events) {
    std::uint16_t native = 0;
    if (events & ready_readable) native |= POLLIN;
    if (events & ready_writable) native |= POLLOUT;
    return native;
}

std::uint16_t ready_bits(short revents) {
    std::uint16_t bits = 0;
    if (revents & POLLIN) bits |= ready_readable;
    if (revents & POLLOUT) bits |= ready_writable;
    if (revents & POLLERR) bits |= ready_error;
    if (revents & POLLHUP) bits |= ready_hang_up;
    if (revents & POLLNVAL) bits |= ready_invalid;
    return bits;
}

} // namespace

Status initialize() { return {}; }

Result<std::vector<Interface>> interfaces() {
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return last_error();
    std::map<std::string, Interface> by_name;
    std::vector<std::string> order;
    for (auto* entry = list; entry; entry = entry->ifa_next) {
        const std::string name = entry->ifa_name;
        auto [it, added] = by_name.try_emplace(name);
        Interface& item = it->second;
        if (added) {
            order.push_back(name);
            item.index = if_nametoindex(entry->ifa_name);
            item.ipv6_index = item.index;
            item.id = name;
            item.display_name = name;
            item.up = (entry->ifa_flags & IFF_UP) != 0;
            item.multicast = (entry->ifa_flags & IFF_MULTICAST) != 0;
            const std::string sys = "/sys/class/net/" + name + "/";
            item.mtu = static_cast<std::uint32_t>(std::strtoul(read_line(sys + "mtu").c_str(), nullptr, 10));
            const auto speed = std::strtoll(read_line(sys + "speed").c_str(), nullptr, 10);
            if (speed > 0) item.speed = static_cast<std::uint64_t>(speed) * 1000000u;
            if (entry->ifa_flags & IFF_LOOPBACK) item.kind = InterfaceKind::loopback;
            else if (std::ifstream(sys + "wireless").good() || std::ifstream(sys + "phy80211").good()) item.kind = InterfaceKind::wireless;
            else if (entry->ifa_flags & IFF_POINTOPOINT) item.kind = name.rfind("ppp", 0) == 0 ? InterfaceKind::ppp : InterfaceKind::tunnel;
            else if (read_line(sys + "type") == "1") item.kind = InterfaceKind::ethernet;
            std::istringstream mac(read_line(sys + "address"));
            std::string part;
            while (std::getline(mac, part, ':'))
                if (!part.empty()) item.hardware.push_back(static_cast<std::uint8_t>(std::strtoul(part.c_str(), nullptr, 16)));
            if (std::all_of(item.hardware.begin(), item.hardware.end(), [](std::uint8_t b) { return b == 0; })) item.hardware.clear();
        }
        if (!entry->ifa_addr || (entry->ifa_addr->sa_family != AF_INET && entry->ifa_addr->sa_family != AF_INET6)) continue;
        Interface::Assigned assigned;
        assigned.address = from_native(entry->ifa_addr, sizeof(sockaddr_storage));
        assigned.address.port = 0;
        if (entry->ifa_netmask) {
            const auto mask = from_native(entry->ifa_netmask, sizeof(sockaddr_storage));
            const std::size_t bytes = mask.family == Family::ipv4 ? 4 : 16;
            for (std::size_t i = 0; i < bytes; ++i) assigned.prefix_length += static_cast<std::uint8_t>(__builtin_popcount(mask.bytes[i]));
        }
        item.addresses.push_back(std::move(assigned));
    }
    freeifaddrs(list);
    // Default gateways from the kernel's IPv4 routing table.
    std::ifstream routes("/proc/net/route");
    std::string line;
    std::getline(routes, line);
    while (std::getline(routes, line)) {
        char name[64];
        unsigned destination = 0, gateway = 0;
        if (std::sscanf(line.c_str(), "%63s %x %x", name, &destination, &gateway) == 3 && destination == 0 && gateway != 0) {
            if (auto it = by_name.find(name); it != by_name.end()) {
                std::array<std::uint8_t, 4> bytes{};
                std::memcpy(bytes.data(), &gateway, 4);
                it->second.gateways.push_back(Address::ipv4(bytes));
            }
        }
    }
    std::vector<Interface> result;
    for (const auto& name : order) result.push_back(std::move(by_name[name]));
    return result;
}

Result<DnsConfiguration> dns_configuration(std::optional<std::uint32_t>) {
    DnsConfiguration configuration;
    std::ifstream file("/etc/resolv.conf");
    std::string line;
    while (std::getline(file, line)) {
        std::istringstream words(line);
        std::string keyword;
        words >> keyword;
        if (keyword == "nameserver") {
            std::string server;
            words >> server;
            if (auto address = parse_numeric(server, 53);
                address && std::find(configuration.servers.begin(), configuration.servers.end(), *address) == configuration.servers.end())
                configuration.servers.push_back(*address);
        } else if (keyword == "search" || keyword == "domain") {
            std::string domain;
            while (words >> domain) configuration.search.push_back(domain);
        }
    }
    return configuration;
}

Result<std::uint32_t> best_interface(const Address& destination) {
    // The route the kernel picks for a connected datagram socket.
    const int fd = ::socket(native_family(destination.family), SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return last_error();
    sockaddr_storage target{};
    auto copy = destination;
    if (!copy.port) copy.port = 9;
    const auto length = to_native(copy, target);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&target), length) != 0) {
        ::close(fd);
        return Error::host_unreachable;
    }
    sockaddr_storage local{};
    socklen_t local_length = sizeof(local);
    getsockname(fd, reinterpret_cast<sockaddr*>(&local), &local_length);
    ::close(fd);
    auto source = from_native(reinterpret_cast<sockaddr*>(&local), local_length);
    source.port = 0;
    const auto list = interfaces();
    if (!list) return list.error();
    for (const auto& item : *list)
        for (const auto& assigned : item.addresses)
            if (assigned.address.family == source.family && assigned.address.bytes == source.bytes) return item.index;
    return Error::host_unreachable;
}

Result<Handle> open_socket(Family family, SocketType type, int protocol) {
    // The area's contract (net.hpp): local sockets are streams on every host.
    if (family == Family::local && type == SocketType::datagram) return Error::socket_type;
    const int kind = family == Family::local ? SOCK_STREAM : native_type(type);
    const int fd = ::socket(native_family(family), kind | SOCK_NONBLOCK | SOCK_CLOEXEC, protocol);
    if (fd < 0) return last_error();
    return handle_of(fd);
}

void close_socket(Handle socket) noexcept {
    if (socket) ::close(fd_of(socket));
}

Status set_blocking(Handle socket, bool blocking) {
    const int flags = fcntl(fd_of(socket), F_GETFL);
    if (flags < 0) return last_error();
    if (fcntl(fd_of(socket), F_SETFL, blocking ? (flags & ~O_NONBLOCK) : (flags | O_NONBLOCK)) != 0) return last_error();
    return {};
}

Status bind(Handle socket, const Address& address) {
    sockaddr_storage native{};
    const auto length = to_native(address, native);
    if (::bind(fd_of(socket), reinterpret_cast<sockaddr*>(&native), length) != 0) return last_error();
    return {};
}

Status listen(Handle socket, int backlog) {
    if (::listen(fd_of(socket), backlog) != 0) return last_error();
    return {};
}

Result<Accepted> accept(Handle socket) {
    sockaddr_storage peer{};
    socklen_t length = sizeof(peer);
    const int fd = accept4(fd_of(socket), reinterpret_cast<sockaddr*>(&peer), &length, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) return last_error();
    return Accepted{handle_of(fd), from_native(reinterpret_cast<sockaddr*>(&peer), length)};
}

Status connect(Handle socket, const Address& address) {
    sockaddr_storage native{};
    const auto length = to_native(address, native);
    if (::connect(fd_of(socket), reinterpret_cast<sockaddr*>(&native), length) != 0) return last_error();
    return {};
}

Result<std::size_t> send(Handle socket, ConstBytes data, std::uint32_t flags) {
    const auto sent = ::send(fd_of(socket), data.data(), data.size(), native_flags(flags));
    if (sent < 0) return last_error();
    return static_cast<std::size_t>(sent);
}

Result<std::size_t> send_to(Handle socket, ConstBytes data, const Address& to, std::uint32_t flags) {
    sockaddr_storage native{};
    const auto length = to_native(to, native);
    const auto sent = ::sendto(fd_of(socket), data.data(), data.size(), native_flags(flags),
                               reinterpret_cast<sockaddr*>(&native), length);
    if (sent < 0) return last_error();
    return static_cast<std::size_t>(sent);
}

Result<std::size_t> receive(Handle socket, Bytes buffer, std::uint32_t flags) {
    const auto got = ::recv(fd_of(socket), buffer.data(), buffer.size(), native_flags(flags) & ~MSG_NOSIGNAL);
    if (got < 0) return last_error();
    return static_cast<std::size_t>(got);
}

Result<Received> receive_from(Handle socket, Bytes buffer, std::uint32_t flags) {
    sockaddr_storage from{};
    socklen_t length = sizeof(from);
    const auto got = ::recvfrom(fd_of(socket), buffer.data(), buffer.size(), native_flags(flags) & ~MSG_NOSIGNAL,
                                reinterpret_cast<sockaddr*>(&from), &length);
    if (got < 0) return last_error();
    return Received{static_cast<std::size_t>(got), from_native(reinterpret_cast<sockaddr*>(&from), length)};
}

Status shutdown(Handle socket, Shutdown how) {
    const int native = how == Shutdown::receive ? SHUT_RD : how == Shutdown::send ? SHUT_WR : SHUT_RDWR;
    if (::shutdown(fd_of(socket), native) != 0) return last_error();
    return {};
}

Result<Address> local_address(Handle socket) {
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    if (getsockname(fd_of(socket), reinterpret_cast<sockaddr*>(&address), &length) != 0) return last_error();
    return from_native(reinterpret_cast<sockaddr*>(&address), length);
}

Result<Address> peer_address(Handle socket) {
    sockaddr_storage address{};
    socklen_t length = sizeof(address);
    if (getpeername(fd_of(socket), reinterpret_cast<sockaddr*>(&address), &length) != 0) return last_error();
    return from_native(reinterpret_cast<sockaddr*>(&address), length);
}

Result<std::size_t> readable_bytes(Handle socket) {
    int bytes = 0;
    if (ioctl(fd_of(socket), FIONREAD, &bytes) != 0) return last_error();
    return static_cast<std::size_t>(bytes);
}

Result<SocketError> pending_error(Handle socket) {
    int code = 0;
    socklen_t length = sizeof(code);
    if (getsockopt(fd_of(socket), SOL_SOCKET, SO_ERROR, &code, &length) != 0) return last_error();
    return SocketError{linux_backend::error(code)};
}

Status set_option(Handle socket, Option option, int value) {
    const auto native = native_option(option);
    if (!native) return Error::protocol_option;
    if (setsockopt(fd_of(socket), native->level, native->name, &value, sizeof(value)) != 0) return last_error();
    return {};
}

Result<int> get_option(Handle socket, Option option) {
    const auto native = native_option(option);
    if (!native) return Error::protocol_option;
    int value = 0;
    socklen_t length = sizeof(value);
    if (getsockopt(fd_of(socket), native->level, native->name, &value, &length) != 0) return last_error();
    return value;
}

Status set_linger(Handle socket, Linger value) {
    ::linger native{value.on ? 1 : 0, value.seconds};
    if (setsockopt(fd_of(socket), SOL_SOCKET, SO_LINGER, &native, sizeof(native)) != 0) return last_error();
    return {};
}

Result<Linger> get_linger(Handle socket) {
    ::linger native{};
    socklen_t length = sizeof(native);
    if (getsockopt(fd_of(socket), SOL_SOCKET, SO_LINGER, &native, &length) != 0) return last_error();
    return Linger{native.l_onoff != 0, static_cast<std::uint16_t>(native.l_linger)};
}

Result<std::uint16_t> ready(Handle socket, std::uint16_t events) { return ready(socket, events, 0); }

Result<std::uint16_t> ready(Handle socket, std::uint16_t events, std::uint64_t timeout_ns) {
    pollfd fd{fd_of(socket), static_cast<short>(poll_events(events)), 0};
    const int timeout_ms = static_cast<int>(std::min<std::uint64_t>((timeout_ns + 999999) / 1000000, 0x7fffffff));
    for (;;) {
        const int result = poll(&fd, 1, timeout_ms);
        if (result < 0 && errno == EINTR) continue;
        if (result < 0) return last_error();
        return result == 0 ? std::uint16_t{0} : ready_bits(fd.revents);
    }
}

Result<std::vector<std::uint8_t>> export_socket(Handle socket, std::uint32_t target_pid) {
    if (target_pid != 0 && target_pid != static_cast<std::uint32_t>(getpid())) return Error::unsupported;
    const int fd = fcntl(fd_of(socket), F_DUPFD_CLOEXEC, 0);
    if (fd < 0) return last_error();
    std::vector<std::uint8_t> blob(sizeof(int) + sizeof(pid_t));
    const pid_t self = getpid();
    std::memcpy(blob.data(), &fd, sizeof(fd));
    std::memcpy(blob.data() + sizeof(fd), &self, sizeof(self));
    return blob;
}

Result<Handle> import_socket(std::span<const std::uint8_t> blob) {
    if (blob.size() != sizeof(int) + sizeof(pid_t)) return Error::invalid_argument;
    int fd = -1;
    pid_t owner = 0;
    std::memcpy(&fd, blob.data(), sizeof(fd));
    std::memcpy(&owner, blob.data() + sizeof(fd), sizeof(owner));
    if (owner != getpid()) return Error::unsupported;
    return handle_of(fd);
}

Result<std::vector<Resolved>> resolve(std::optional<std::string_view> node, std::optional<std::string_view> service,
                                      const Hints& hints) {
    addrinfo native{};
    native.ai_family = native_family(hints.family);
    native.ai_socktype = native_type(hints.type);
    native.ai_protocol = hints.protocol;
    if (hints.flags & resolve_passive) native.ai_flags |= AI_PASSIVE;
    if (hints.flags & resolve_canonical_name) native.ai_flags |= AI_CANONNAME;
    if (hints.flags & resolve_numeric_host) native.ai_flags |= AI_NUMERICHOST;
    if (hints.flags & resolve_all) native.ai_flags |= AI_ALL;
    if (hints.flags & resolve_address_config) native.ai_flags |= AI_ADDRCONFIG;
    if (hints.flags & resolve_v4_mapped) native.ai_flags |= AI_V4MAPPED;
    if (hints.flags & resolve_numeric_service) native.ai_flags |= AI_NUMERICSERV;
    const std::string node_text(node.value_or(""));
    const std::string service_text(service.value_or(""));
    addrinfo* list = nullptr;
    const int code = getaddrinfo(node ? node_text.c_str() : nullptr, service ? service_text.c_str() : nullptr, &native, &list);
    if (code != 0) return resolver_error(code);
    std::vector<Resolved> result;
    for (auto* entry = list; entry; entry = entry->ai_next) {
        if (entry->ai_family != AF_INET && entry->ai_family != AF_INET6) continue;
        Resolved item;
        item.address = from_native(entry->ai_addr, entry->ai_addrlen);
        item.type = socket_type_of(entry->ai_socktype);
        item.protocol = entry->ai_protocol;
        if (entry == list && entry->ai_canonname) item.canonical_name = entry->ai_canonname;
        result.push_back(std::move(item));
    }
    freeaddrinfo(list);
    if (result.empty()) return Error::not_found;
    return result;
}

Result<Name> name_info(const Address& address, std::uint32_t flags) {
    sockaddr_storage native{};
    const auto length = to_native(address, native);
    int native_flags = 0;
    if (flags & name_no_fqdn) native_flags |= NI_NOFQDN;
    if (flags & name_numeric_host) native_flags |= NI_NUMERICHOST;
    if (flags & name_required) native_flags |= NI_NAMEREQD;
    if (flags & name_numeric_service) native_flags |= NI_NUMERICSERV;
    if (flags & name_datagram) native_flags |= NI_DGRAM;
    char host[NI_MAXHOST], service[NI_MAXSERV];
    const int code = getnameinfo(reinterpret_cast<sockaddr*>(&native), length, host, sizeof(host), service, sizeof(service), native_flags);
    if (code != 0) return resolver_error(code);
    return Name{host, service};
}

Result<std::string> host_name() {
    char name[256]{};
    if (gethostname(name, sizeof(name) - 1) != 0) return last_error();
    return std::string(name);
}

Result<HostEntry> host_by_name(std::string_view name) {
    const auto resolved = resolve(name, std::nullopt, Hints{Family::ipv4, SocketType::stream, 0, resolve_canonical_name});
    if (!resolved) return resolved.error();
    HostEntry entry;
    entry.name = resolved->front().canonical_name.empty() ? std::string(name) : resolved->front().canonical_name;
    for (const auto& item : *resolved) {
        auto address = item.address;
        address.port = 0;
        if (std::find(entry.addresses.begin(), entry.addresses.end(), address) == entry.addresses.end()) entry.addresses.push_back(address);
    }
    return entry;
}

Result<HostEntry> host_by_address(const Address& address) {
    const auto name = name_info(address, name_required);
    if (!name) return name.error();
    HostEntry entry;
    entry.name = name->host;
    auto copy = address;
    copy.port = 0;
    entry.addresses.push_back(copy);
    return entry;
}

Result<ServiceEntry> service_by_name(std::string_view name, std::optional<std::string_view> protocol) {
    const std::string service(name), proto(protocol.value_or(""));
    servent result{}, *found = nullptr;
    char buffer[4096];
    if (getservbyname_r(service.c_str(), protocol ? proto.c_str() : nullptr, &result, buffer, sizeof(buffer), &found) != 0 || !found)
        return Error::not_found;
    ServiceEntry entry{found->s_name, found->s_proto, {}, ntohs(static_cast<std::uint16_t>(found->s_port))};
    for (char** alias = found->s_aliases; alias && *alias; ++alias) entry.aliases.emplace_back(*alias);
    return entry;
}

Result<ServiceEntry> service_by_port(std::uint16_t port, std::optional<std::string_view> protocol) {
    const std::string proto(protocol.value_or(""));
    servent result{}, *found = nullptr;
    char buffer[4096];
    if (getservbyport_r(htons(port), protocol ? proto.c_str() : nullptr, &result, buffer, sizeof(buffer), &found) != 0 || !found)
        return Error::not_found;
    ServiceEntry entry{found->s_name, found->s_proto, {}, port};
    for (char** alias = found->s_aliases; alias && *alias; ++alias) entry.aliases.emplace_back(*alias);
    return entry;
}

Result<ProtocolEntry> protocol_by_name(std::string_view name) {
    const std::string text(name);
    protoent result{}, *found = nullptr;
    char buffer[1024];
    if (getprotobyname_r(text.c_str(), &result, buffer, sizeof(buffer), &found) != 0 || !found) return Error::not_found;
    ProtocolEntry entry{found->p_name, {}, found->p_proto};
    for (char** alias = found->p_aliases; alias && *alias; ++alias) entry.aliases.emplace_back(*alias);
    return entry;
}

} // namespace alchemy::host::net
