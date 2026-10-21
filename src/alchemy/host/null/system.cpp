// Null host backend, area "system": every call fails with Error::unsupported
// (include/alchemy/host/host.hpp).
#include "alchemy/host/system.hpp"

namespace alchemy::host::system {

Result<Memory> memory() { return Error::unsupported; }
Result<CpuTimes> cpu_times() { return Error::unsupported; }
Result<std::vector<CpuTimes>> processor_times() { return Error::unsupported; }
Result<std::array<std::uint8_t, 16>> machine_uuid() { return Error::unsupported; }
Result<Pressure> memory_pressure() { return Error::unsupported; }
Result<std::string> host_name(bool) { return Error::unsupported; }
Result<std::string> user_name() { return Error::unsupported; }
Result<PowerRequest> PowerRequest::create(std::string_view, bool, bool) { return Error::unsupported; }
PowerRequest& PowerRequest::operator=(PowerRequest&& other) noexcept {
    handle_ = std::exchange(other.handle_, {});
    return *this;
}
void PowerRequest::release() noexcept { handle_ = {}; }
Result<PowerStatus> power_status() { return Error::unsupported; }
Status watch_sleep(std::function<void(bool)>) { return Error::unsupported; }
Status watch_power_source(std::function<void()>) { return Error::unsupported; }

} // namespace alchemy::host::system
