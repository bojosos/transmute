#pragma once

// Host area "time": clocks (docs/FX_HOST_SEAMS.md, "time").
//
// ticks()            high-resolution monotonic counter (QueryPerformanceCounter;
//                    CLOCK_MONOTONIC in ns on Linux), inline: translation and
//                    lock-wait statistics read it on every translation.
// tick_frequency()   ticks per second.
// monotonic_ms()     coarse monotonic milliseconds (GetTickCount64), for
//                    timeouts and deadlines.
// wall_time()        UTC wall clock in 100 ns units since 1601-01-01 (the
//                    FILETIME epoch the cache files store; Linux adds
//                    11644473600 s to the Unix epoch).
// process_times()    this process's creation time (wall_time units) and its
//                    kernel and user CPU time (100 ns units).
// sleep_ms()         sleeps at least `milliseconds`.

#include "fx/host/host.hpp"

#include <cstdint>

#if defined(TRANSMUTE_FX_HOST_WIN32)
#include "fx/host/win32/time.hpp"
#endif

namespace transmute::fx::host {

struct ProcessTimes {
    std::uint64_t created{};   // wall_time() units
    std::uint64_t kernel{};    // 100 ns units
    std::uint64_t user{};      // 100 ns units
};

std::uint64_t wall_time() noexcept;
bool process_times(ProcessTimes& times) noexcept;
void sleep_ms(unsigned milliseconds) noexcept;

} // namespace transmute::fx::host
