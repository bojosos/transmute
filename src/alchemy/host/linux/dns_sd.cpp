// Linux host backend, area "dns_sd" (include/alchemy/host/dns_sd.hpp).
//
// Address lookups go through the host resolver (getaddrinfo on a worker thread; ".local" names
// resolve over mDNS where the host's NSS has nss-mdns). Service discovery (browse, resolve,
// register, the local. domain) needs an mDNS responder, which this backend does not speak yet
// (Avahi's client library is the next step): those report Failure::not_running, and available()
// says so, as on a host without a DNS-SD client.
//
// As on Windows, a request's callback runs under its state's mutex, and cancel() clears the
// callback under that mutex, so none runs once cancel() has returned.
#include "alchemy/host/dns_sd.hpp"

#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <thread>

namespace alchemy::host::dns_sd {

struct Operation::State {
    std::mutex mutex;
    AddressCallback on_addresses;   // cleared by cancel
};

namespace {

using State = Operation::State;

std::string without_dot(std::string_view host) {
    if (!host.empty() && host.back() == '.') host.remove_suffix(1);
    return std::string(host);
}

} // namespace

Operation::Operation() = default;
Operation::Operation(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}
Operation::Operation(Operation&&) noexcept = default;
Operation& Operation::operator=(Operation&& other) noexcept {
    if (this != &other) {
        cancel();
        state_ = std::move(other.state_);
    }
    return *this;
}
Operation::~Operation() { cancel(); }

void Operation::cancel() noexcept {
    const auto state = std::move(state_);
    if (!state) return;
    std::lock_guard lock(state->mutex);
    state->on_addresses = nullptr;
}

Failure available() { return Failure::not_running; }
Operation browse(std::string_view, std::uint32_t, BrowseCallback, Failure& failure) { failure = Failure::not_running; return {}; }
Operation browse_domains(std::function<void()>, Failure& failure) { failure = Failure::not_running; return {}; }
Operation resolve(std::string_view, std::uint32_t, ResolveCallback, Failure& failure) { failure = Failure::not_running; return {}; }
Operation register_service(const Registration&, RegisterCallback, Failure& failure) { failure = Failure::not_running; return {}; }

Operation get_addr_info(std::string_view host, std::uint32_t protocols, AddressCallback callback, Failure& failure) {
    failure = Failure::none;
    if (!callback) { failure = Failure::bad_param; return {}; }
    auto state = std::make_shared<State>();
    state->on_addresses = std::move(callback);
    const std::weak_ptr<State> weak = state;
    try {
        std::thread([weak, query = without_dot(host), protocols] {
            addrinfo hints{};
            hints.ai_family = (protocols & 3) == 1 ? AF_INET : (protocols & 3) == 2 ? AF_INET6 : AF_UNSPEC;
            hints.ai_socktype = SOCK_STREAM;
            addrinfo* list = nullptr;
            const auto error = getaddrinfo(query.c_str(), nullptr, &hints, &list);
            std::vector<net::Address> addresses;
            for (auto* item = error == 0 ? list : nullptr; item; item = item->ai_next) {
                net::Address address;
                if (item->ai_family == AF_INET) {
                    std::array<std::uint8_t, 4> bytes{};
                    std::memcpy(bytes.data(), &reinterpret_cast<const sockaddr_in*>(item->ai_addr)->sin_addr, 4);
                    address = net::Address::ipv4(bytes);
                } else if (item->ai_family == AF_INET6) {
                    const auto* v6 = reinterpret_cast<const sockaddr_in6*>(item->ai_addr);
                    std::array<std::uint8_t, 16> bytes{};
                    std::memcpy(bytes.data(), &v6->sin6_addr, 16);
                    address = net::Address::ipv6(bytes, 0, v6->sin6_scope_id);
                } else {
                    continue;
                }
                if (std::find(addresses.begin(), addresses.end(), address) == addresses.end()) addresses.push_back(std::move(address));
            }
            if (list) freeaddrinfo(list);
            const auto alive = weak.lock();
            if (!alive) return;
            std::lock_guard lock(alive->mutex);
            if (alive->on_addresses) alive->on_addresses(addresses);
        }).detach();
    } catch (const std::system_error&) {
        failure = Failure::no_memory;
        return {};
    }
    return Operation(state);
}

} // namespace alchemy::host::dns_sd
