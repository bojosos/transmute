// fp family tests: FPCR/FPSR system registers and cumulative exception
// flags (owner: fp).
#include "fx/translator/emit_fp_tests.hpp"
#include "fx/translator/emit_fp_internal.hpp"

#include <cmath>
#include <cstdio>
#include <string>
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

// ---- tininess before rounding -------------------------------------------------------------------
// A64 detects tininess before rounding, x64 after: a result that rounds up in
// magnitude to +-min_normal sets UFC|IXC on A64 and IXC alone on x64 (with
// FZ = 0; NOTES_FP.md). The contexts below put every lane's exact result within
// a few ulps of +-min_normal, on both sides, in every rounding mode with FZ = 0.

namespace {

enum class TinyOp { mul, div, fma, narrow };

// elem >= 0: by-element form, Vm lane `elem` is the second factor of every lane.
struct TinyWord { const char* name; std::uint32_t word; unsigned es; TinyOp op; unsigned lanes; int elem = -1; };

const TinyWord kTinyWords[] = {
    {"fmul s", enc::fp2(0, 0, 0, 1, 2), 32, TinyOp::mul, 1},
    {"fmul d", enc::fp2(0, 1, 0, 1, 2), 64, TinyOp::mul, 1},
    {"fmul s1=s1*s2", enc::fp2(0, 0, 1, 1, 2), 32, TinyOp::mul, 1},
    {"fnmul d", enc::fp2(8, 1, 0, 1, 2), 64, TinyOp::mul, 1},
    {"fdiv s", enc::fp2(1, 0, 0, 1, 2), 32, TinyOp::div, 1},
    {"fdiv d s2=s1/s2", enc::fp2(1, 1, 2, 1, 2), 64, TinyOp::div, 1},
    {"fmadd s", enc::fp3(0, 0, 0, 0, 1, 2, 3), 32, TinyOp::fma, 1},
    {"fmsub d", enc::fp3(0, 1, 1, 0, 1, 2, 3), 64, TinyOp::fma, 1},
    {"fnmadd s", enc::fp3(1, 0, 0, 0, 1, 2, 3), 32, TinyOp::fma, 1},
    {"fnmsub d s3", enc::fp3(1, 1, 1, 3, 1, 2, 3), 64, TinyOp::fma, 1},
    {"fmul 4s", enc::v3(1, 1, 0, 0x1b, 0, 1, 2), 32, TinyOp::mul, 4},
    {"fmul 2s", enc::v3(0, 1, 0, 0x1b, 0, 1, 2), 32, TinyOp::mul, 2},
    {"fmul 2d", enc::v3(1, 1, 1, 0x1b, 0, 1, 2), 64, TinyOp::mul, 2},
    {"fdiv 4s", enc::v3(1, 1, 0, 0x1f, 0, 1, 2), 32, TinyOp::div, 4},
    {"fmla 4s", enc::v3(1, 0, 0, 0x19, 0, 1, 2), 32, TinyOp::fma, 4},
    {"fmls 2d", enc::v3(1, 0, 3, 0x19, 0, 1, 2), 64, TinyOp::fma, 2},
    {"fmul s,v.s[1]", enc::selem(0, 2, 1, 0, 2, 9, 0, 0, 1), 32, TinyOp::mul, 1, 1},
    {"fmla 4s,v.s[3]", enc::velem(1, 0, 2, 1, 0, 2, 1, 1, 0, 1), 32, TinyOp::fma, 4, 3},
    {"fcvt s<-d", enc::fp1(4, 1, 0, 1), 64, TinyOp::narrow, 1},
    {"fcvtn 2s", enc::v2(0, 0, 1, 0x16, 0, 1), 64, TinyOp::narrow, 2},
    {"fcvtn2 4s", enc::v2(1, 0, 1, 0x16, 0, 1), 64, TinyOp::narrow, 2},
};

void put_lane(GuestContext& c, unsigned r, unsigned es, unsigned i, std::uint64_t value) {
    if (es == 64) { c.v[r][i] = value; return; }
    const unsigned shift = (i & 1) * 32;
    c.v[r][i / 2] = (c.v[r][i / 2] & ~(0xffffffffull << shift)) | (value << shift);
}

std::uint64_t bits_of(double v, unsigned es) { return es == 64 ? d2b(v) : f2b(static_cast<float>(v)); }

// Random significand in [1, 2) times 2^e, with a random sign.
double rand_scaled(Rng& rng, int lo, int hi) {
    const double m = 1.0 + static_cast<double>(rng.next() >> 11) * 0x1p-53;
    const int e = lo + static_cast<int>(rng.below(static_cast<unsigned>(hi - lo + 1)));
    return (rng.below(2) ? -1.0 : 1.0) * std::ldexp(m, e);
}

// x moved by k ulps of precision es (k may be negative).
double nudge(double x, int k, unsigned es) {
    for (; k > 0; --k) x = es == 64 ? std::nextafter(x, HUGE_VAL) : std::nextafterf(static_cast<float>(x), HUGE_VALF);
    for (; k < 0; ++k) x = es == 64 ? std::nextafter(x, -HUGE_VAL) : std::nextafterf(static_cast<float>(x), -HUGE_VALF);
    return x;
}

// One lane of operands (V1, V2, V3 / V0) whose exact result is near +-min_normal.
void tiny_lane(GuestContext& c, Rng& rng, const TinyWord& w, unsigned lane, double elem) {
    const unsigned es = w.es;
    const double M = es == 64 ? 0x1p-1022 : 0x1p-126;
    const auto as_es = [&](double v) { return es == 64 ? v : static_cast<double>(static_cast<float>(v)); };
    const int k = static_cast<int>(rng.below(7)) - 3;
    switch (w.op) {
    case TinyOp::mul: {
        if (w.elem >= 0) {   // x near M / elem in every lane of Vn
            const double x = nudge(as_es(M / std::fabs(elem)), k, es);
            put_lane(c, 1, es, lane, bits_of(rng.below(2) ? x : -x, es));
            break;
        }
        const double x = as_es(rand_scaled(rng, -20, 20));
        const double y = nudge(as_es(M / x), k, es);
        put_lane(c, 1, es, lane, bits_of(x, es));
        put_lane(c, 2, es, lane, bits_of(rng.below(2) ? y : -y, es));
        break;
    }
    case TinyOp::div: {
        const double y = as_es(rand_scaled(rng, -20, 20));
        const double x = nudge(as_es(M * std::fabs(y)), k, es);
        put_lane(c, 1, es, lane, bits_of(rng.below(2) ? x : -x, es));
        put_lane(c, 2, es, lane, bits_of(y, es));
        break;
    }
    case TinyOp::fma: {
        // n * m near M, plus an addend a few ulps of M or smaller (either sign).
        const double x = w.elem >= 0 ? nudge(as_es(M / std::fabs(elem)), k, es) : as_es(rand_scaled(rng, -20, 20));
        const double y = w.elem >= 0 ? elem : nudge(as_es(M / x), k, es);
        const double acc = as_es(std::ldexp(rand_scaled(rng, 0, 0), (es == 64 ? -1074 : -149) + static_cast<int>(rng.below(24))));
        put_lane(c, 1, es, lane, bits_of(rng.below(2) ? x : -x, es));
        if (w.elem < 0) put_lane(c, 2, es, lane, bits_of(y, es));
        put_lane(c, 3, es, lane, bits_of(acc, es));
        put_lane(c, 0, es, lane, bits_of(acc, es));
        break;
    }
    case TinyOp::narrow: {
        // Doubles within 2^-20 (relative) of single min_normal.
        const double v = 0x1p-126 * (1.0 + (static_cast<double>(rng.next() >> 11) * 0x1p-53 * 2.0 - 1.0) * 0x1p-20);
        put_lane(c, 1, 64, lane, d2b(rng.below(2) ? v : -v));
        break;
    }
    }
}

std::vector<GuestContext> tiny_contexts(const TinyWord& w, unsigned per_mode, bool fz) {
    std::vector<GuestContext> out;
    Rng rng(w.word);
    for (unsigned rmode = 0; rmode < 4; ++rmode) {
        for (unsigned i = 0; i < per_mode; ++i) {
            GuestContext c = testing::random_context(rng.next());
            fill_vectors(c, rng, w.es, false);
            c.fpcr = rmode << 22 | (fz ? 1u << 24 : 0u);
            c.fpsr = 0;
            double elem = 0;
            if (w.elem >= 0) {
                elem = rand_scaled(rng, -20, 20);
                if (w.es == 32) elem = static_cast<float>(elem);
                put_lane(c, 2, w.es, static_cast<unsigned>(w.elem), bits_of(elem, w.es));
            }
            for (unsigned l = 0; l < w.lanes; ++l) tiny_lane(c, rng, w, l, elem);
            // 64-bit vectors: x64 computes the upper lanes too, and their flags
            // are a separate divergence (NOTES_FP.md, "2S/4H garbage upper lanes").
            if (w.lanes > 1 && w.lanes * w.es == 64 && w.op != TinyOp::narrow)
                for (unsigned r = 0; r < 4; ++r) c.v[r][1] = 0;
            out.push_back(c);
        }
    }
    return out;
}

// Contexts whose FPSR (IOC..IXC) after the word and an MRS differs from the
// reference interpreter, and contexts where the result register differs.
struct TinyTally { unsigned fpsr = 0, result = 0, checked = 0; };
TinyTally tiny_mismatches(const TinyWord& w, const std::vector<GuestContext>& contexts) {
    TinyTally t;
    const std::uint32_t mrs = 0xd53b4420u | 28u;
    const unsigned rd = w.word & 31;
    for (const auto& ctx : contexts) {
        GuestContext expected = ctx;
        if (!testing::oracle_step(expected, w.word) || !testing::oracle_step(expected, mrs)) continue;
        GuestContext actual = ctx;
        const std::uint32_t words[2] = {w.word, mrs};
        testing::run_family(actual, fp_emit, words, fp_mxcsr_for_fpcr(ctx.fpcr));
        ++t.checked;
        if ((expected.x[28] & 0x1f) != (actual.x[28] & 0x1f)) ++t.fpsr;
        if (expected.v[rd][0] != actual.v[rd][0] || expected.v[rd][1] != actual.v[rd][1]) ++t.result;
    }
    return t;
}

} // namespace

FX_TEST(fp_zz_tininess_before_rounding_report) {
    // Diagnostic: with the default lowering x64's after-rounding tininess
    // misses UFC where a result rounds up to +-min_normal (FZ = 0). Results
    // are exact either way.
    for (const auto& w : kTinyWords) {
        const auto t = tiny_mismatches(w, tiny_contexts(w, 64, false));
        std::printf("    %-16s FPSR differs in %3u of %3u contexts (FZ = 0)\n", w.name, t.fpsr, t.checked);
        FX_CHECK_EQ(t.result, 0u);
        FX_CHECK(t.checked > 0);
    }
}

FX_TEST(fp_tiny_before_rounding_exact_flags) {
    // TRANSMUTE_FX_FP=tinybefore: FPSR and results equal the reference
    // interpreter on the boundary contexts (FZ = 0), through the memory,
    // aliasing, memory-source and known-zero-upper contexts.
    fp_set_knobs_for_testing(static_cast<int>(fpx::fp_tiny_before));
    for (const auto& w : kTinyWords) {
        const auto t = tiny_mismatches(w, tiny_contexts(w, 64, false));
        if (t.fpsr || t.result) {
            char label[160];
            std::snprintf(label, sizeof(label), "%s: FPSR differs in %u, result in %u of %u", w.name, t.fpsr, t.result,
                          t.checked);
            testing::report_failure(__FILE__, __LINE__, label);
        }
        CheckOptions opt = fp_opt(w.es);   // NaN payloads (0/0 in zeroed lanes) are host policy
        opt.compare_fpsr = true;
        const auto contexts = tiny_contexts(w, 16, false);
        FX_CHECK_EQ(check_batch(w.word, contexts, opt, std::string(w.name) + " tinybefore"), contexts.size());
    }
    fp_set_knobs_for_testing(-1);
}

FX_TEST(fp_tiny_before_rounding_results_bit_exact) {
    // The knob's lowering keeps every result: random and special operands
    // (NaN payloads as host policy, fp_opt) in every FPCR mode, FZ included,
    // and the boundary contexts with FZ = 1 (products, quotients, fused).
    fp_set_knobs_for_testing(static_cast<int>(fpx::fp_tiny_before));
    for (const auto& w : kTinyWords) {
        std::vector<GuestContext> contexts;
        Rng rng(w.word ^ 0x5eed);
        for (const auto fpcr : fpcr_modes()) {
            for (unsigned i = 0; i < 24; ++i) {
                GuestContext c = testing::random_context(rng.next());
                fill_vectors(c, rng, w.es, true);
                c.fpcr = fpcr;
                contexts.push_back(c);
            }
        }
        // FZ = 1 boundary results of the narrowing forms are the known FZ = 1
        // divergence (A64 flushes before rounding; the comparator's fz_tininess
        // tolerance does not cover FCVTN lanes), the same without the knob.
        if (w.op != TinyOp::narrow)
            for (const auto& c : tiny_contexts(w, 16, true)) contexts.push_back(c);
        FX_CHECK_EQ(check_batch(w.word, contexts, fp_opt(w.es), std::string(w.name) + " tinybefore results"),
                    contexts.size());
    }
    fp_set_knobs_for_testing(-1);
}
