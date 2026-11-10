#pragma once

// linux backend of fx/host/time.hpp: the inline clocks (vDSO clock_gettime).

#include <time.h>

#include <cstdint>

namespace transmute::fx::host {

inline std::uint64_t ticks() noexcept {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::uint64_t>(now.tv_sec) * 1000000000u + static_cast<std::uint64_t>(now.tv_nsec);
}

inline std::uint64_t tick_frequency() noexcept { return 1000000000u; }

inline std::uint64_t monotonic_ms() noexcept {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC_COARSE, &now);
    return static_cast<std::uint64_t>(now.tv_sec) * 1000u + static_cast<std::uint64_t>(now.tv_nsec) / 1000000u;
}

} // namespace transmute::fx::host
