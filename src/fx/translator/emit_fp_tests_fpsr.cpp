// fp family tests: FPCR/FPSR system registers and cumulative exception
// flags (owner: fp).
#include "fx/translator/emit_fp_tests.hpp"

#include <cstdio>
#include <utility>
#include <vector>

using namespace transmute::fx;
using namespace transmute::fx::fptest;

namespace {

struct FlagCase { const char* name; std::uint32_t word; unsigned es; };

// (mismatching contexts, checked contexts); mismatches are not reported.
std::pair<unsigned, unsigned> fpsr_mismatches(std::uint32_t word, unsigned es, unsigned contexts) {
    unsigned bad = 0, checked = 0;
    Rng rng(word);
    for (unsigned i = 0; i < contexts; ++i) {
        GuestContext ctx = testing::random_context(rng.next());
        fill_vectors(ctx, rng, es, true);
        ctx.fpcr = fpcr_modes()[rng.below(8)];
        ctx.fpsr = 0;
        GuestContext expected = ctx;
        const std::uint32_t mrs = 0xd53b4420u | 28u;
        if (!testing::oracle_step(expected, word) || !testing::oracle_step(expected, mrs)) return {0, 0};
        GuestContext actual = ctx;
        const std::uint32_t words[2] = {word, mrs};
        testing::run_family(actual, fp_emit, words, fp_mxcsr_for_fpcr(ctx.fpcr));
        ++checked;
        if ((expected.x[28] & 0x1f) != (actual.x[28] & 0x1f)) ++bad;
    }
    return {bad, checked};
}

} // namespace

FX_TEST(fp_mrs_msr_fpcr_fpsr_match_oracle) {
    for (std::uint64_t seed = 0; seed < 64; ++seed) {
        GuestContext ctx = testing::random_context(seed);
        Rng r(seed);
        const std::uint32_t fpcr_value = static_cast<std::uint32_t>(r.next()) & 0x07c00000u;   // AHP DN FZ RMode
        const std::uint32_t fpsr_value = static_cast<std::uint32_t>(r.next()) & 0x0800009fu;
        ctx.x[3] = fpcr_value | (r.next() & 0xffffffff00000000ULL);
        ctx.x[4] = fpsr_value;
        ctx.fpcr = 0;
        ctx.fpsr = 0;
        // msr fpcr, x3; mrs x5, fpcr; msr fpsr, x4; mrs x6, fpsr
        const std::uint32_t words[] = {0xd51b4403u, 0xd53b4405u, 0xd51b4424u, 0xd53b4426u};
        GuestContext expected = ctx;
        for (auto w : words) FX_REQUIRE(testing::oracle_step(expected, w));
        GuestContext actual = ctx;
        FX_REQUIRE(testing::run_family(actual, fp_emit, words, 0x1f80));
        FX_CHECK_EQ(actual.x[5], expected.x[5]);
        FX_CHECK_EQ(actual.x[6], expected.x[6]);
        FX_CHECK_EQ(actual.fpcr, expected.fpcr);
        FX_CHECK_EQ(actual.fpsr, fpsr_value);
        GuestContext alias = ctx;
        FX_REQUIRE(run_aliasing(alias, words, 0x1f80));
        FX_CHECK_EQ(alias.x[5], expected.x[5]);
        FX_CHECK_EQ(alias.x[6], expected.x[6]);
    }
}

FX_TEST(fp_msr_fpcr_rewrites_mxcsr) {
    // MSR FPCR, then arithmetic in the new mode, against the oracle.
    for (unsigned rmode = 0; rmode < 4; ++rmode) {
        for (unsigned fz = 0; fz < 2; ++fz) {
            GuestContext ctx = testing::random_context(rmode * 2 + fz);
            ctx.fpcr = 0;
            ctx.x[3] = (rmode << 22) | (fz << 24);
            ctx.v[1][0] = f2b(1.0f);
            ctx.v[2][0] = f2b(1.0e-8f);     // inexact sum: rounding visible
            ctx.v[4][0] = 0x00400000;       // denormal: FZ visible
            ctx.v[5][0] = f2b(1.0f);
            const std::uint32_t words[] = {0xd51b4403u, enc::fp2(2, 0, 0, 1, 2), enc::fp2(0, 0, 6, 4, 5),
                                           enc::fp2(3, 0, 7, 1, 2)};
            GuestContext expected = ctx;
            for (auto w : words) FX_REQUIRE(testing::oracle_step(expected, w));
            GuestContext actual = ctx;
            FX_REQUIRE(testing::run_family(actual, fp_emit, words, 0x1f80));
            FX_CHECK_EQ(actual.v[0][0], expected.v[0][0]);
            FX_CHECK_EQ(actual.v[6][0], expected.v[6][0]);
            FX_CHECK_EQ(actual.v[7][0], expected.v[7][0]);
        }
    }
}

FX_TEST(fp_mrs_fpsr_reports_accumulated_flags) {
    struct Case { const char* name; std::uint32_t word; std::uint64_t a, b; std::uint32_t flags; };
    const Case cases[] = {
        {"fdiv by zero -> DZC", enc::fp2(1, 0, 0, 1, 2), f2b(1.0f), f2b(0.0f), 0x02},
        {"fadd inexact -> IXC", enc::fp2(2, 0, 0, 1, 2), f2b(1.0f), f2b(1.0e-8f), 0x10},
        {"fmul overflow -> OFC|IXC", enc::fp2(0, 0, 0, 1, 2), f2b(3.0e38f), f2b(10.0f), 0x14},
        {"fmul underflow -> UFC|IXC", enc::fp2(0, 0, 0, 1, 2), f2b(1.0e-30f), f2b(1.0e-20f), 0x18},
        {"fsub inf-inf -> IOC", enc::fp2(3, 0, 0, 1, 2), 0x7f800000, 0x7f800000, 0x01},
        {"fadd exact -> none", enc::fp2(2, 0, 0, 1, 2), f2b(1.0f), f2b(2.0f), 0x00},
        {"fcmpe qnan -> IOC", enc::fcmp(0, 1, 2, 16), 0x7fc00000, f2b(1.0f), 0x01},
        {"fcmp qnan -> none", enc::fcmp(0, 1, 2, 0), 0x7fc00000, f2b(1.0f), 0x00},
        {"fcvtzs overflow -> IOC", enc::fpint(0, 0, 3, 0, 0, 1), f2b(3.0e9f), 0, 0x01},
        {"fcvtzu negative -> IOC", enc::fpint(0, 0, 3, 1, 0, 1), f2b(-5.0f), 0, 0x01},
        {"fcvtzs inexact -> IXC", enc::fpint(0, 0, 3, 0, 0, 1), f2b(1.5f), 0, 0x10},
        {"frint32x inexact -> IXC", enc::fp1(17, 0, 0, 1), f2b(1.5f), 0, 0x10},
        {"frint32z inexact -> IXC", enc::fp1(16, 0, 0, 1), f2b(-1.25f), 0, 0x10},
        {"frint32z exact -> none", enc::fp1(16, 0, 0, 1), f2b(-7.0f), 0, 0x00},
        {"frint32x overflow -> IOC", enc::fp1(17, 0, 0, 1), f2b(3.0e9f), 0, 0x01},
        {"frint64z nan -> IOC", enc::fp1(18, 0, 0, 1), 0x7fc00000, 0, 0x01},
        {"frint64x inf -> IOC", enc::fp1(19, 1, 0, 1), 0x7ff0000000000000ULL, 0, 0x01},
        {"frint32z 2^31 -> IOC", enc::v2(0, 0, 0, 0x1e, 0, 1), f2b(2147483648.0f), 0, 0x01},
    };
    for (const auto& c : cases) {
        GuestContext ctx = testing::random_context(3);
        ctx.fpcr = 0;
        ctx.fpsr = 0x08000000;   // QC: preserved
        ctx.v[1][0] = c.a;
        ctx.v[2][0] = c.b;
        ctx.x[9] = 0;
        // insn; mrs x3, fpsr; msr fpsr, x9; mrs x4, fpsr
        const std::uint32_t words[] = {c.word, 0xd53b4423u, 0xd51b4429u, 0xd53b4424u};
        GuestContext expected = ctx;
        for (auto w : words) FX_REQUIRE(testing::oracle_step(expected, w));
        GuestContext actual = ctx;
        FX_REQUIRE(testing::run_family(actual, fp_emit, words, 0x1f80));
        if (actual.x[3] != (0x08000000u | c.flags)) {
            char label[128];
            std::snprintf(label, sizeof(label), "%s: fpsr 0x%llx (oracle 0x%llx)", c.name,
                          static_cast<unsigned long long>(actual.x[3]), static_cast<unsigned long long>(expected.x[3]));
            testing::report_failure(__FILE__, __LINE__, label);
        }
        FX_CHECK_EQ(actual.x[3] & 0x9f, expected.x[3] & 0x9f);
        FX_CHECK_EQ(actual.x[4], 0u);
        FX_CHECK_EQ(expected.x[4], 0u);
    }
}

FX_TEST(fp_zz_fpsr_divergence_report) {
    // Diagnostic: share of random contexts whose cumulative FPSR flags differ
    // from the reference interpreter (flags are best effort; NOTES_FP.md).
    const FlagCase cases[] = {
        {"fadd s", enc::fp2(2, 0, 0, 1, 2), 32},          {"fmul d", enc::fp2(0, 1, 0, 1, 2), 64},
        {"fdiv s", enc::fp2(1, 0, 0, 1, 2), 32},          {"fsqrt d", enc::fp1(3, 1, 0, 1), 64},
        {"fmadd s", enc::fp3(0, 0, 0, 0, 1, 2, 3), 32},   {"fmax s", enc::fp2(4, 0, 0, 1, 2), 32},
        {"fmaxnm d", enc::fp2(6, 1, 0, 1, 2), 64},        {"fcmp s", enc::fcmp(0, 1, 2, 0), 32},
        {"fcmpe d", enc::fcmp(1, 1, 2, 16), 64},          {"fcvt s<-d", enc::fp1(4, 1, 0, 1), 64},
        {"fcvtzs w,s", enc::fpint(0, 0, 3, 0, 0, 1), 32}, {"fcvtzu x,d", enc::fpint(1, 1, 3, 1, 0, 1), 64},
        {"fcvtas w,s", enc::fpint(0, 0, 0, 4, 0, 1), 32}, {"scvtf s,x", enc::fpint(1, 0, 0, 2, 0, 1), 32},
        {"frinta s", enc::fp1(12, 0, 0, 1), 32},          {"frintx d", enc::fp1(14, 1, 0, 1), 64},
        {"fadd 4s", enc::v3(1, 0, 0, 0x1a, 0, 1, 2), 32}, {"fadd 2s", enc::v3(0, 0, 0, 0x1a, 0, 1, 2), 32},
        {"fcvtzs 4s", enc::v2(1, 0, 2, 0x1b, 0, 1), 32},  {"fcvtzu 4s", enc::v2(1, 1, 2, 0x1b, 0, 1), 32},
        {"fcmge 4s", enc::v3(1, 1, 0, 0x1c, 0, 1, 2), 32}, {"frecps 4s", enc::v3(1, 0, 0, 0x1f, 0, 1, 2), 32},
        {"frint32x 4s", enc::v2(1, 1, 0, 0x1e, 0, 1), 32}, {"frint64z d", enc::fp1(18, 1, 0, 1), 64},
        {"fmlal 4s", enc::v3(1, 0, 0, 0x1d, 0, 1, 2), 16},
    };
    for (const auto& c : cases) {
        const auto [bad, checked] = fpsr_mismatches(c.word, c.es, 400);
        std::printf("    %-12s FPSR differs in %3u of %3u contexts\n", c.name, bad, checked);
    }
}
