// fp family tests: lowerings from the SIMD&FP cost audit (owner: fp).
//
// Pooled FMOV immediates, pooled masks for FNMUL/FABD/FACGE/FACGT, UCVTF on
// 32-bit lanes through one FMA, SCVTF/UCVTF S from a SIMD register without a
// GPR round trip, and 64-bit vector conversions of sources known zero above
// bit 64. Each runs against the reference interpreter with the new lowering
// and with the old one (TRANSMUTE_FX_FP=oldimm,oldmask,oldcvt through
// fp_set_knobs_for_testing), on edge values (NaNs, +-0, +-Inf, denormals,
// integer rounding and range boundaries) in every FPCR mode (RMode x FZ),
// through the memory, aliasing, memory-source and known-zero-upper contexts
// (check_batch); the integer -> float conversions also compare FPSR.
#include "fx/translator/emit_fp_tests.hpp"
#include "fx/translator/emit_fp_internal.hpp"

#include <cstdio>
#include <tuple>
#include <utility>
#include <vector>

using namespace transmute::fx;
using namespace transmute::fx::fptest;

namespace {

constexpr int kOldBits = fpx::fp_old_imm | fpx::fp_old_mask | fpx::fp_old_cvt;

// Runs `body` with the new lowerings, then with the old ones.
template <class F> void for_both_lowerings(F&& body) {
    for (const int bits : {0, kOldBits}) {
        fp_set_knobs_for_testing(bits);
        body(bits == 0 ? "new" : "old");
    }
    fp_set_knobs_for_testing(-1);
}

void set_lane(GuestContext& c, unsigned r, unsigned es, unsigned i, std::uint64_t value) {
    const unsigned per = 64 / es;
    auto& half = c.v[r][i / per];
    if (es == 64) { half = value; return; }
    const unsigned shift = (i % per) * es;
    const std::uint64_t mask = ((1ULL << es) - 1) << shift;
    half = (half & ~mask) | ((value << shift) & mask);
}

// Every ordered pair of specials in lanes of V1/V2, every FPCR mode; some
// contexts have V1/V2/V3 zero above bit 64 (64-bit vector forms).
std::vector<GuestContext> special_pairs(unsigned es, bool snan) {
    std::vector<std::uint64_t> list;
    for (auto v : specials(es))
        if (snan || !is_snan(v, es)) list.push_back(v);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
    for (auto a : list)
        for (auto b : list) pairs.emplace_back(a, b);
    const unsigned lanes = 128 / es;
    std::vector<GuestContext> out;
    Rng rng(es * 7 + 3);
    for (const auto fpcr : fpcr_modes()) {
        for (std::size_t i = 0; i < pairs.size(); i += lanes) {
            GuestContext c = testing::random_context(rng.next());
            fill_vectors(c, rng, es, snan);
            c.fpcr = fpcr;
            for (unsigned l = 0; l < lanes; ++l) {
                const auto& p = pairs[(i + l) % pairs.size()];
                set_lane(c, 1, es, l, p.first);
                set_lane(c, 2, es, l, p.second);
            }
            if (rng.below(3) == 0)
                for (unsigned r : {1u, 2u, 3u}) c.v[r][1] = 0;
            out.push_back(c);
        }
    }
    return out;
}

// Integer lanes at rounding and range boundaries of u32/s32 -> single.
std::vector<GuestContext> integer_contexts() {
    static const std::uint32_t ints[] = {
        0u, 1u, 2u, 3u, 0x7fu, 0x80u, 0xffu, 0x7fffu, 0x8000u, 0xffffu, 0x10000u, 0x10001u, 0x00ffffffu,
        0x01000000u, 0x01000001u, 0x01000002u, 0x01000003u, 0x01ffffffu, 0x02000001u, 0x7fffff80u, 0x7fffffbfu,
        0x7fffffc0u, 0x7fffffc1u, 0x7fffffffu, 0x80000000u, 0x80000001u, 0x80000080u, 0x80000081u, 0xffffff7fu,
        0xffffff80u, 0xffffff81u, 0xffffffbfu, 0xffffffc0u, 0xfffffffeu, 0xffffffffu, 0xfffe8001u, 0x0001ffffu};
    std::vector<GuestContext> out;
    Rng rng(11);
    const unsigned n = static_cast<unsigned>(std::size(ints));
    for (const auto fpcr : fpcr_modes()) {
        for (unsigned i = 0; i < n; ++i) {
            GuestContext c = testing::random_context(rng.next());
            c.fpcr = fpcr;
            for (unsigned r = 0; r < 4; ++r)
                for (unsigned l = 0; l < 4; ++l) set_lane(c, r, 32, l, ints[(i + 5 * r + 3 * l) % n]);
            set_lane(c, 1, 32, 0, ints[i]);
            if (i % 3 == 0) c.v[1][1] = 0;                                     // zero above 64
            if (i % 5 == 0) c.v[1][1] = 0, c.v[1][0] &= 0xffffffffULL;         // zero above 32
            out.push_back(c);
        }
    }
    return out;
}

CheckOptions with_fpsr(CheckOptions o) {
    o.compare_fpsr = true;
    o.fpsr_mask = 0x1f;
    return o;
}

} // namespace

FX_TEST(fp_lowering_fmov_immediates_all_values) {
    std::vector<GuestContext> ctxs;
    Rng rng(21);
    for (unsigned i = 0; i < 3; ++i) {
        GuestContext c = testing::random_context(rng.next());
        fill_vectors(c, rng, 32);
        c.fpcr = fpcr_modes()[rng.below(8)];
        ctxs.push_back(c);
    }
    for_both_lowerings([&](const char* which) {
        unsigned n = 0;
        for (unsigned imm = 0; imm < 256; ++imm) {
            for (unsigned ft : {0u, 1u}) n += check_batch(enc::fmov_imm(ft, 5, imm), ctxs, exact_opt(), "fmov scalar imm");
            for (unsigned q : {0u, 1u}) n += check_batch(enc::vfmov(q, 0, 0, imm, 6), ctxs, exact_opt(), "fmov 2s/4s imm");
            n += check_batch(enc::vfmov(1, 1, 0, imm, 7), ctxs, exact_opt(), "fmov 2d imm");
        }
        std::printf("    %s: %u contexts\n", which, n);
        FX_CHECK_EQ(n, 256u * 5u * static_cast<unsigned>(ctxs.size()));
    });
}

FX_TEST(fp_lowering_integer_to_float_edges) {
    const auto ctxs = integer_contexts();
    const CheckOptions opt = with_fpsr(fp_opt(32));
    for_both_lowerings([&](const char* which) {
        unsigned n = 0;
        for (unsigned u : {0u, 1u}) {
            for (unsigned q : {0u, 1u})
                for (const auto [rd, rn] : {std::pair{3u, 1u}, std::pair{1u, 1u}})
                    n += check_batch(enc::v2(q, u, 0, 0x1d, rd, rn), ctxs, opt, u ? "ucvtf 2s/4s" : "scvtf 2s/4s");
            for (const auto [rd, rn] : {std::pair{3u, 1u}, std::pair{1u, 1u}})
                n += check_batch(enc::s2(u, 0, 0x1d, rd, rn), ctxs, opt, u ? "ucvtf s,s" : "scvtf s,s");
        }
        std::printf("    %s: %u contexts\n", which, n);
        FX_CHECK_EQ(n, 12u * static_cast<unsigned>(ctxs.size()));
    });
}

FX_TEST(fp_lowering_float_to_int_2s_specials) {
    // FCVT[NMPZA][SU] .2S, sources sometimes known zero above bit 64.
    struct Op { unsigned u, a, opc; };
    static const Op ops[] = {{0, 0, 0x1a}, {0, 0, 0x1b}, {0, 1, 0x1a}, {0, 1, 0x1b}, {0, 0, 0x1c},
                             {1, 0, 0x1a}, {1, 0, 0x1b}, {1, 1, 0x1a}, {1, 1, 0x1b}, {1, 0, 0x1c}};
    const auto ctxs = special_pairs(32, true);
    for_both_lowerings([&](const char* which) {
        unsigned n = 0;
        for (const auto& o : ops)
            for (const auto [rd, rn] : {std::pair{3u, 1u}, std::pair{1u, 1u}})
                n += check_batch(enc::v2(0, o.u, o.a << 1, o.opc, rd, rn), ctxs, exact_opt(), "fcvt* 2s");
        std::printf("    %s: %u contexts\n", which, n);
        FX_CHECK_EQ(n, 20u * static_cast<unsigned>(ctxs.size()));
    });
}

FX_TEST(fp_lowering_masks_special_pairs) {
    for_both_lowerings([&](const char* which) {
        for (unsigned es : {32u, 64u}) {
            const unsigned ft = es == 64 ? 1 : 0, sz = es == 64 ? 1 : 0;
            const auto ctxs = special_pairs(es, true);
            unsigned n = 0;
            for (const auto [rd, rn, rm] : {std::tuple{3u, 1u, 2u}, std::tuple{1u, 1u, 2u}, std::tuple{2u, 1u, 2u}}) {
                n += check_batch(enc::fp2(8, ft, rd, rn, rm), ctxs, fp_opt(es), "fnmul");
                n += check_batch(enc::s3(1, 2 | sz, 0x1a, rd, rn, rm), ctxs, fp_opt(es), "fabd scalar");
                for (unsigned q : {0u, 1u}) {
                    if (es == 64 && !q) continue;
                    n += check_batch(enc::v3(q, 1, 2 | sz, 0x1a, rd, rn, rm), ctxs, fp_opt(es), "fabd vector");
                    for (unsigned a : {0u, 1u})
                        n += check_batch(enc::v3(q, 1, (a << 1) | sz, 0x1d, rd, rn, rm), ctxs, with_fpsr(exact_opt()),
                                         a ? "facgt vector" : "facge vector");
                }
                for (unsigned a : {0u, 1u})
                    n += check_batch(enc::s3(1, (a << 1) | sz, 0x1d, rd, rn, rm), ctxs, with_fpsr(exact_opt()),
                                     a ? "facgt scalar" : "facge scalar");
            }
            std::printf("    %s %s: %u contexts\n", which, es == 32 ? "S" : "D", n);
            FX_CHECK(n > 0);
        }
    });
}
