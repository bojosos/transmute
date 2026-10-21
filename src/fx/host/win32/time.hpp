#pragma once

// win32 backend of fx/host/time.hpp: the inline clocks.

#include "fx/host/win32/kernel32.hpp"

#include <cstdint>

namespace transmute::fx::host {

inline std::uint64_t ticks() noexcept {
    long long t;
    QueryPerformanceCounter(reinterpret_cast<_LARGE_INTEGER*>(&t));
    return static_cast<std::uint64_t>(t);
}

inline std::uint64_t tick_frequency() noexcept {
    long long f;
    QueryPerformanceFrequency(reinterpret_cast<_LARGE_INTEGER*>(&f));
    return static_cast<std::uint64_t>(f);
}

inline std::uint64_t monotonic_ms() noexcept { return GetTickCount64(); }

} // namespace transmute::fx::host
