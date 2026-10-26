// fp family tests: x64 instruction counts of hot forms (owner: fp).
#include "fx/translator/emit_fp_tests.hpp"

#include <cstdio>
#include <immintrin.h>

using namespace transmute::fx;
using namespace transmute::fx::fptest;

namespace {

struct Hot {
    const char* name;
    std::uint32_t word;
    unsigned avx_limit;    // with AVX/FMA (0 = not checked)
    unsigned zero_limit;   // ... when the sources are known zero above the scalar (0 = avx_limit)
};

void report(const Hot* items, std::size_t count, bool check) {
    for (std::size_t i = 0; i < count; ++i) {
        const auto& h = items[i];
        const unsigned n = emitted_instructions(h.word, true);
        const unsigned total = emitted_instructions(h.word, false);
        const unsigned z = emitted_instructions(h.word, true, true);
        std::printf("    %-34s 0x%08x  %2u on the common path (%2u emitted, %2u zero-upper sources)\n", h.name,
                    h.word, n, total, z);
        if (check && h.avx_limit) {
            FX_CHECK(n != 0);
            const unsigned zl = h.zero_limit ? h.zero_limit : h.avx_limit;
            if (n > h.avx_limit || z > zl) {
                char text[160];
                std::snprintf(text, sizeof(text), "%s: %u instructions (limit %u), %u with zero-upper sources (limit %u)",
                              h.name, n, h.avx_limit, z, zl);
                testing::report_failure(__FILE__, __LINE__, text);
            }
        }
    }
}

// rd == rn aliasing (v0 = v0 op v1) and distinct registers.
const Hot kHot[] = {
    {"fadd v2.4s, v0.4s, v1.4s", enc::v3(1, 0, 0, 0x1a, 2, 0, 1), 1},
    {"fadd v0.4s, v0.4s, v1.4s", enc::v3(1, 0, 0, 0x1a, 0, 0, 1), 1},
    {"fadd v2.2d, v0.2d, v1.2d", enc::v3(1, 0, 1, 0x1a, 2, 0, 1), 1},
    {"fadd v2.2s, v0.2s, v1.2s", enc::v3(0, 0, 0, 0x1a, 2, 0, 1), 2, 1},
    {"fmul v2.4s, v0.4s, v1.4s", enc::v3(1, 1, 0, 0x1b, 2, 0, 1), 1},
    {"fmul v2.4s, v0.4s, v1.s[1]", enc::velem(1, 0, 2, 1, 0, 1, 9, 0, 2, 0), 2},
    {"fmla v2.4s, v0.4s, v1.4s", enc::v3(1, 0, 0, 0x19, 2, 0, 1), 1},
    {"fmla v0.4s, v0.4s, v1.4s", enc::v3(1, 0, 0, 0x19, 0, 0, 1), 1},
    {"fmla v2.4s, v0.4s, v1.s[3]", enc::velem(1, 0, 2, 1, 0, 1, 1, 1, 2, 0), 2},
    {"fmla v2.2d, v0.2d, v1.2d", enc::v3(1, 0, 1, 0x19, 2, 0, 1), 1},
    {"fmls v2.4s, v0.4s, v1.4s", enc::v3(1, 0, 2, 0x19, 2, 0, 1), 1},
    {"fadd s2, s0, s1", enc::fp2(2, 0, 2, 0, 1), 2, 1},
    {"fadd d2, d0, d1", enc::fp2(2, 1, 2, 0, 1), 2, 1},
    {"fmul d0, d0, d1", enc::fp2(0, 1, 0, 0, 1), 2, 1},
    {"fmadd s3, s0, s1, s2", enc::fp3(0, 0, 0, 3, 0, 1, 2), 2, 2},
    {"fmadd d3, d0, d1, d2", enc::fp3(0, 0, 1, 3, 0, 1, 2), 2, 2},
    {"fmadd d2, d0, d1, d2", enc::fp3(0, 0, 1, 2, 0, 1, 2), 2, 1},
    {"fmsub d3, d0, d1, d2", enc::fp3(0, 1, 1, 3, 0, 1, 2), 2, 2},
    {"fcmp s0, s1", enc::fcmp(0, 0, 1, 0), 8},
    {"fcmp d0, #0.0", enc::fcmp(1, 0, 0, 8), 9},
    {"fcsel s2, s0, s1, gt", enc::fcsel(0, 2, 0, 1, 12), 4},
    {"fcsel d2, d0, d1, mi", enc::fcsel(1, 2, 0, 1, 4), 4},
    {"scvtf s0, w1", enc::fpint(0, 0, 0, 2, 0, 1), 2},
    {"scvtf d0, x1", enc::fpint(1, 1, 0, 2, 0, 1), 2},
    {"ucvtf d0, w1", enc::fpint(0, 1, 0, 3, 0, 1), 3},
    {"ucvtf d0, x1", enc::fpint(1, 1, 0, 3, 0, 1), 4},
    {"fcvtzs w0, s1", enc::fpint(0, 0, 3, 0, 0, 1), 3},
    {"fcvtzs x0, d1", enc::fpint(1, 1, 3, 0, 0, 1), 3},
    {"fcvtzu w0, d1", enc::fpint(0, 1, 3, 1, 0, 1), 4},
    {"scvtf v0.4s, v1.4s", enc::v2(1, 0, 0, 0x1d, 0, 1), 1},
    {"fcvtzs v0.4s, v1.4s", enc::v2(1, 0, 2, 0x1b, 0, 1), 5},
    {"fmov s0, w1", enc::fpint(0, 0, 0, 7, 0, 1), 1},
    {"fmov d0, d1", enc::fp1(0, 1, 0, 1), 1},
    {"fneg v0.4s, v1.4s", enc::v2(1, 1, 2, 0x0f, 0, 1), 1},
    {"fabs d0, d1", enc::fp1(1, 1, 0, 1), 1},
    // NaN-exact min/max: numeric 3 + unordered test, MOVMSKPS, JRCXZ (the fixup runs on NaN lanes only).
    {"fmax v2.4s, v0.4s, v1.4s", enc::v3(1, 0, 0, 0x1e, 2, 0, 1), 7},
    {"fmaxnm v2.4s, v0.4s, v1.4s", enc::v3(1, 0, 0, 0x18, 2, 0, 1), 7},
    {"fminnm d2, d0, d1", enc::fp2(7, 1, 2, 0, 1), 9, 8},
    {"fsqrt v0.4s, v1.4s", enc::v2(1, 1, 2, 0x1f, 0, 1), 1},
    {"frintm d0, d1", enc::fp1(10, 1, 0, 1), 2, 1},
    {"fcvt d0, s1", enc::fp1(5, 0, 0, 1), 2, 1},
    {"faddp v2.4s, v0.4s, v1.4s", enc::v3(1, 1, 0, 0x1a, 2, 0, 1), 1},
    {"fcmgt v2.4s, v0.4s, v1.4s", enc::v3(1, 1, 2, 0x1c, 2, 0, 1), 1},
    {"fccmp s0, s1, #0, ne", enc::fccmp(0, 0, 1, 1, 0, 0), 0},
    {"fcvtzu v0.4s, v1.4s", enc::v2(1, 1, 2, 0x1b, 0, 1), 0},
    {"ucvtf v0.4s, v1.4s", enc::v2(1, 1, 0, 0x1d, 0, 1), 5},
    {"ucvtf v0.2s, v1.2s", enc::v2(0, 1, 0, 0x1d, 0, 1), 6, 5},
    {"scvtf v0.2s, v1.2s", enc::v2(0, 0, 0, 0x1d, 0, 1), 3, 1},
    {"scvtf s0, s1", enc::s2(0, 0, 0x1d, 0, 1), 2, 1},
    {"ucvtf s0, s1", enc::s2(1, 0, 0x1d, 0, 1), 3},
    {"fmov s0, #1.0", enc::fmov_imm(0, 0, 0x70), 1},
    {"fmov d0, #-2.0", enc::fmov_imm(1, 0, 0x80), 1},
    {"fmov v0.2s, #1.0", enc::vfmov(0, 0, 0, 0x70, 0), 1},
    {"fnmul s2, s0, s1", enc::fp2(8, 0, 2, 0, 1), 3, 2},
    {"fabd s2, s0, s1", enc::s3(1, 2, 0x1a, 2, 0, 1), 2},
    {"fabd v2.4s, v0.4s, v1.4s", enc::v3(1, 1, 2, 0x1a, 2, 0, 1), 2},
    {"facgt v2.4s, v0.4s, v1.4s", enc::v3(1, 1, 2, 0x1d, 2, 0, 1), 3},
    {"facge s2, s0, s1", enc::s3(1, 0, 0x1d, 2, 0, 1), 3},
    {"fcvtzs v0.2d, v1.2d", enc::v2(1, 0, 3, 0x1b, 0, 1), 0},
    {"frinta v0.4s, v1.4s", enc::v2(1, 1, 0, 0x18, 0, 1), 0},
    {"fcvtas w0, s1", enc::fpint(0, 0, 0, 4, 0, 1), 0},
    {"frecpe v0.4s, v1.4s", enc::v2(1, 0, 2, 0x1d, 0, 1), 0},
    {"frsqrte v0.4s, v1.4s", enc::v2(1, 1, 2, 0x1d, 0, 1), 0},
    {"frecps v2.4s, v0.4s, v1.4s", enc::v3(1, 0, 0, 0x1f, 2, 0, 1), 0},
    {"fmulx v2.4s, v0.4s, v1.4s", enc::v3(1, 0, 0, 0x1b, 2, 0, 1), 0},
    {"fmaxv s0, v1.4s", enc::vacross(1, 1, 0, 0x0f, 0, 1), 0},
    {"frint32x v0.4s, v1.4s", 0x6e21e820u, 5},
    {"frint32z s0, s1", 0x1e284020u, 6, 5},
    {"frint64z s0, s1", 0x1e294020u, 8, 7},
    {"frint32x d0, d1", 0x1e68c020u, 6, 5},
    {"fmlal v2.4s, v0.4h, v1.4h", 0x4e21ec02u, 3},
    {"fmlal2 v2.4s, v0.4h, v1.4h", 0x6e21cc02u, 5},
    {"fmlsl v2.2s, v0.2h, v1.2h", 0x0ea1ec02u, 7},
    {"fmlal v2.4s, v0.4h, v1.h[5]", 0x4f910802u, 5},
    {"fadd v2.8h, v0.8h, v1.8h", 0x4e411402u, 0},
    {"fmla v2.8h, v0.8h, v1.8h", 0x4e410c02u, 0},
};

} // namespace

FX_TEST(fp_hot_form_instruction_counts) {
    const auto host = fp_host_features();
    if (!host.avx || !host.fma) {
        std::printf("    host lacks AVX/FMA: counts reported, not checked\n");
    }
    report(kHot, std::size(kHot), host.avx && host.fma);
}

FX_TEST(fp_hot_form_instruction_counts_sse) {
    FeatureScope scope(sse_only());
    report(kHot, std::size(kHot), false);
}

// The FRECPE/FRSQRTE emitters compute the A64 8-bit estimate as
// round(512 / m) and round(512 / sqrt(m)) in single precision. Check every
// possible input against the A64 integer algorithms in all rounding modes.
FX_TEST(fp_estimate_formulas_match_arm_tables) {
    const auto recip = [](unsigned scaled) { const unsigned a = scaled * 2 + 1; return ((1u << 19) / a + 1) / 2; };
    const auto rsqrt = [](unsigned s) {
        unsigned a = s < 256 ? s * 2 + 1 : (((s >> 1) << 1) + 1) * 2;
        unsigned b = 512;
        while (static_cast<unsigned long long>(a) * (b + 1) * (b + 1) < (1ULL << 28)) ++b;
        return (b + 1) / 2;
    };
    const unsigned saved = _mm_getcsr();
    unsigned bad = 0;
    for (unsigned rc = 0; rc < 4; ++rc) {
        _mm_setcsr(0x1f80u | (rc << 13));
        for (unsigned k = 0; k < 256; ++k) {
            const __m128 m = _mm_set_ss(1.0f + static_cast<float>(k) / 256.0f + 1.0f / 512.0f);
            __m128 q = _mm_div_ss(_mm_set_ss(512.0f), m);
            q = _mm_round_ss(q, q, 8);
            if (static_cast<unsigned>(_mm_cvtss_f32(q)) != recip(256 + k)) ++bad;
            const __m128d md = _mm_set_sd(1.0 + k / 256.0 + 1.0 / 512.0);
            __m128d qd = _mm_div_sd(_mm_set_sd(512.0), md);
            qd = _mm_round_sd(qd, qd, 8);
            if (static_cast<unsigned>(_mm_cvtsd_f64(qd)) != recip(256 + k)) ++bad;
        }
        for (unsigned s = 128; s < 512; ++s) {
            const unsigned a = s < 256 ? s * 2 + 1 : (((s >> 1) << 1) + 1) * 2;
            __m128 v = _mm_sqrt_ss(_mm_set_ss(static_cast<float>(a) / 256.0f));
            __m128 q = _mm_div_ss(_mm_set_ss(512.0f), v);
            q = _mm_round_ss(q, q, 8);
            if (static_cast<unsigned>(_mm_cvtss_f32(q)) != rsqrt(s)) ++bad;
            __m128d vd = _mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(a / 256.0));
            __m128d qd = _mm_div_sd(_mm_set_sd(512.0), vd);
            qd = _mm_round_sd(qd, qd, 8);
            if (static_cast<unsigned>(_mm_cvtsd_f64(qd)) != rsqrt(s)) ++bad;
        }
    }
    _mm_setcsr(saved);
    FX_CHECK_EQ(bad, 0u);
}
