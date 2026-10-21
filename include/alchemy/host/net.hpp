#pragma once

// Host area "net" (owner: r1). The rules are in include/alchemy/host/host.hpp. Sockets (AF_UNIX
// included), polling, name resolution and network interfaces. The callers in the macOS layer check
// the network policy (network_policy.hpp) before they get here.
// Written by r2 for SystemConfiguration, libresolv and Network.framework (cross-owner, agreed
// with r1): InterfaceKind::cellular, Interface::ipv6_index and gateways, dns_configuration,
// best_interface.

#include "alchemy/host/host.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::host::net {

// A socket address as a value, independent of any sockaddr layout (the host's
// or Darwin's): callers convert at their edge. The helpers below are plain
// C++ and work on every backend (src/alchemy/host/common/net.cpp).
enum class Family : std::uint8_t { unspecified, ipv4, ipv6, local };
struct Address {
    Family family{};
    std::uint16_t port{};                    // host byte order (ipv4, ipv6)
    std::array<std::uint8_t, 16> bytes{};    // network order; ipv4 uses the first 4
    std::uint32_t flow_info{}, scope_id{};   // ipv6
    std::string path;                        // local (AF_UNIX); empty: unnamed

    [[nodiscard]] static Address ipv4(std::array<std::uint8_t, 4> bytes, std::uint16_t port = 0) noexcept;
    [[nodiscard]] static Address ipv6(std::array<std::uint8_t, 16> bytes, std::uint16_t port = 0, std::uint32_t scope_id = 0) noexcept;
    [[nodiscard]] static Address local(std::string_view path);
    friend bool operator==(const Address&, const Address&) = default;
};

// Loopback: 127.0.0.0/8, ::1 and IPv4-mapped ::ffff:127.0.0.0/104. A local
// (AF_UNIX) address stays on this machine and counts as loopback.
[[nodiscard]] bool is_loopback(const Address& address) noexcept;
// A numeric IPv4 (dotted quad) or IPv6 literal, optionally bracketed, with an
// optional zone ("fe80::1%7": a numeric zone is the scope id); nullopt for
// anything that needs name resolution.
[[nodiscard]] std::optional<Address> parse_numeric(std::string_view host, std::uint16_t port = 0);
// A host name or literal that stays on this machine: "localhost", names
// ending in ".localhost" (RFC 6761) and loopback literals. A trailing dot is
// ignored.
[[nodiscard]] bool loopback_host(std::string_view host);
// The address alone ("192.0.2.1", "2001:db8::1" in RFC 5952 form, the path),
// and with its port ("192.0.2.1:80", "[2001:db8::1]:443").
[[nodiscard]] std::string host_text(const Address& address);
[[nodiscard]] std::string to_string(const Address& address);

// Starts the host socket stack once per process (WSAStartup); idempotent.
[[nodiscard]] Status initialize();

// The host's network interfaces (getifaddrs, if_nameindex, SystemConfiguration),
// in the host's order. Names are the host's: the macOS layer gives them
// Darwin names (lo0, en<n>, utun<n>) itself.
enum class InterfaceKind : std::uint8_t { loopback, ethernet, wireless, tunnel, ppp, other, cellular };
struct Interface {
    std::uint32_t index{};                 // the host interface index (IPv6 scope ids)
    std::string id;                        // the host's stable adapter name
    std::string display_name;              // its human-readable name
    InterfaceKind kind{InterfaceKind::other};
    bool up{}, multicast{};
    std::uint32_t mtu{};                   // 0: unknown
    std::uint64_t speed{};                 // transmit bits per second; 0: unknown
    std::vector<std::uint8_t> hardware;    // the link-layer address; empty when none
    struct Assigned {
        Address address;                   // port 0; IPv6 with its scope id
        std::uint8_t prefix_length{};
    };
    std::vector<Assigned> addresses;       // unicast IPv4 and IPv6 addresses
    std::uint32_t ipv6_index{};            // the IPv6 interface index (best_interface for an IPv6 destination)
    std::vector<Address> gateways;         // default gateways, port 0
};
[[nodiscard]] Result<std::vector<Interface>> interfaces();

// The DNS resolver configuration of the interfaces that are up (not
// loopback), or of one interface (`index`, as Interface::index): server
// addresses with port 53, without duplicates or the site-local fec0::/10
// placeholders some hosts list, and the search domains (DNS suffixes).
struct DnsConfiguration {
    std::vector<Address> servers;
    std::vector<std::string> search;
};
[[nodiscard]] Result<DnsConfiguration> dns_configuration(std::optional<std::uint32_t> index = std::nullopt);

// The index (Interface::index, or ipv6_index for IPv6) of the interface the
// host routes traffic to `destination` through. Error::host_unreachable when
// there is no route.
[[nodiscard]] Result<std::uint32_t> best_interface(const Address& destination);

// ---- sockets ----
// A socket is a Handle closed with close_socket (never host::close). Every
// socket this area opens (open_socket, accept, import_socket) is non-blocking
// and not inherited by child processes. Failures are the socket kinds of
// host::Error: would_block, in_progress, already, not_connected, ...
enum class SocketType : std::uint8_t { unspecified, stream, datagram };
// `protocol` 0 picks the type's default (TCP, UDP). A local (AF_UNIX) socket
// is a stream.
[[nodiscard]] Result<Handle> open_socket(Family family, SocketType type, int protocol = 0);
void close_socket(Handle socket) noexcept;
// A blocking socket makes connect wait for its handshake (local sockets).
[[nodiscard]] Status set_blocking(Handle socket, bool blocking);
[[nodiscard]] Status bind(Handle socket, const Address& address);
[[nodiscard]] Status listen(Handle socket, int backlog);
struct Accepted {
    Handle socket;
    Address peer;
};
[[nodiscard]] Result<Accepted> accept(Handle socket);
// On a non-blocking socket a connection under way fails with would_block,
// in_progress or already; pending_error reports how it ended.
[[nodiscard]] Status connect(Handle socket, const Address& address);

// Message flags (MSG_OOB, MSG_PEEK, MSG_DONTROUTE).
inline constexpr std::uint32_t message_out_of_band = 1, message_peek = 2, message_dont_route = 4;
// A send never raises a signal. A datagram longer than the receive buffer is
// truncated to it.
[[nodiscard]] Result<std::size_t> send(Handle socket, ConstBytes data, std::uint32_t flags = 0);
[[nodiscard]] Result<std::size_t> send_to(Handle socket, ConstBytes data, const Address& to, std::uint32_t flags = 0);
[[nodiscard]] Result<std::size_t> receive(Handle socket, Bytes buffer, std::uint32_t flags = 0);
struct Received {
    std::size_t size{};
    Address from;
};
[[nodiscard]] Result<Received> receive_from(Handle socket, Bytes buffer, std::uint32_t flags = 0);
enum class Shutdown : std::uint8_t { receive, send, both };
[[nodiscard]] Status shutdown(Handle socket, Shutdown how);
[[nodiscard]] Result<Address> local_address(Handle socket);
[[nodiscard]] Result<Address> peer_address(Handle socket);
// Bytes that can be read now (FIONREAD).
[[nodiscard]] Result<std::size_t> readable_bytes(Handle socket);
// The error a non-blocking connect ended with (SO_ERROR; reading clears it):
// Error::none when it succeeded.
struct SocketError {
    Error error{Error::none};
};
[[nodiscard]] Result<SocketError> pending_error(Handle socket);

// Integer socket options (SOL_SOCKET, IPPROTO_TCP, IPPROTO_IP, IPPROTO_IPV6).
// One the host lacks fails with Error::protocol_option.
enum class Option : std::uint8_t {
    debug, accepting, reuse_address, keep_alive, dont_route, broadcast, out_of_band_inline, send_buffer, receive_buffer,
    tcp_no_delay, tcp_keep_idle, tcp_keep_interval, tcp_keep_count, ip_type_of_service, ip_time_to_live, ipv6_only
};
[[nodiscard]] Status set_option(Handle socket, Option option, int value);
[[nodiscard]] Result<int> get_option(Handle socket, Option option);
struct Linger {
    bool on{};
    std::uint16_t seconds{};
};
[[nodiscard]] Status set_linger(Handle socket, Linger value);
[[nodiscard]] Result<Linger> get_linger(Handle socket);

// Readiness of one socket now, without waiting (poll). `events` is readable
// and/or writable; the result may add error, hang_up and invalid.
inline constexpr std::uint16_t ready_readable = 1, ready_writable = 2, ready_error = 4, ready_hang_up = 8, ready_invalid = 16;
[[nodiscard]] Result<std::uint16_t> ready(Handle socket, std::uint16_t events);
// The same, waiting up to `timeout_ns` (rounded up to whole milliseconds)
// for the socket to become ready or fail; 0 when the time runs out.
[[nodiscard]] Result<std::uint16_t> ready(Handle socket, std::uint16_t events, std::uint64_t timeout_ns);

// A socket for another host process (fork/exec, inherited descriptors): an
// opaque blob made for process `target_pid` (0: this process), opened there
// by import_socket.
[[nodiscard]] Result<std::vector<std::uint8_t>> export_socket(Handle socket, std::uint32_t target_pid);
[[nodiscard]] Result<Handle> import_socket(std::span<const std::uint8_t> blob);

// ---- names ----
// These block; the macOS layer runs them on resolver threads. Their failures:
// not_found (no such name), would_block (a temporary failure: try again),
// address_not_available (the name exists without an address of the kind
// asked for), invalid_argument (bad flags or hints), address_family,
// socket_type, protocol_not_supported (a service the socket type lacks),
// no_memory, io (an unrecoverable failure), other.

// getaddrinfo. Flags (the AI_* meanings):
inline constexpr std::uint32_t resolve_passive = 1, resolve_canonical_name = 2, resolve_numeric_host = 4, resolve_all = 0x100,
                               resolve_address_config = 0x400, resolve_v4_mapped = 0x800, resolve_numeric_service = 0x1000;
struct Hints {
    Family family{Family::unspecified};
    SocketType type{SocketType::unspecified};
    int protocol{};
    std::uint32_t flags{};
};
struct Resolved {
    Address address;                     // ipv4 or ipv6, with the service's port
    SocketType type{};
    int protocol{};
    std::string canonical_name;          // with resolve_canonical_name, on the first entry
};
// Either `node` or `service` may be absent (not both). No IPv4 or IPv6
// result: not_found.
[[nodiscard]] Result<std::vector<Resolved>> resolve(std::optional<std::string_view> node, std::optional<std::string_view> service,
                                                    const Hints& hints);
// getnameinfo. Flags (the NI_* meanings):
inline constexpr std::uint32_t name_no_fqdn = 1, name_numeric_host = 2, name_required = 4, name_numeric_service = 8, name_datagram = 16;
struct Name {
    std::string host, service;
};
[[nodiscard]] Result<Name> name_info(const Address& address, std::uint32_t flags);
// This machine's host name (gethostname).
[[nodiscard]] Result<std::string> host_name();

// The legacy databases: gethostbyname (IPv4), gethostbyaddr, getservbyname,
// getservbyport and getprotobyname.
struct HostEntry {
    std::string name;
    std::vector<std::string> aliases;
    std::vector<Address> addresses;      // one family
};
[[nodiscard]] Result<HostEntry> host_by_name(std::string_view name);
[[nodiscard]] Result<HostEntry> host_by_address(const Address& address);
struct ServiceEntry {
    std::string name, protocol;
    std::vector<std::string> aliases;
    std::uint16_t port{};                // host byte order
};
[[nodiscard]] Result<ServiceEntry> service_by_name(std::string_view name, std::optional<std::string_view> protocol);
[[nodiscard]] Result<ServiceEntry> service_by_port(std::uint16_t port, std::optional<std::string_view> protocol);
struct ProtocolEntry {
    std::string name;
    std::vector<std::string> aliases;
    int number{};
};
[[nodiscard]] Result<ProtocolEntry> protocol_by_name(std::string_view name);

} // namespace alchemy::host::net
