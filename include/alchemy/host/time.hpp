#pragma once

// Host area "time" (owner: r1). The rules are in include/alchemy/host/host.hpp. Clocks and timers.
// Written by r2 for NSNetService timeouts (cross-owner, agreed with r1): Timer.

#include "alchemy/host/host.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace alchemy::host::time {

// Monotonic time in nanoseconds, not counting suspend (mach_absolute_time).
[[nodiscard]] std::uint64_t monotonic_ns() noexcept;
// The host's millisecond tick count (wraps every 49.7 days), the clock of the
// host window-message timestamps.
[[nodiscard]] std::uint32_t tick_ms() noexcept;
// Wall-clock time in nanoseconds since 1970-01-01 UTC.
[[nodiscard]] std::int64_t realtime_ns() noexcept;
// Sleeps for `duration` and wakes within about a hundred microseconds of it
// where the host has high-resolution timers (Darwin's nanosleep and usleep
// do; a millisecond-granular sleep is far too coarse for idle loops).
void sleep_ns(std::uint64_t duration) noexcept;
// Monotonic time in nanoseconds, counting time asleep (mach_continuous_time).
[[nodiscard]] std::uint64_t continuous_ns() noexcept;
// CPU time (user + system) in nanoseconds of this process and of the calling
// thread (CLOCK_PROCESS_CPUTIME_ID, CLOCK_THREAD_CPUTIME_ID).
[[nodiscard]] Result<std::uint64_t> process_cpu_ns();
[[nodiscard]] Result<std::uint64_t> thread_cpu_ns();

// A timer on the host's thread pool (dispatch_source timers, CFRunLoopTimer
// timeouts): its callback runs on a pool thread. Destroying it cancels it.
class Timer {
public:
    [[nodiscard]] static Result<Timer> create(std::function<void()> callback);
    Timer();  // no timer
    Timer(Timer&&) noexcept;
    Timer& operator=(Timer&&) noexcept;
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    ~Timer();
    [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(state_); }

    // Fires `due_ns` from now, then every `period_ns` (0: once); replaces an earlier schedule.
    [[nodiscard]] Status schedule(std::uint64_t due_ns, std::uint64_t period_ns = 0);
    // Stops the timer and waits for a running callback to return (never call it from the callback).
    void cancel() noexcept;

    struct State;

private:
    std::unique_ptr<State> state_;
};

} // namespace alchemy::host::time
