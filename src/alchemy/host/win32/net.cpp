// gethostbyname and gethostbyaddr back the legacy Darwin lookups.
#define _WINSOCK_DEPRECATED_NO_WARNINGS
#include "alchemy/host/net.hpp"

#include "alchemy/host/win32/win32.hpp"

#include <WS2tcpip.h>
#include <afunix.h>
#include <iphlpapi.h>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstring>
#include <mutex>

namespace alchemy::host::net {

Status initialize() {
    static std::once_flag once;
    static Error result = Error::none;
    std::call_once(once, [] {
        WSADATA data{};
        if (const auto code = WSAStartup(MAKEWORD(2, 2), &data)) result = win32::error(static_cast<DWORD>(code));
    });
    if (result != Error::none) return result;
    return {};
}

namespace {

// A sockaddr_in / sockaddr_in6 as an Address (port and scope kept); nullopt for other families.
std::optional<Address> address_of(const sockaddr* address) {
    if (!address) return std::nullopt;
    if (address->sa_family == AF_INET) {
        const auto* v4 = reinterpret_cast<const sockaddr_in*>(address);
        std::array<std::uint8_t, 4> bytes{};
        std::memcpy(bytes.data(), &v4->sin_addr, 4);
        return Address::ipv4(bytes, ntohs(v4->sin_port));
    }
    if (address->sa_family == AF_INET6) {
        const auto* v6 = reinterpret_cast<const sockaddr_in6*>(address);
        std::array<std::uint8_t, 16> bytes{};
        std::memcpy(bytes.data(), &v6->sin6_addr, 16);
        return Address::ipv6(bytes, ntohs(v6->sin6_port), v6->sin6_scope_id);
    }
    return std::nullopt;
}

// GetAdaptersAddresses with `flags` into `buffer`; NO_ERROR, ERROR_NO_DATA or the failure.
ULONG adapters(ULONG flags, std::vector<std::uint8_t>& buffer) {
    ULONG size = 32 * 1024;
    ULONG status = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 4 && status == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        status = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    return status;
}

} // namespace

Result<DnsConfiguration> dns_configuration(std::optional<std::uint32_t> index) {
    std::vector<std::uint8_t> buffer;
    const auto status = adapters(GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST, buffer);
    DnsConfiguration result;
    if (status == ERROR_NO_DATA) return result;
    if (status != NO_ERROR) return win32::error(status);
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter; adapter = adapter->Next) {
        const auto adapter_index = adapter->IfIndex ? adapter->IfIndex : adapter->Ipv6IfIndex;
        if (index) {
            if (adapter_index != *index) continue;
        } else if (adapter->OperStatus != IfOperStatusUp || adapter->IfType == IF_TYPE_SOFTWARE_LOOPBACK) {
            continue;
        }
        for (auto* server = adapter->FirstDnsServerAddress; server; server = server->Next) {
            auto address = address_of(server->Address.lpSockaddr);
            if (!address) continue;
            // Site-local fec0::/10 placeholders Windows lists when no IPv6 DNS is configured.
            if (address->family == Family::ipv6 && address->bytes[0] == 0xfe && (address->bytes[1] & 0xc0) == 0xc0) continue;
            address->port = 53;
            if (std::find(result.servers.begin(), result.servers.end(), *address) == result.servers.end()) result.servers.push_back(*address);
        }
        if (adapter->DnsSuffix && *adapter->DnsSuffix) {
            const auto suffix = win32::narrow(adapter->DnsSuffix);
            if (std::find(result.search.begin(), result.search.end(), suffix) == result.search.end()) result.search.push_back(suffix);
        }
    }
    return result;
}

Result<std::uint32_t> best_interface(const Address& destination) {
    SOCKADDR_STORAGE target{};
    if (destination.family == Family::ipv4) {
        auto& v4 = reinterpret_cast<sockaddr_in&>(target);
        v4.sin_family = AF_INET;
        std::memcpy(&v4.sin_addr, destination.bytes.data(), 4);
    } else if (destination.family == Family::ipv6) {
        auto& v6 = reinterpret_cast<sockaddr_in6&>(target);
        v6.sin6_family = AF_INET6;
        std::memcpy(&v6.sin6_addr, destination.bytes.data(), 16);
        v6.sin6_scope_id = destination.scope_id;
    } else {
        return Error::address_family;
    }
    DWORD index{};
    if (const auto status = GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&target), &index); status != NO_ERROR)
        return status == ERROR_NETWORK_UNREACHABLE || status == ERROR_HOST_UNREACHABLE ? Error::host_unreachable : win32::error(status);
    return static_cast<std::uint32_t>(index);
}

Result<std::vector<Interface>> interfaces() {
    constexpr ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER | GAA_FLAG_INCLUDE_GATEWAYS;
    ULONG size = 32 * 1024;
    std::vector<std::uint8_t> buffer;
    ULONG status = ERROR_BUFFER_OVERFLOW;
    for (int attempt = 0; attempt < 4 && status == ERROR_BUFFER_OVERFLOW; ++attempt) {
        buffer.resize(size);
        status = GetAdaptersAddresses(AF_UNSPEC, flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()), &size);
    }
    std::vector<Interface> result;
    if (status == ERROR_NO_DATA) return result;
    if (status != NO_ERROR) return win32::error(status);
    for (auto* adapter = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buffer.data()); adapter; adapter = adapter->Next) {
        Interface entry;
        entry.index = adapter->IfIndex ? adapter->IfIndex : adapter->Ipv6IfIndex;
        entry.id = adapter->AdapterName ? adapter->AdapterName : "";
        entry.display_name = adapter->FriendlyName ? win32::narrow(adapter->FriendlyName) : std::string();
        switch (adapter->IfType) {
        case IF_TYPE_SOFTWARE_LOOPBACK: entry.kind = InterfaceKind::loopback; break;
        case IF_TYPE_ETHERNET_CSMACD: entry.kind = InterfaceKind::ethernet; break;
        case IF_TYPE_IEEE80211: entry.kind = InterfaceKind::wireless; break;
        case IF_TYPE_TUNNEL: entry.kind = InterfaceKind::tunnel; break;
        case IF_TYPE_PPP: entry.kind = InterfaceKind::ppp; break;
        case IF_TYPE_WWANPP: case IF_TYPE_WWANPP2: entry.kind = InterfaceKind::cellular; break;
        default: entry.kind = InterfaceKind::other; break;
        }
        entry.up = adapter->OperStatus == IfOperStatusUp;
        entry.multicast = !(adapter->Flags & IP_ADAPTER_NO_MULTICAST);
        entry.mtu = adapter->Mtu == 0xffffffffU ? 0 : adapter->Mtu;
        entry.speed = adapter->TransmitLinkSpeed == ~0ULL ? 0 : adapter->TransmitLinkSpeed;
        entry.hardware.assign(adapter->PhysicalAddress, adapter->PhysicalAddress + adapter->PhysicalAddressLength);
        for (auto* unicast = adapter->FirstUnicastAddress; unicast; unicast = unicast->Next) {
            const auto* address = unicast->Address.lpSockaddr;
            Interface::Assigned value;
            value.prefix_length = unicast->OnLinkPrefixLength;
            if (address->sa_family == AF_INET) {
                std::array<std::uint8_t, 4> bytes{};
                std::memcpy(bytes.data(), &reinterpret_cast<const sockaddr_in*>(address)->sin_addr, 4);
                value.address = Address::ipv4(bytes);
            } else if (address->sa_family == AF_INET6) {
                const auto* v6 = reinterpret_cast<const sockaddr_in6*>(address);
                std::array<std::uint8_t, 16> bytes{};
                std::memcpy(bytes.data(), &v6->sin6_addr, 16);
                value.address = Address::ipv6(bytes, 0, v6->sin6_scope_id);
            } else {
                continue;
            }
            entry.addresses.push_back(std::move(value));
        }
        entry.ipv6_index = adapter->Ipv6IfIndex;
        for (auto* gateway = adapter->FirstGatewayAddress; gateway; gateway = gateway->Next)
            if (const auto address = address_of(gateway->Address.lpSockaddr)) entry.gateways.push_back(*address);
        result.push_back(std::move(entry));
    }
    return result;
}

// ---- sockets ----
namespace {

SOCKET native_socket(Handle socket) noexcept { return static_cast<SOCKET>(socket.value); }
Handle wrap_socket(SOCKET socket) noexcept { return socket == INVALID_SOCKET ? Handle{} : Handle{static_cast<std::uintptr_t>(socket)}; }

// A host sockaddr for `address`; false (with the error) when it has none.
struct Native {
    sockaddr_storage storage{};
    int length{};
    const sockaddr* get() const noexcept { return reinterpret_cast<const sockaddr*>(&storage); }
};
Result<Native> to_native(const Address& address) {
    Native native;
    switch (address.family) {
    case Family::ipv4: {
        auto& value = reinterpret_cast<sockaddr_in&>(native.storage);
        value.sin_family = AF_INET;
        value.sin_port = htons(address.port);
        std::memcpy(&value.sin_addr, address.bytes.data(), 4);
        native.length = sizeof(value);
        return native;
    }
    case Family::ipv6: {
        auto& value = reinterpret_cast<sockaddr_in6&>(native.storage);
        value.sin6_family = AF_INET6;
        value.sin6_port = htons(address.port);
        value.sin6_flowinfo = address.flow_info;
        std::memcpy(&value.sin6_addr, address.bytes.data(), 16);
        value.sin6_scope_id = address.scope_id;
        native.length = sizeof(value);
        return native;
    }
    case Family::local: {
        auto& value = reinterpret_cast<sockaddr_un&>(native.storage);
        if (address.path.size() >= sizeof(value.sun_path)) return Error::name_too_long;
        value.sun_family = AF_UNIX;
        std::memcpy(value.sun_path, address.path.data(), address.path.size());
        native.length = sizeof(value);
        return native;
    }
    case Family::unspecified: break;
    }
    return Error::address_family;
}
Address from_native(const sockaddr* native, int length) {
    if (!native || length < static_cast<int>(sizeof(native->sa_family))) return {};
    if (native->sa_family == AF_INET && length >= static_cast<int>(sizeof(sockaddr_in))) {
        const auto& value = *reinterpret_cast<const sockaddr_in*>(native);
        std::array<std::uint8_t, 4> bytes{};
        std::memcpy(bytes.data(), &value.sin_addr, 4);
        return Address::ipv4(bytes, ntohs(value.sin_port));
    }
    if (native->sa_family == AF_INET6 && length >= static_cast<int>(sizeof(sockaddr_in6))) {
        const auto& value = *reinterpret_cast<const sockaddr_in6*>(native);
        std::array<std::uint8_t, 16> bytes{};
        std::memcpy(bytes.data(), &value.sin6_addr, 16);
        auto address = Address::ipv6(bytes, ntohs(value.sin6_port), value.sin6_scope_id);
        address.flow_info = value.sin6_flowinfo;
        return address;
    }
    if (native->sa_family == AF_UNIX) {
        const auto& value = *reinterpret_cast<const sockaddr_un*>(native);
        const auto available = std::min<std::size_t>(sizeof(value.sun_path),
            length > static_cast<int>(offsetof(sockaddr_un, sun_path)) ? static_cast<std::size_t>(length) - offsetof(sockaddr_un, sun_path) : 0);
        return Address::local(std::string_view(value.sun_path, strnlen(value.sun_path, available)));
    }
    return {};
}
int native_family(Family family) {
    switch (family) {
    case Family::ipv4: return AF_INET;
    case Family::ipv6: return AF_INET6;
    case Family::local: return AF_UNIX;
    case Family::unspecified: break;
    }
    return AF_UNSPEC;
}
// Non-blocking and not inherited, as every socket of this area.
Result<Handle> adopt(SOCKET socket) {
    if (socket == INVALID_SOCKET) return win32::last_socket_error();
    u_long enabled = 1;
    if (ioctlsocket(socket, FIONBIO, &enabled) || !SetHandleInformation(reinterpret_cast<HANDLE>(socket), HANDLE_FLAG_INHERIT, 0)) {
        const auto error = win32::last_error();
        closesocket(socket);
        return error;
    }
    return wrap_socket(socket);
}
Status socket_status(int result) {
    if (result == SOCKET_ERROR) return win32::last_socket_error();
    return {};
}
// Winsock's flags for the MSG_* meanings.
int native_flags(std::uint32_t flags) {
    int result = 0;
    if (flags & message_out_of_band) result |= MSG_OOB;
    if (flags & message_peek) result |= MSG_PEEK;
    if (flags & message_dont_route) result |= MSG_DONTROUTE;
    return result;
}
int clamp_length(std::size_t size) { return static_cast<int>(std::min<std::size_t>(size, INT_MAX)); }
// Winsock fails a receive into a buffer shorter than the datagram with
// WSAEMSGSIZE after copying the prefix: the prefix is the result.
Result<std::size_t> received(int count, std::size_t capacity) {
    if (count != SOCKET_ERROR) return static_cast<std::size_t>(count);
    if (WSAGetLastError() == WSAEMSGSIZE) return capacity;
    return win32::last_socket_error();
}
struct NativeOption { int level, name; };
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
    // TCP_KEEPALIVE (idle seconds), TCP_KEEPINTVL and TCP_KEEPCNT (ws2ipdef.h; Windows 10 1709 and later).
    case Option::tcp_keep_idle: return NativeOption{IPPROTO_TCP, 3};
    case Option::tcp_keep_interval: return NativeOption{IPPROTO_TCP, 17};
    case Option::tcp_keep_count: return NativeOption{IPPROTO_TCP, 16};
    case Option::ip_type_of_service: return NativeOption{IPPROTO_IP, IP_TOS};
    case Option::ip_time_to_live: return NativeOption{IPPROTO_IP, IP_TTL};
    case Option::ipv6_only: return NativeOption{IPPROTO_IPV6, IPV6_V6ONLY};
    }
    return std::nullopt;
}

} // namespace

Result<Handle> open_socket(Family family, SocketType type, int protocol) {
    if (const auto started = initialize(); !started) return started.error();
    if (type == SocketType::unspecified) return Error::socket_type;
    if (family == Family::unspecified) return Error::address_family;
    if (family == Family::local && type != SocketType::stream) return Error::socket_type;
    return adopt(::socket(native_family(family), type == SocketType::stream ? SOCK_STREAM : SOCK_DGRAM, protocol));
}

void close_socket(Handle socket) noexcept {
    if (socket) closesocket(native_socket(socket));
}

Status set_blocking(Handle socket, bool blocking) {
    u_long value = blocking ? 0 : 1;
    return socket_status(ioctlsocket(native_socket(socket), FIONBIO, &value));
}

Status bind(Handle socket, const Address& address) {
    const auto native = to_native(address);
    if (!native) return native.error();
    return socket_status(::bind(native_socket(socket), native->get(), native->length));
}

Status listen(Handle socket, int backlog) { return socket_status(::listen(native_socket(socket), backlog)); }

Result<Accepted> accept(Handle socket) {
    sockaddr_storage peer{};
    int length = sizeof(peer);
    const auto accepted = adopt(::accept(native_socket(socket), reinterpret_cast<sockaddr*>(&peer), &length));
    if (!accepted) return accepted.error();
    return Accepted{*accepted, from_native(reinterpret_cast<const sockaddr*>(&peer), length)};
}

Status connect(Handle socket, const Address& address) {
    const auto native = to_native(address);
    if (!native) return native.error();
    return socket_status(::connect(native_socket(socket), native->get(), native->length));
}

Result<std::size_t> send(Handle socket, ConstBytes data, std::uint32_t flags) {
    const auto count = ::send(native_socket(socket), reinterpret_cast<const char*>(data.data()), clamp_length(data.size()), native_flags(flags));
    if (count == SOCKET_ERROR) return win32::last_socket_error();
    return static_cast<std::size_t>(count);
}

Result<std::size_t> send_to(Handle socket, ConstBytes data, const Address& to, std::uint32_t flags) {
    const auto native = to_native(to);
    if (!native) return native.error();
    const auto count = ::sendto(native_socket(socket), reinterpret_cast<const char*>(data.data()), clamp_length(data.size()), native_flags(flags),
                                native->get(), native->length);
    if (count == SOCKET_ERROR) return win32::last_socket_error();
    return static_cast<std::size_t>(count);
}

Result<std::size_t> receive(Handle socket, Bytes buffer, std::uint32_t flags) {
    const auto length = clamp_length(buffer.size());
    return received(::recv(native_socket(socket), reinterpret_cast<char*>(buffer.data()), length, native_flags(flags)),
                    static_cast<std::size_t>(length));
}

Result<Received> receive_from(Handle socket, Bytes buffer, std::uint32_t flags) {
    sockaddr_storage from{};
    int from_length = sizeof(from);
    const auto length = clamp_length(buffer.size());
    const auto count = received(::recvfrom(native_socket(socket), reinterpret_cast<char*>(buffer.data()), length, native_flags(flags),
                                           reinterpret_cast<sockaddr*>(&from), &from_length),
                                static_cast<std::size_t>(length));
    if (!count) return count.error();
    return Received{*count, from_native(reinterpret_cast<const sockaddr*>(&from), from_length)};
}

Status shutdown(Handle socket, Shutdown how) {
    const int native = how == Shutdown::receive ? SD_RECEIVE : how == Shutdown::send ? SD_SEND : SD_BOTH;
    return socket_status(::shutdown(native_socket(socket), native));
}

Result<Address> local_address(Handle socket) {
    sockaddr_storage value{};
    int length = sizeof(value);
    if (::getsockname(native_socket(socket), reinterpret_cast<sockaddr*>(&value), &length)) return win32::last_socket_error();
    return from_native(reinterpret_cast<const sockaddr*>(&value), length);
}

Result<Address> peer_address(Handle socket) {
    sockaddr_storage value{};
    int length = sizeof(value);
    if (::getpeername(native_socket(socket), reinterpret_cast<sockaddr*>(&value), &length)) return win32::last_socket_error();
    return from_native(reinterpret_cast<const sockaddr*>(&value), length);
}

Result<std::size_t> readable_bytes(Handle socket) {
    u_long count = 0;
    if (ioctlsocket(native_socket(socket), FIONREAD, &count)) return win32::last_socket_error();
    return static_cast<std::size_t>(count);
}

Result<SocketError> pending_error(Handle socket) {
    int value = 0, length = sizeof(value);
    if (getsockopt(native_socket(socket), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&value), &length)) return win32::last_socket_error();
    return SocketError{value ? win32::error(static_cast<DWORD>(value)) : Error::none};
}

Status set_option(Handle socket, Option option, int value) {
    const auto native = native_option(option);
    if (!native) return Error::protocol_option;
    return socket_status(setsockopt(native_socket(socket), native->level, native->name, reinterpret_cast<const char*>(&value), sizeof(value)));
}

Result<int> get_option(Handle socket, Option option) {
    const auto native = native_option(option);
    if (!native) return Error::protocol_option;
    int value = 0, length = sizeof(value);
    if (getsockopt(native_socket(socket), native->level, native->name, reinterpret_cast<char*>(&value), &length)) return win32::last_socket_error();
    return value;
}

Status set_linger(Handle socket, Linger value) {
    linger native{};
    native.l_onoff = static_cast<u_short>(value.on);
    native.l_linger = value.seconds;
    return socket_status(setsockopt(native_socket(socket), SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&native), sizeof(native)));
}

Result<Linger> get_linger(Handle socket) {
    linger native{};
    int length = sizeof(native);
    if (getsockopt(native_socket(socket), SOL_SOCKET, SO_LINGER, reinterpret_cast<char*>(&native), &length)) return win32::last_socket_error();
    return Linger{native.l_onoff != 0, native.l_linger};
}

Result<std::uint16_t> ready(Handle socket, std::uint16_t events) { return ready(socket, events, 0); }

Result<std::uint16_t> ready(Handle socket, std::uint16_t events, std::uint64_t timeout_ns) {
    // WSAPoll fails the whole call with WSAEINVAL for POLLPRI (Winsock has no
    // urgent-data readiness), so it is never asked for.
    WSAPOLLFD query{native_socket(socket), 0, 0};
    if (events & ready_readable) query.events |= POLLRDNORM;
    if (events & ready_writable) query.events |= POLLWRNORM;
    const auto milliseconds = std::min<std::uint64_t>((timeout_ns + 999'999) / 1'000'000, INT_MAX);
    if (WSAPoll(&query, 1, static_cast<INT>(milliseconds)) < 0) return win32::last_socket_error();
    unsigned result = 0;
    if (query.revents & POLLIN) result |= events & ready_readable;
    if (query.revents & POLLOUT) result |= events & ready_writable;
    if (query.revents & POLLERR) result |= ready_error;
    if (query.revents & POLLHUP) result |= ready_hang_up;
    if (query.revents & POLLNVAL) result |= ready_invalid;
    return static_cast<std::uint16_t>(result);
}

Result<std::vector<std::uint8_t>> export_socket(Handle socket, std::uint32_t target_pid) {
    WSAPROTOCOL_INFOW information{};
    if (WSADuplicateSocketW(native_socket(socket), target_pid ? target_pid : GetCurrentProcessId(), &information))
        return win32::last_socket_error();
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&information);
    return std::vector<std::uint8_t>(bytes, bytes + sizeof(information));
}

Result<Handle> import_socket(std::span<const std::uint8_t> blob) {
    if (blob.size() != sizeof(WSAPROTOCOL_INFOW)) return Error::invalid_argument;
    if (const auto started = initialize(); !started) return started.error();
    WSAPROTOCOL_INFOW information{};
    std::memcpy(&information, blob.data(), sizeof(information));
    return adopt(WSASocketW(FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, FROM_PROTOCOL_INFO, &information, 0, WSA_FLAG_OVERLAPPED));
}

// ---- names ----
namespace {

// getaddrinfo/getnameinfo and the legacy lookups report WSA codes.
Error resolver_error(int code) {
    switch (code) {
    case WSAHOST_NOT_FOUND: return Error::not_found;
    case WSATRY_AGAIN: return Error::would_block;
    case WSANO_RECOVERY: return Error::io;
    case WSANO_DATA: return Error::address_not_available;
    case WSA_NOT_ENOUGH_MEMORY: return Error::no_memory;
    case WSAEINVAL: return Error::invalid_argument;
    case WSAEAFNOSUPPORT: return Error::address_family;
    case WSATYPE_NOT_FOUND: return Error::protocol_not_supported;
    case WSAESOCKTNOSUPPORT: return Error::socket_type;
    case WSAEFAULT: return Error::fault;
    default: return Error::other;
    }
}
SocketType socket_type(int native) {
    return native == SOCK_STREAM ? SocketType::stream : native == SOCK_DGRAM ? SocketType::datagram : SocketType::unspecified;
}
std::vector<std::string> copy_names(char** values) {
    std::vector<std::string> names;
    if (values)
        for (std::size_t index = 0; values[index] && index < 64; ++index) names.emplace_back(values[index]);
    return names;
}
Result<HostEntry> host_entry(const hostent* value, std::string_view fallback) {
    if (!value) return resolver_error(WSAGetLastError());
    if (value->h_addrtype != AF_INET && value->h_addrtype != AF_INET6) return Error::io;
    if (value->h_length != (value->h_addrtype == AF_INET ? 4 : 16)) return Error::io;
    HostEntry entry;
    entry.name = value->h_name ? value->h_name : std::string(fallback);
    entry.aliases = copy_names(value->h_aliases);
    for (std::size_t index = 0; value->h_addr_list && value->h_addr_list[index] && index < 64; ++index) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(value->h_addr_list[index]);
        if (value->h_addrtype == AF_INET) {
            std::array<std::uint8_t, 4> address{};
            std::copy_n(bytes, 4, address.begin());
            entry.addresses.push_back(Address::ipv4(address));
        } else {
            std::array<std::uint8_t, 16> address{};
            std::copy_n(bytes, 16, address.begin());
            entry.addresses.push_back(Address::ipv6(address));
        }
    }
    if (entry.addresses.empty()) return Error::address_not_available;
    return entry;
}
Result<ServiceEntry> service_entry(const servent* value) {
    if (!value) return Error::not_found;
    return ServiceEntry{value->s_name ? value->s_name : "", value->s_proto ? value->s_proto : "", copy_names(value->s_aliases),
                        ntohs(static_cast<u_short>(value->s_port))};
}

} // namespace

Result<std::vector<Resolved>> resolve(std::optional<std::string_view> node, std::optional<std::string_view> service, const Hints& hints) {
    if (const auto started = initialize(); !started) return started.error();
    addrinfo native{};
    native.ai_family = native_family(hints.family);
    native.ai_socktype = hints.type == SocketType::stream ? SOCK_STREAM : hints.type == SocketType::datagram ? SOCK_DGRAM : 0;
    native.ai_protocol = hints.protocol;
    if (hints.flags & resolve_passive) native.ai_flags |= AI_PASSIVE;
    if (hints.flags & resolve_canonical_name) native.ai_flags |= AI_CANONNAME;
    if (hints.flags & resolve_numeric_host) native.ai_flags |= AI_NUMERICHOST;
    if (hints.flags & resolve_all) native.ai_flags |= AI_ALL;
    if (hints.flags & resolve_address_config) native.ai_flags |= AI_ADDRCONFIG;
    if (hints.flags & resolve_v4_mapped) native.ai_flags |= AI_V4MAPPED;
    if (hints.flags & resolve_numeric_service) native.ai_flags |= AI_NUMERICSERV;
    const std::string node_text(node.value_or("")), service_text(service.value_or(""));
    addrinfo* list = nullptr;
    if (const auto code = getaddrinfo(node ? node_text.c_str() : nullptr, service ? service_text.c_str() : nullptr, &native, &list))
        return resolver_error(code);
    std::vector<Resolved> result;
    for (auto* item = list; item; item = item->ai_next) {
        if (item->ai_family != AF_INET && item->ai_family != AF_INET6) continue;
        Resolved entry;
        entry.address = from_native(item->ai_addr, static_cast<int>(item->ai_addrlen));
        entry.type = socket_type(item->ai_socktype);
        entry.protocol = item->ai_protocol;
        if (item->ai_canonname) entry.canonical_name = item->ai_canonname;
        result.push_back(std::move(entry));
    }
    freeaddrinfo(list);
    if (result.empty()) return Error::not_found;
    return result;
}

Result<Name> name_info(const Address& address, std::uint32_t flags) {
    if (const auto started = initialize(); !started) return started.error();
    const auto native = to_native(address);
    if (!native) return native.error();
    int native_flags = 0;
    if (flags & name_no_fqdn) native_flags |= NI_NOFQDN;
    if (flags & name_numeric_host) native_flags |= NI_NUMERICHOST;
    if (flags & name_required) native_flags |= NI_NAMEREQD;
    if (flags & name_numeric_service) native_flags |= NI_NUMERICSERV;
    if (flags & name_datagram) native_flags |= NI_DGRAM;
    char host[NI_MAXHOST]{}, service[NI_MAXSERV]{};
    if (const auto code = getnameinfo(native->get(), native->length, host, NI_MAXHOST, service, NI_MAXSERV, native_flags))
        return resolver_error(code);
    return Name{host, service};
}

Result<std::string> host_name() {
    if (const auto started = initialize(); !started) return started.error();
    char name[256]{};
    if (gethostname(name, sizeof(name))) return win32::last_socket_error();
    return std::string(name);
}

Result<HostEntry> host_by_name(std::string_view name) {
    if (const auto started = initialize(); !started) return started.error();
    const std::string text(name);
    return host_entry(gethostbyname(text.c_str()), name);
}

Result<HostEntry> host_by_address(const Address& address) {
    if (const auto started = initialize(); !started) return started.error();
    if (address.family != Family::ipv4 && address.family != Family::ipv6) return Error::address_family;
    const auto length = address.family == Family::ipv4 ? 4 : 16;
    return host_entry(gethostbyaddr(reinterpret_cast<const char*>(address.bytes.data()), length, native_family(address.family)), {});
}

Result<ServiceEntry> service_by_name(std::string_view name, std::optional<std::string_view> protocol) {
    if (const auto started = initialize(); !started) return started.error();
    const std::string text(name), protocol_text(protocol.value_or(""));
    return service_entry(getservbyname(text.c_str(), protocol ? protocol_text.c_str() : nullptr));
}

Result<ServiceEntry> service_by_port(std::uint16_t port, std::optional<std::string_view> protocol) {
    if (const auto started = initialize(); !started) return started.error();
    const std::string protocol_text(protocol.value_or(""));
    return service_entry(getservbyport(htons(port), protocol ? protocol_text.c_str() : nullptr));
}

Result<ProtocolEntry> protocol_by_name(std::string_view name) {
    if (const auto started = initialize(); !started) return started.error();
    const std::string text(name);
    const auto* value = getprotobyname(text.c_str());
    if (!value) return Error::not_found;
    return ProtocolEntry{value->p_name ? value->p_name : "", copy_names(value->p_aliases), value->p_proto};
}

} // namespace alchemy::host::net
