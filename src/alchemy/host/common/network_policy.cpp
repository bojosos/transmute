// The network policy (alchemy/host/network_policy.hpp), above the host
// backends: plain C++ over host::process and host::net.
#include "alchemy/host/network_policy.hpp"

#include "alchemy/host/net.hpp"
#include "alchemy/host/process.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace alchemy::network_policy {
namespace {

std::atomic<int> testing_override{-1};

Mode environment_mode() {
    const auto value = host::process::environment("ALCHEMY_NETWORK").value_or(std::string());
    if (value.empty() || value == "full") return Mode::full;
    if (value == "loopback") return Mode::loopback;
    std::fprintf(stderr, "alchemy: ALCHEMY_NETWORK=%s is not \"full\" or \"loopback\"; allowing loopback traffic only\n", value.c_str());
    return Mode::loopback;
}

std::string lower(std::string_view text) {
    std::string result(text);
    for (auto& character : result) if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
    return result;
}

// A host as the deny list compares it: lower-case, without IPv6 brackets or a trailing dot.
std::string canonical_host(std::string_view host) {
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    if (!host.empty() && host.back() == '.') host.remove_suffix(1);
    return lower(host);
}

// An address as the deny list compares it: family and bytes only, IPv4-mapped IPv6 as IPv4.
std::optional<host::net::Address> canonical_address(const host::net::Address& address) {
    if (address.family == host::net::Family::ipv4) return host::net::Address::ipv4({address.bytes[0], address.bytes[1], address.bytes[2], address.bytes[3]});
    if (address.family != host::net::Family::ipv6) return std::nullopt;
    static constexpr std::array<std::uint8_t, 12> mapped{0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};
    if (std::equal(mapped.begin(), mapped.end(), address.bytes.begin()))
        return host::net::Address::ipv4({address.bytes[12], address.bytes[13], address.bytes[14], address.bytes[15]});
    return host::net::Address::ipv6(address.bytes);
}

struct DenyList {
    std::vector<std::string> names;      // exact names
    std::vector<std::string> domains;    // "*.<domain>": ".<domain>", matched as a suffix
    std::vector<host::net::Address> addresses;
};

DenyList parse_deny_list(std::string_view text) {
    DenyList list;
    while (!text.empty()) {
        const auto end = text.find_first_of(", \t");
        auto entry = canonical_host(text.substr(0, end));
        text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
        if (entry.empty()) continue;
        if (entry.size() > 2 && entry.compare(0, 2, "*.") == 0) list.domains.push_back(entry.substr(1));
        else if (const auto address = host::net::parse_numeric(entry)) {
            list.addresses.push_back(*canonical_address(*address));
            list.names.push_back(host::net::host_text(list.addresses.back()));
        } else list.names.push_back(std::move(entry));
    }
    return list;
}

std::mutex deny_list_mutex;
std::shared_ptr<const DenyList> deny_list_override;   // tests

std::shared_ptr<const DenyList> deny_list() {
    {
        std::lock_guard lock(deny_list_mutex);
        if (deny_list_override) return deny_list_override;
    }
    static const auto value = std::make_shared<const DenyList>(
        parse_deny_list(host::process::environment("ALCHEMY_NETWORK_DENY").value_or(std::string())));
    return value;
}

void deny_line(std::string_view reason, std::string_view api, std::string_view target) {
    static std::mutex mutex;
    std::lock_guard lock(mutex);
    std::fprintf(stderr, "alchemy: network denied (%.*s): %.*s %.*s\n", static_cast<int>(reason.size()), reason.data(),
                 static_cast<int>(api.size()), api.data(), static_cast<int>(target.size()), target.data());
    std::fflush(stderr);
}

} // namespace

Mode mode() {
    const auto forced = testing_override.load(std::memory_order_relaxed);
    if (forced >= 0) return static_cast<Mode>(forced);
    static const Mode value = environment_mode();   // read once: the policy cannot change during the run
    return value;
}

void set_mode_for_testing(std::optional<Mode> value) {
    testing_override.store(value ? static_cast<int>(*value) : -1, std::memory_order_relaxed);
}

bool loopback_address(const host::net::Address& address) { return host::net::is_loopback(address); }

std::string address_text(const host::net::Address& address) {
    if (address.family == host::net::Family::ipv4 || address.family == host::net::Family::ipv6) return host::net::to_string(address);
    if (address.family == host::net::Family::local) return "(local)";
    return "(none)";
}

bool numeric_host(std::string_view host) { return host::net::parse_numeric(host).has_value(); }

bool loopback_host(std::string_view host) { return host::net::loopback_host(host); }

std::string url_host(std::string_view url) {
    const auto scheme = url.find("://");
    if (scheme == std::string_view::npos) return {};
    auto rest = url.substr(scheme + 3);
    rest = rest.substr(0, rest.find_first_of("/?#"));
    if (const auto at = rest.rfind('@'); at != std::string_view::npos) rest = rest.substr(at + 1);
    if (!rest.empty() && rest.front() == '[') {
        const auto close = rest.find(']');
        return lower(close == std::string_view::npos ? rest : rest.substr(0, close + 1));
    }
    return lower(rest.substr(0, rest.find(':')));
}

void deny(std::string_view api, std::string_view target) { deny_line("loopback-only", api, target); }

bool listed_host(std::string_view host) {
    const auto list = deny_list();
    if (list->names.empty() && list->domains.empty()) return false;
    auto name = canonical_host(host);
    if (const auto address = host::net::parse_numeric(name)) name = host::net::host_text(*canonical_address(*address));
    for (const auto& entry : list->names) if (name == entry) return true;
    for (const auto& domain : list->domains)
        if (name.size() > domain.size() && name.compare(name.size() - domain.size(), domain.size(), domain) == 0) return true;
    return false;
}

bool listed_address(const host::net::Address& address) {
    const auto list = deny_list();
    if (list->addresses.empty()) return false;
    const auto value = canonical_address(address);
    return value && std::find(list->addresses.begin(), list->addresses.end(), *value) != list->addresses.end();
}

bool refuse_listed_host(std::string_view api, std::string_view host, std::string_view target) {
    if (!listed_host(host)) return false;
    deny_line("deny list", api, target.empty() ? host : target);
    return true;
}

bool refuse_listed_address(std::string_view api, const host::net::Address& address) {
    if (!listed_address(address)) return false;
    deny_line("deny list", api, address_text(address));
    return true;
}

bool log_enabled() {
    static const bool value = host::process::environment("ALCHEMY_NETWORK_LOG").value_or(std::string()) == "1";
    return value;
}

void allowed(std::string_view api, std::string_view target) {
    if (!log_enabled()) return;
    static std::mutex mutex;
    static std::set<std::string> seen;
    std::string key(api);
    key += ' ';
    key += target;
    std::lock_guard lock(mutex);
    if (!seen.insert(key).second) return;
    std::fprintf(stderr, "alchemy: network allowed: %s\n", key.c_str());
    std::fflush(stderr);
}

void set_deny_list_for_testing(std::optional<std::string_view> list) {
    std::lock_guard lock(deny_list_mutex);
    deny_list_override = list ? std::make_shared<const DenyList>(parse_deny_list(*list)) : nullptr;
}

} // namespace alchemy::network_policy
