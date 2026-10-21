#pragma once

// Host x64 feature detection (owner: integration). Emitters must check these
// before using an extension; the baseline is x86-64-v2 (SSE4.2, POPCNT).

namespace transmute::fx {

struct CpuFeatures {
    bool sse3{}, ssse3{}, sse41{}, sse42{}, popcnt{}, aes{}, pclmul{};
    bool avx{}, avx2{}, fma{}, f16c{}, bmi1{}, bmi2{}, lzcnt{}, movbe{}, sha{};
    bool avx512f{}, avx512bw{}, avx512vl{}, avx512dq{};
    bool apx{};            // reserved: 32 GPRs (not yet used)
    bool rdrand{}, rdseed{}, invariant_tsc{};
    bool os_avx{};         // OS saves YMM state (XGETBV)
    bool gfni{};           // GF2P8AFFINEQB etc. (appended by translator core)
};

// Detected once; thread-safe.
const CpuFeatures& cpu_features() noexcept;

} // namespace transmute::fx
