#pragma once

// x86-64 intrinsics with one spelling for MSVC and GCC/Clang. MSVC's
// <intrin.h> names (__cpuid, _umul128, _InterlockedCompareExchange128, ...)
// do not exist, or mean something else, under GCC and Clang; code that needs
// them on every host compiler includes this header instead.
//   cpuid(out, leaf, subleaf)        CPUID (eax, ebx, ecx, edx in out[0..3])
//   xgetbv0()                        XCR0
//   multiply_128(a, b, high)         a * b: low 64 bits, *high the upper 64
//   divide_128(high, low, d, rem)    (high:low) / d, as DIV (the quotient
//                                    must fit 64 bits)
//   compare_exchange_128(...)        LOCK CMPXCHG16B
//   read_tsc()                       RDTSC

#include <cstdint>

#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <cpuid.h>
#include <x86intrin.h>
#endif

namespace transmute {

inline void cpuid(int out[4], int leaf, int subleaf = 0) noexcept {
#if defined(_MSC_VER)
    __cpuidex(out, leaf, subleaf);
#else
    unsigned a, b, c, d;
    __cpuid_count(static_cast<unsigned>(leaf), static_cast<unsigned>(subleaf), a, b, c, d);
    out[0] = static_cast<int>(a);
    out[1] = static_cast<int>(b);
    out[2] = static_cast<int>(c);
    out[3] = static_cast<int>(d);
#endif
}

// Only after CPUID.1:ECX.OSXSAVE says XGETBV is available.
inline std::uint64_t xgetbv0() noexcept {
#if defined(_MSC_VER)
    return _xgetbv(0);
#else
    unsigned low, high;
    __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
    return (static_cast<std::uint64_t>(high) << 32) | low;
#endif
}

inline std::uint64_t multiply_128(std::uint64_t a, std::uint64_t b, std::uint64_t* high) noexcept {
#if defined(_MSC_VER)
    unsigned long long upper;
    const auto low = _umul128(a, b, &upper);
    *high = upper;
    return low;
#else
    const auto product = static_cast<unsigned __int128>(a) * b;
    *high = static_cast<std::uint64_t>(product >> 64);
    return static_cast<std::uint64_t>(product);
#endif
}

inline std::uint64_t divide_128(std::uint64_t high, std::uint64_t low, std::uint64_t divisor,
                                std::uint64_t* remainder) noexcept {
#if defined(_MSC_VER)
    unsigned long long rest;
    const auto quotient = _udiv128(high, low, divisor, &rest);
    *remainder = rest;
    return quotient;
#else
    std::uint64_t quotient, rest;
    __asm__("divq %4" : "=a"(quotient), "=d"(rest) : "a"(low), "d"(high), "rm"(divisor) : "cc");
    *remainder = rest;
    return quotient;
#endif
}

// Compares the 16 bytes at `target` (16-byte aligned) with expected[0] (low)
// and expected[1] (high); stores desired_low/desired_high if equal. Always
// leaves the old memory value in `expected`.
inline bool compare_exchange_128(volatile std::int64_t* target, std::int64_t desired_high, std::int64_t desired_low,
                                 std::int64_t expected[2]) noexcept {
#if defined(_MSC_VER)
    return _InterlockedCompareExchange128(reinterpret_cast<volatile long long*>(target), desired_high, desired_low,
                                          reinterpret_cast<long long*>(expected)) != 0;
#else
    bool equal;
    __asm__ volatile("lock cmpxchg16b %1"
                     : "=@ccz"(equal), "+m"(*target), "+a"(expected[0]), "+d"(expected[1])
                     : "b"(desired_low), "c"(desired_high)
                     : "memory");
    return equal;
#endif
}

inline std::uint64_t read_tsc() noexcept { return __rdtsc(); }

} // namespace transmute
