// Null host backend, area "dns_sd": no DNS-SD client, so every request fails
// with Failure::not_running (include/alchemy/host/host.hpp).
#include "alchemy/host/dns_sd.hpp"

namespace alchemy::host::dns_sd {

struct Operation::State {};

Operation::Operation() = default;
Operation::Operation(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}
Operation::Operation(Operation&&) noexcept = default;
Operation& Operation::operator=(Operation&&) noexcept = default;
Operation::~Operation() = default;
void Operation::cancel() noexcept { state_.reset(); }

Failure available() { return Failure::not_running; }
Operation browse(std::string_view, std::uint32_t, BrowseCallback, Failure& failure) { failure = Failure::not_running; return {}; }
Operation browse_domains(std::function<void()>, Failure& failure) { failure = Failure::not_running; return {}; }
Operation resolve(std::string_view, std::uint32_t, ResolveCallback, Failure& failure) { failure = Failure::not_running; return {}; }
Operation register_service(const Registration&, RegisterCallback, Failure& failure) { failure = Failure::not_running; return {}; }
Operation get_addr_info(std::string_view, std::uint32_t, AddressCallback, Failure& failure) { failure = Failure::not_running; return {}; }

} // namespace alchemy::host::dns_sd
