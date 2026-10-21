// CNTVCT_EL0 support (owner: atomics/system).
//
// CNTFRQ_EL0 reports kSystemCounterFrequency. With an invariant TSC the inline
// sequence computes (rdtsc * multiplier) >> 32, reading the multiplier through
// GuestContext::helpers[helper_system_timer_data] so translated bytes stay
// machine-independent within a process. Without one, helper_counter scales
// the host tick counter (host::ticks) to the same nominal rate.
#include "fx/translator/emit_system_internal.hpp"
#include "fx/host/time.hpp"

#include <intrin.h>

namespace transmute::fx::system_detail {
namespace {

std::uint64_t mul_div(std::uint64_t a, std::uint64_t b, std::uint64_t c) {
    std::uint64_t high;
    const std::uint64_t low = _umul128(a, b, &high);
    std::uint64_t remainder;
    return _udiv128(high, low, c, &remainder);
}

std::uint64_t measure_tsc_hz() {
    int r[4]{};
    __cpuid(r, 0);
    if (r[0] >= 0x15) {
        __cpuid(r, 0x15);
        const auto denominator = static_cast<std::uint32_t>(r[0]);
        const auto numerator = static_cast<std::uint32_t>(r[1]);
        const auto crystal = static_cast<std::uint32_t>(r[2]);
        if (denominator && numerator && crystal) {
            return static_cast<std::uint64_t>(crystal) * numerator / denominator;
        }
    }
    const std::uint64_t frequency = host::tick_frequency();
    // Best of three 2 ms windows against the host tick counter.
    std::uint64_t best = 0;
    for (int attempt = 0; attempt < 3; ++attempt) {
        const std::uint64_t start = host::ticks();
        const auto tsc0 = __rdtsc();
        const auto target = start + frequency / 500;
        std::uint64_t now;
        do { now = host::ticks(); } while (now < target);
        const auto tsc1 = __rdtsc();
        const auto hz = mul_div(tsc1 - tsc0, frequency, now - start);
        if (hz > best) best = hz;
    }
    return best;
}

struct alignas(64) TimerData {
    std::uint64_t multiplier;
};

TimerData& timer_data() {
    static TimerData data = [] {
        TimerData d{};
        const auto hz = measure_tsc_hz();
        if (hz >= kSystemCounterFrequency) d.multiplier = (kSystemCounterFrequency << 32) / hz;
        return d;
    }();
    return data;
}

} // namespace

std::uint64_t timer_multiplier() { return timer_data().multiplier; }

const void* timer_data_address() { return &timer_data(); }

std::uint64_t helper_counter(GuestContext*) {
    static const std::uint64_t frequency = host::tick_frequency();
    return mul_div(host::ticks(), kSystemCounterFrequency, frequency);
}

} // namespace transmute::fx::system_detail
