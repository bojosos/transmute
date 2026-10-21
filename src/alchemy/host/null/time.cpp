// Null host backend, area "time": every call fails with Error::unsupported
// (include/alchemy/host/host.hpp).
#include "alchemy/host/time.hpp"

namespace alchemy::host::time {

// Clocks cannot fail: the null backend has no time source and reports 0.
std::uint64_t monotonic_ns() noexcept { return 0; }
std::int64_t realtime_ns() noexcept { return 0; }
std::uint32_t tick_ms() noexcept { return static_cast<std::uint32_t>(monotonic_ns() / 1'000'000); }
void sleep_ns(std::uint64_t) noexcept {}
std::uint64_t continuous_ns() noexcept { return 0; }
Result<std::uint64_t> process_cpu_ns() { return Error::unsupported; }
Result<std::uint64_t> thread_cpu_ns() { return Error::unsupported; }

struct Timer::State {};
Result<Timer> Timer::create(std::function<void()>) { return Error::unsupported; }
Timer::Timer() = default;
Timer::Timer(Timer&&) noexcept = default;
Timer& Timer::operator=(Timer&&) noexcept = default;
Timer::~Timer() = default;
Status Timer::schedule(std::uint64_t, std::uint64_t) { return Error::unsupported; }
void Timer::cancel() noexcept {}

} // namespace alchemy::host::time
