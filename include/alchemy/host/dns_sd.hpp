#pragma once

// Host area "dns_sd" (owner: r2). The rules are in include/alchemy/host/host.hpp. DNS service
// discovery on the link-local "local." domain (Bonjour): browsing, resolving and registering
// services, and address lookups. The Windows DNS-SD client (dnsapi) today; Avahi on Linux.
//
// Names are UTF-8. A service instance's full name is "<instance>.<type>local" with the instance
// label escaped as dns_sd escapes it ("\." "\\" "\DDD"). Every request reports to its callback
// on a host thread; destroying its Operation (or cancel()) stops it, and no callback runs after
// cancel() returns. A registered service is withdrawn when its Operation ends. The network
// policy (network_policy.hpp) is the caller's to check.

#include "alchemy/host/host.hpp"
#include "alchemy/host/net.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace alchemy::host::dns_sd {

// Why a request failed or could not start.
enum class Failure : std::uint8_t {
    none,
    not_running,     // the host has no DNS-SD client
    no_such_record,
    timeout,
    bad_param,
    no_memory,
    refused,
    name_conflict,
    unknown,
};

// Failure::none when the host's DNS-SD client is present.
[[nodiscard]] Failure available();

// A running request.
class Operation {
public:
    Operation();  // no request
    Operation(Operation&&) noexcept;
    Operation& operator=(Operation&&) noexcept;
    Operation(const Operation&) = delete;
    Operation& operator=(const Operation&) = delete;
    ~Operation();
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(state_); }
    void cancel() noexcept;

    struct State;
    explicit Operation(std::shared_ptr<State> state) noexcept;

private:
    std::shared_ptr<State> state_;
};

// TXT properties as the host carries them: a key, and a text value or none.
using Properties = std::vector<std::pair<std::string, std::optional<std::string>>>;

// Browsing `query` ("_qlab._tcp.local"): each callback is one batch of the
// instances' full names; `goodbye` marks a departing instance (TTL 0).
struct Pointer {
    std::string target;
    bool goodbye{};
};
using BrowseCallback = std::function<void(Failure failure, const std::vector<Pointer>& pointers)>;
[[nodiscard]] Operation browse(std::string_view query, std::uint32_t interface_index, BrowseCallback callback, Failure& failure);

// The one "local." domain, reported once.
[[nodiscard]] Operation browse_domains(std::function<void()> callback, Failure& failure);

// Resolving a service instance by its full name: one callback, with the
// instance on success.
struct Instance {
    std::string host;                          // "name.local" (no trailing dot)
    std::uint16_t port{};                      // host byte order
    std::uint32_t interface_index{};           // 0: unknown
    Properties properties;
    std::optional<std::array<std::uint8_t, 4>> ipv4;
    std::optional<std::array<std::uint8_t, 16>> ipv6;
};
using ResolveCallback = std::function<void(Failure failure, const Instance* instance)>;
[[nodiscard]] Operation resolve(std::string_view full_name, std::uint32_t interface_index, ResolveCallback callback, Failure& failure);

// Registering a service: the callback reports the registered full name (the
// responder may rename it) or the failure.
struct Registration {
    std::string full_name;
    std::string host;                          // the target host ("name.local")
    std::uint16_t port{};                      // host byte order
    std::uint32_t interface_index{};           // 0: every interface
    Properties properties;
};
using RegisterCallback = std::function<void(Failure failure, std::string_view registered_name)>;
[[nodiscard]] Operation register_service(const Registration& registration, RegisterCallback callback, Failure& failure);

// The addresses of `host` (".local" names over mDNS), without duplicates;
// `protocols` bit 0 IPv4, bit 1 IPv6 (0: both). Empty when none was found.
using AddressCallback = std::function<void(const std::vector<net::Address>& addresses)>;
[[nodiscard]] Operation get_addr_info(std::string_view host, std::uint32_t protocols, AddressCallback callback, Failure& failure);

} // namespace alchemy::host::dns_sd
