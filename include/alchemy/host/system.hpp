#pragma once

// Host area "system" (owner: r2). The rules are in include/alchemy/host/host.hpp. Machine facts:
// CPU, memory and memory pressure, power and battery, network adapters, host and user names.
// Written by r1 for libSystem (cross-owner, agreed with r2): cpu_times, processor_times,
// machine_uuid, memory_pressure.
// Written by r3 for IOKit (cross-owner, agreed with r2): power_status, watch_sleep,
// watch_power_source.

#include "alchemy/host/host.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace alchemy::host::system {

struct Memory {
    std::uint64_t physical{}, available{};
};
[[nodiscard]] Result<Memory> memory();

// CPU time since boot (host_statistics HOST_CPU_LOAD_INFO): all processors
// together, or one record per processor (host_processor_info). system_ns
// excludes idle time.
struct CpuTimes {
    std::uint64_t user_ns{}, system_ns{}, idle_ns{};
};
[[nodiscard]] Result<CpuTimes> cpu_times();
[[nodiscard]] Result<std::vector<CpuTimes>> processor_times();

// The stable identity of this machine (gethostuuid).
[[nodiscard]] Result<std::array<std::uint8_t, 16>> machine_uuid();

// The host's memory pressure now (DISPATCH_SOURCE_TYPE_MEMORYPRESSURE):
// critical when memory is low, normal when it is plentiful, else warning.
enum class Pressure : std::uint8_t { normal, warning, critical };
[[nodiscard]] Result<Pressure> memory_pressure();

// The machine's DNS host name (gethostname, SCDynamicStoreCopyComputerName).
// `physical` asks for the computer's own name rather than the one it answers
// to on the network (they differ on a cluster's virtual server).
[[nodiscard]] Result<std::string> host_name(bool physical = true);
// The login name of the user this process runs as (getlogin, NSUserName).
[[nodiscard]] Result<std::string> user_name();

// Keeps the system out of idle sleep, the display on, or both, while it is
// held (IOPMAssertionCreateWithName; an NSProcessInfo activity).
class PowerRequest {
public:
    [[nodiscard]] static Result<PowerRequest> create(std::string_view reason, bool system, bool display);
    PowerRequest() = default;  // holds nothing
    PowerRequest(PowerRequest&& other) noexcept
        : handle_(std::exchange(other.handle_, {})), system_(other.system_), display_(other.display_) {}
    PowerRequest& operator=(PowerRequest&& other) noexcept;
    PowerRequest(const PowerRequest&) = delete;
    PowerRequest& operator=(const PowerRequest&) = delete;
    ~PowerRequest() { release(); }
    // Lets the system and display sleep again.
    void release() noexcept;

private:
    Handle handle_;
    bool system_{}, display_{};
};

// The machine's battery and power source (IOPSCopyPowerSourcesInfo).
struct PowerStatus {
    bool battery{};                    // a battery is present (the rest describe it)
    bool external_power{};             // running on AC power
    bool charging{};
    std::optional<std::uint8_t> percent;           // charge left, 0-100
    std::optional<std::uint32_t> seconds_to_empty; // on battery, when the host estimates it
};
[[nodiscard]] Result<PowerStatus> power_status();
// Calls `changed` on a host thread when the system is about to sleep (true)
// and when it has woken (false; a wake may be reported more than once).
// IORegisterForSystemPower. The registration lasts for the process.
[[nodiscard]] Status watch_sleep(std::function<void(bool sleeping)> changed);
// Calls `changed` on a host thread when the power source (AC or battery) or
// the battery charge changes (IOPSNotificationCreateRunLoopSource). The
// registration lasts for the process.
[[nodiscard]] Status watch_power_source(std::function<void()> changed);

} // namespace alchemy::host::system
