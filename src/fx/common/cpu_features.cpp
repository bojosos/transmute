#include "fx/common/cpu_features.hpp"

#include <intrin.h>
#include <immintrin.h>

namespace transmute::fx {
namespace {
CpuFeatures detect() noexcept {
    CpuFeatures f;
    int r[4]{};
    __cpuid(r, 0);
    const int max_leaf = r[0];
    __cpuid(r, 1);
    const unsigned ecx1 = static_cast<unsigned>(r[2]);
    f.sse3 = ecx1 & (1u << 0);
    f.pclmul = ecx1 & (1u << 1);
    f.ssse3 = ecx1 & (1u << 9);
    f.fma = ecx1 & (1u << 12);
    f.sse41 = ecx1 & (1u << 19);
    f.sse42 = ecx1 & (1u << 20);
    f.movbe = ecx1 & (1u << 22);
    f.popcnt = ecx1 & (1u << 23);
    f.aes = ecx1 & (1u << 25);
    f.rdrand = ecx1 & (1u << 30);
    const bool osxsave = ecx1 & (1u << 27);
    f.avx = ecx1 & (1u << 28);
    f.f16c = ecx1 & (1u << 29);
    unsigned long long xcr0 = 0;
    if (osxsave) xcr0 = _xgetbv(0);
    f.os_avx = (xcr0 & 6) == 6;
    const bool os_avx512 = (xcr0 & 0xe6) == 0xe6;
    if (max_leaf >= 7) {
        __cpuidex(r, 7, 0);
        const unsigned ebx7 = static_cast<unsigned>(r[1]);
        const unsigned edx7 = static_cast<unsigned>(r[3]);
        f.bmi1 = ebx7 & (1u << 3);
        f.avx2 = (ebx7 & (1u << 5)) && f.os_avx;
        f.bmi2 = ebx7 & (1u << 8);
        f.avx512f = (ebx7 & (1u << 16)) && os_avx512;
        f.avx512dq = (ebx7 & (1u << 17)) && os_avx512;
        f.sha = ebx7 & (1u << 29);
        f.rdseed = ebx7 & (1u << 18);
        f.gfni = (static_cast<unsigned>(r[2]) & (1u << 8)) != 0;   // CPUID.7.0:ECX[8]
        f.avx512bw = (ebx7 & (1u << 30)) && os_avx512;
        f.avx512vl = (ebx7 & (1u << 31)) && os_avx512;
        __cpuidex(r, 7, 1);
        f.apx = (static_cast<unsigned>(r[3]) & (1u << 21)) != 0;
        (void)edx7;
    }
    if (!f.os_avx) { f.avx = false; f.fma = false; f.f16c = false; }
    __cpuid(r, static_cast<int>(0x80000000u));
    if (static_cast<unsigned>(r[0]) >= 0x80000001u) {
        __cpuid(r, static_cast<int>(0x80000001u));
        f.lzcnt = static_cast<unsigned>(r[2]) & (1u << 5);
    }
    __cpuid(r, static_cast<int>(0x80000000u));
    if (static_cast<unsigned>(r[0]) >= 0x80000007u) {
        __cpuid(r, static_cast<int>(0x80000007u));
        f.invariant_tsc = static_cast<unsigned>(r[3]) & (1u << 8);
    }
    return f;
}
} // namespace

const CpuFeatures& cpu_features() noexcept {
    static const CpuFeatures features = detect();
    return features;
}

} // namespace transmute::fx
