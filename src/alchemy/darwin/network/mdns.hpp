#pragma once

// Host multicast DNS service discovery (owner: v-netsec). Host only: no
// guest calls, so the functions below may run on any thread. The guest
// APIs over it are dns_sd (dns_sd.cpp, libSystem's DNSService* / TXTRecord*)
// and NSNetService / NSNetServiceBrowser (net_service.cpp).
//
// Backend: the host's DNS-SD client (host::dns_sd: on Windows dnsapi.dll's
// DnsServiceBrowse, DnsServiceResolve, DnsServiceRegister, Windows 10 1809+);
// a host without one reports kDNSServiceErr_ServiceNotRunning. Only the link-local "local." domain exists (no wide-area
// DNS-SD). Removal of a browsed service is the responder's goodbye (a PTR
// record with TTL 0). TXT records cross the Windows API as UTF-16 key/value
// strings, so values must be UTF-8 text (binary values are not preserved).
// Address lookups (GetAddrInfo) use the host resolver, which answers ".local"
// names over mDNS.

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace alchemy::gui::network::mdns {

using Bytes = std::vector<std::uint8_t>;

// dns_sd.h kDNSServiceErr_* values (the NSNetService provider maps them).
namespace error {
inline constexpr std::int32_t none = 0, unknown = -65537, no_such_name = -65538, no_memory = -65539, bad_param = -65540,
    bad_reference = -65541, bad_state = -65542, bad_flags = -65543, unsupported = -65544, not_initialized = -65545,
    already_registered = -65547, name_conflict = -65548, invalid = -65549, incompatible = -65551,
    bad_interface_index = -65552, refused = -65553, no_such_record = -65554, no_auth = -65555, no_such_key = -65556,
    service_not_running = -65563, timeout = -65568;
} // namespace error

struct Event {
    enum class Kind { found, lost, domain, registered, resolved, address, failed };
    Kind kind{Kind::failed};
    std::int32_t error{};             // kDNSServiceErr_* (failed)
    bool more_coming{};               // more events of the same batch follow
    std::uint32_t interface_index{};
    std::string name;                 // service instance name (unescaped), or the domain for Kind::domain
    std::string type;                 // "_qlab._tcp."
    std::string domain;               // "local."
    std::string host;                 // target host "name.local." (resolved) / the queried host (address)
    std::uint16_t port{};             // host byte order
    Bytes txt;                        // TXT record in wire format (resolved)
    std::vector<Bytes> addresses;     // Darwin sockaddr_in / sockaddr_in6 bytes with the service port (resolved, address)
    std::uint32_t ttl{};              // address
};

// Receives events on a host thread (the DNS-SD client's callback thread or a worker).
// It must not call guest code.
using Sink = std::function<void(Event)>;

// A running operation. Destroying it (or cancel()) stops it: no event reaches
// the sink after cancel() returns. Registered services are withdrawn.
class Operation {
public:
    virtual ~Operation() = default;
    virtual void cancel() = 0;
};

// kDNSServiceErr_none when the host's DNS-SD client is available.
[[nodiscard]] std::int32_t available();

// "_qlab._tcp" / "_qlab._tcp." -> "_qlab._tcp."; nullopt when not "_name._tcp|_udp"
// (subtypes "_sub,_qlab._tcp" are not supported).
[[nodiscard]] std::optional<std::string> normalize_type(std::string_view type);
// True for "", "local" and "local." (case-insensitive).
[[nodiscard]] bool is_local_domain(std::string_view domain);
// dns_sd escaping of a service instance label ('.' and '\' escaped with '\',
// control bytes as \DDD).
[[nodiscard]] std::string escape_label(std::string_view label);
// The machine's mDNS host name ("<dns host name>.local.").
[[nodiscard]] std::string local_host_name();
// The default service name for registrations (the computer's NetBIOS-free
// display name: the DNS host label).
[[nodiscard]] std::string default_service_name();

// TXT wire format <-> key/value pairs ("key" without '=' has no value).
struct TxtEntry { std::string key; std::optional<Bytes> value; };
[[nodiscard]] std::optional<std::vector<TxtEntry>> parse_txt(std::span<const std::uint8_t> wire);
[[nodiscard]] Bytes build_txt(const std::vector<TxtEntry>& entries);

// The operations return nullptr and set `status` (kDNSServiceErr_*) when they
// cannot start. `interface_index` 0 means every interface.
std::unique_ptr<Operation> browse(const std::string& type, std::uint32_t interface_index, Sink sink, std::int32_t& status);
// One "local." domain event (Bonjour's only browse/registration domain here).
std::unique_ptr<Operation> browse_domains(Sink sink, std::int32_t& status);
std::unique_ptr<Operation> resolve(const std::string& name, const std::string& type, std::uint32_t interface_index, Sink sink,
                                   std::int32_t& status);
// `name` empty registers under default_service_name(). `port` in host order.
std::unique_ptr<Operation> register_service(const std::string& name, const std::string& type, std::uint16_t port,
                                            std::span<const std::uint8_t> txt, std::uint32_t interface_index, Sink sink,
                                            std::int32_t& status);
// protocols: bit 0 IPv4, bit 1 IPv6 (0 = both).
std::unique_ptr<Operation> get_addr_info(const std::string& host, std::uint32_t protocols, std::uint32_t interface_index,
                                         Sink sink, std::int32_t& status);

} // namespace alchemy::gui::network::mdns
