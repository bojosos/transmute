// fp family tests: exhaustive special values (pairs for binary operations)
// in every FPCR rounding mode with FZ off and on (owner: fp).
#include "fx/translator/emit_fp_tests.hpp"

#include <cstdio>
#include <string>
#include <tuple>
#include <vector>

using namespace transmute::fx;
using namespace transmute::fx::fptest;

namespace {

std::vector<std::uint64_t> special_list(unsigned es, bool snan) {
    std::vector<std::uint64_t> out;
    for (auto v : specials(es))
        if (snan || !is_snan(v, es)) out.push_back(v);
    return out;
}

void set_lane(GuestContext& c, unsigned r, unsigned es, unsigned i, std::uint64_t value) {
    const unsigned per = 64 / es;
    auto& half = c.v[r][i / per];
    if (es == 64) { half = value; return; }
    const unsigned shift = (i % per) * es;
    const std::uint64_t mask = ((1ULL << es) - 1) << shift;
    half = (half & ~mask) | ((value << shift) & mask);
}

// All ordered pairs of specials in Vn/Vm lanes (4S or 2D), every FPCR mode.
std::vector<GuestContext> pair_contexts(unsigned es, unsigned rn, unsigned rm, bool snan, unsigned lanes) {
    const auto list = special_list(es, snan);
    std::vector<std::pair<std::uint64_t, std::uint64_t>> pairs;
    for (auto a : list)
        for (auto b : list) pairs.emplace_back(a, b);
    std::vector<GuestContext> out;
    Rng rng(es * 977 + rn * 31 + rm);
    for (const auto fpcr : fpcr_modes()) {
        for (std::size_t i = 0; i < pairs.size(); i += lanes) {
            GuestContext c = testing::random_context(rng.next());
            fill_vectors(c, rng, es, snan);
            c.fpcr = fpcr;
            for (unsigned l = 0; l < lanes; ++l) {
                const auto& p = pairs[(i + l) % pairs.size()];
                set_lane(c, rn, es, l, p.first);
                set_lane(c, rm, es, l, p.second);
            }
            out.push_back(c);
        }
    }
    return out;
}

std::vector<GuestContext> unary_contexts(unsigned es, unsigned rn, unsigned lanes) {
    const auto list = special_list(es, true);
    std::vector<GuestContext> out;
    Rng rng(es * 131 + rn);
    for (const auto fpcr : fpcr_modes()) {
        for (std::size_t i = 0; i < list.size(); i += lanes) {
            GuestContext c = testing::random_context(rng.next());
            fill_vectors(c, rng, es);
            c.fpcr = fpcr;
            for (unsigned l = 0; l < lanes; ++l) set_lane(c, rn, es, l, list[(i + l) % list.size()]);
            out.push_back(c);
        }
    }
    return out;
}

struct Op { const char* name; unsigned u, a, opc; bool snan; bool fp_result; };

void run_vector_pairs(const Op* ops, std::size_t count) {
    for (unsigned es : {32u, 64u}) {
        const unsigned sz = es == 64 ? 1 : 0;
        for (std::size_t i = 0; i < count; ++i) {
            const auto& o = ops[i];
            const auto ctxs = pair_contexts(es, 1, 2, o.snan, 128 / es);
            const std::uint32_t word = enc::v3(1, o.u, (o.a << 1) | sz, o.opc, 3, 1, 2);
            CheckOptions opt = o.fp_result ? fp_opt(es) : exact_opt();
            const unsigned n = check_batch(word, ctxs, opt, o.name);
            std::printf("    %-10s %s  %6u contexts\n", o.name, es == 32 ? "4S" : "2D", n);
            FX_CHECK(n == ctxs.size());
            // Scalar form on lane 0 (SIMD scalar / FP data processing).
        }
    }
}

} // namespace

FX_TEST(fp_special_pairs_vector_arith) {
    static const Op ops[] = {
        {"fadd", 0, 0, 0x1a, true, true},   {"fsub", 0, 1, 0x1a, true, true},
        {"fmul", 1, 0, 0x1b, true, true},   {"fdiv", 1, 0, 0x1f, true, true},
        {"fabd", 1, 1, 0x1a, true, true},   {"fmulx", 0, 0, 0x1b, true, true},
        {"frecps", 0, 0, 0x1f, true, true},
    };
    run_vector_pairs(ops, std::size(ops));
}

FX_TEST(fp_special_pairs_vector_minmax) {
    static const Op ops[] = {
        {"fmax", 0, 0, 0x1e, true, true},    {"fmin", 0, 1, 0x1e, true, true},
        {"fmaxnm", 0, 0, 0x18, false, true}, {"fminnm", 0, 1, 0x18, false, true},
        {"fmaxp", 1, 0, 0x1e, true, true},   {"fminnmp", 1, 1, 0x18, false, true},
    };
    run_vector_pairs(ops, std::size(ops));
}

FX_TEST(fp_special_pairs_vector_compare) {
    static const Op ops[] = {
        {"fcmeq", 0, 0, 0x1c, true, false}, {"fcmge", 1, 0, 0x1c, true, false},
        {"fcmgt", 1, 1, 0x1c, true, false}, {"facge", 1, 0, 0x1d, true, false},
        {"facgt", 1, 1, 0x1d, true, false},
    };
    run_vector_pairs(ops, std::size(ops));
}

FX_TEST(fp_special_pairs_scalar_compare_and_select) {
    for (unsigned es : {32u, 64u}) {
        const unsigned ft = es == 64 ? 1 : 0;
        const auto ctxs = pair_contexts(es, 1, 2, true, 1);
        unsigned n = 0;
        for (unsigned opc2 : {0u, 16u}) n += check_batch(enc::fcmp(ft, 1, 2, opc2), ctxs, fp_opt(es), "fcmp/fcmpe");
        for (unsigned cond : {0u, 4u, 10u, 12u, 13u})
            n += check_batch(enc::fccmp(ft, 1, 2, cond, 0, 5), ctxs, fp_opt(es), "fccmp");
        // FMAX/FMIN/FMAXNM/FMINNM scalar (oracle zero-extension bug corrected).
        CheckOptions mm = fp_opt(es);
        mm.reference = ref_scalar_minmax;
        for (unsigned op : {4u, 5u}) n += check_batch(enc::fp2(op, ft, 3, 1, 2), ctxs, mm, "fmax/fmin scalar");
        const auto ctxs_q = pair_contexts(es, 1, 2, false, 1);
        for (unsigned op : {6u, 7u}) n += check_batch(enc::fp2(op, ft, 3, 1, 2), ctxs_q, mm, "fmaxnm/fminnm scalar");
        for (unsigned op : {0u, 1u, 2u, 3u, 8u}) n += check_batch(enc::fp2(op, ft, 3, 1, 2), ctxs, fp_opt(es), "arith scalar");
        std::printf("    scalar %s: %u contexts\n", es == 32 ? "S" : "D", n);
    }
}

FX_TEST(fp_special_triples_fused) {
    for (unsigned es : {32u, 64u}) {
        const unsigned ft = es == 64 ? 1 : 0;
        auto ctxs = pair_contexts(es, 1, 2, true, 128 / es);
        Rng rng(5);
        const auto list = special_list(es, true);
        for (auto& c : ctxs)
            for (unsigned l = 0; l < 128 / es; ++l) set_lane(c, 3, es, l, list[rng.below(static_cast<unsigned>(list.size()))]);
        unsigned n = 0;
        n += check_batch(enc::v3(1, 0, es == 64 ? 1 : 0, 0x19, 3, 1, 2), ctxs, fp_opt(es), "fmla");
        n += check_batch(enc::v3(1, 0, es == 64 ? 3 : 2, 0x19, 3, 1, 2), ctxs, fp_opt(es), "fmls");
        for (unsigned o1 = 0; o1 < 2; ++o1)
            for (unsigned o0 = 0; o0 < 2; ++o0)
                n += check_batch(enc::fp3(o1, o0, ft, 4, 1, 2, 3), ctxs, fp_opt(es), "fmadd family");
        std::printf("    fused %s: %u contexts\n", es == 32 ? "S" : "D", n);
    }
}

FX_TEST(fp_special_unary_vector) {
    struct U { const char* name; unsigned u, a, opc; bool fp_result; };
    static const U ops[] = {
        {"fsqrt", 1, 1, 0x1f, true},  {"frintn", 0, 0, 0x18, true}, {"frintm", 0, 0, 0x19, true},
        {"frintp", 0, 1, 0x18, true}, {"frintz", 0, 1, 0x19, true}, {"frinta", 1, 0, 0x18, true},
        {"frintx", 1, 0, 0x19, true}, {"frinti", 1, 1, 0x19, true}, {"fabs", 0, 1, 0x0f, true},
        {"fneg", 1, 1, 0x0f, true},  {"fcvtns", 0, 0, 0x1a, false}, {"fcvtnu", 1, 0, 0x1a, false},
        {"fcvtms", 0, 0, 0x1b, false}, {"fcvtmu", 1, 0, 0x1b, false}, {"fcvtas", 0, 0, 0x1c, false},
        {"fcvtau", 1, 0, 0x1c, false}, {"fcvtps", 0, 1, 0x1a, false}, {"fcvtpu", 1, 1, 0x1a, false},
        {"fcvtzs", 0, 1, 0x1b, false}, {"fcvtzu", 1, 1, 0x1b, false}, {"fcmgt0", 0, 1, 0x0c, false},
        {"fcmeq0", 0, 1, 0x0d, false}, {"fcmlt0", 0, 1, 0x0e, false}, {"fcmge0", 1, 1, 0x0c, false},
        {"fcmle0", 1, 1, 0x0d, false}, {"frecpe", 0, 1, 0x1d, true}, {"frsqrte", 1, 1, 0x1d, true},
    };
    for (unsigned es : {32u, 64u}) {
        const auto ctxs = unary_contexts(es, 1, 128 / es);
        const auto ctxs1 = unary_contexts(es, 1, 1);
        unsigned n = 0;
        for (const auto& o : ops) {
            const unsigned size = (o.a << 1) | (es == 64 ? 1 : 0);
            CheckOptions opt = o.fp_result ? fp_opt(es) : exact_opt();
            n += check_batch(enc::v2(1, o.u, size, o.opc, 2, 1), ctxs, opt, o.name);
            if (!o.fp_result || o.opc == 0x1d)   // conversions, compares, estimates: SIMD scalar forms
                n += check_batch(enc::s2(o.u, size, o.opc, 2, 1), ctxs1, opt, o.name);
        }
        for (unsigned rnd : {0u, 1u, 2u, 3u, 4u, 6u, 7u})   // scalar FRINT N P M Z A X I
            n += check_batch(enc::fp1(8 + rnd, es == 64 ? 1 : 0, 2, 1), ctxs1, fp_opt(es), "frint scalar");
        std::printf("    unary %s: %u contexts\n", es == 32 ? "S" : "D", n);
    }
}

FX_TEST(fp_special_conversions_to_gpr) {
    static const unsigned rm_opc[][2] = {{0, 0}, {0, 1}, {0, 4}, {0, 5}, {1, 0},
                                         {1, 1}, {2, 0}, {2, 1}, {3, 0}, {3, 1}};
    for (unsigned es : {32u, 64u}) {
        const auto ctxs = unary_contexts(es, 1, 1);
        unsigned n = 0;
        for (unsigned sf = 0; sf < 2; ++sf) {
            for (const auto& p : rm_opc) n += check_batch(enc::fpint(sf, es == 64 ? 1 : 0, p[0], p[1], 0, 1), ctxs, exact_opt(), "fcvt to gpr");
            for (unsigned scale : {0u, 1u, 31u, 32u, 33u, 48u, 63u}) {
                if (!sf && scale < 32) continue;
                for (unsigned opc : {0u, 1u}) n += check_batch(enc::fpfix(sf, es == 64 ? 1 : 0, 3, opc, scale, 0, 1), ctxs, exact_opt(), "fcvtz fixed");
            }
        }
        std::printf("    to gpr %s: %u contexts\n", es == 32 ? "S" : "D", n);
    }
}

FX_TEST(fp_special_conversions_from_gpr) {
    static const std::uint64_t ints[] = {0, 1, 2, 3, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x00ffffff, 0x01000000,
                                         0x01000001, 0x01ffffff, 0x7fffffbf, 0x7fffffc0, 0x7fffffff, 0x80000000ULL,
                                         0x80000001ULL, 0xffffff80ULL, 0xffffffffULL, 0x100000000ULL,
                                         0x001fffffffffffffULL, 0x0020000000000000ULL, 0x0020000000000001ULL,
                                         0x7ffffffffffffdffULL, 0x7ffffffffffffe00ULL, 0x7fffffffffffffffULL,
                                         0x8000000000000000ULL, 0x8000000000000001ULL, 0xfffffffffffff800ULL,
                                         0xfffffffffffffbffULL, 0xfffffffffffffc00ULL, 0xffffffffffffffffULL,
                                         0x8000008000000000ULL, 0x8000008000000001ULL, 0xffffff7fffffffffULL};
    std::vector<GuestContext> ctxs;
    Rng rng(9);
    for (const auto fpcr : fpcr_modes()) {
        for (auto v : ints) {
            GuestContext c = testing::random_context(rng.next());
            c.fpcr = fpcr;
            c.x[1] = v;
            c.v[1][0] = v;
            ctxs.push_back(c);
        }
    }
    unsigned n = 0;
    for (unsigned es : {32u, 64u}) {
        const unsigned ft = es == 64 ? 1 : 0;
        for (unsigned sf = 0; sf < 2; ++sf) {
            for (unsigned opc : {2u, 3u}) {
                n += check_batch(enc::fpint(sf, ft, 0, opc, 0, 1), ctxs, fp_opt(es), "scvtf/ucvtf gpr");
                for (unsigned scale : {1u, 32u, 40u, 63u}) {
                    if (!sf && scale < 32) continue;
                    n += check_batch(enc::fpfix(sf, ft, 0, opc, scale, 0, 1), ctxs, fp_opt(es), "scvtf/ucvtf fixed");
                }
            }
        }
        for (unsigned u = 0; u < 2; ++u) n += check_batch(enc::s2(u, es == 64 ? 1 : 0, 0x1d, 0, 1), ctxs, fp_opt(es), "scvtf/ucvtf simd");
    }
    std::printf("    from gpr: %u contexts\n", n);
}

namespace {

// Min/max do not round: of the FPCR modes only FZ changes them (the bit-exact
// tests keep FPCR = 0 and FZ, a quarter of the contexts).
constexpr std::uint32_t kMinMaxFpcr[] = {0u, 1u << 24};
std::vector<GuestContext> rounding_free(std::vector<GuestContext> ctxs) {
    std::erase_if(ctxs, [](const GuestContext& c) { return c.fpcr != 0u && c.fpcr != (1u << 24); });
    return ctxs;
}

// Vd equal to a source checks register handling more than new values: every
// other context (keeps the suite inside its ctest timeout).
std::vector<GuestContext> every_other(const std::vector<GuestContext>& ctxs) {
    std::vector<GuestContext> out;
    for (std::size_t i = 0; i < ctxs.size(); i += 2) out.push_back(ctxs[i]);
    return out;
}

} // namespace

// FMAX/FMIN/FMAXNM/FMINNM NaN results bit for bit (FPCR.DN = 0): every pair of
// specials (quiet and signalling NaN payloads, +-0, denormals, infinities,
// normals) in every lane, scalar S/D and vector .2S/.4S/.2D, Vd distinct from
// and equal to each source, with and without FZ, both register
// contexts. The reference interpreter follows FPProcessNaNs.
FX_TEST(fp_special_pairs_minmax_bit_exact) {
    unsigned total = 0;
    for (unsigned es : {32u, 64u}) {
        const unsigned sz = es == 64 ? 1 : 0;
        for (unsigned op = 0; op < 4; ++op) {   // FMAX, FMIN, FMAXNM, FMINNM
            const unsigned a = op & 1u, opc = (op & 2u) ? 0x18 : 0x1e;
            const char* name = op == 0 ? "fmax" : op == 1 ? "fmin" : op == 2 ? "fmaxnm" : "fminnm";
            for (const auto [rd, rn, rm] : {std::tuple{3u, 1u, 2u}, std::tuple{1u, 1u, 2u}, std::tuple{2u, 1u, 2u}}) {
                for (unsigned q = es == 64 ? 1 : 0; q < 2; ++q) {
                    const auto all = rounding_free(pair_contexts(es, 1, 2, true, (q ? 128 : 64) / es));
                    const auto ctxs = rd == 3 ? all : every_other(all);
                    const std::uint32_t word = enc::v3(q, 0, (a << 1) | sz, opc, rd, rn, rm);
                    const unsigned n = check_batch(word, ctxs, exact_opt(), std::string(name) + " vector exact");
                    FX_CHECK(n == ctxs.size());
                    total += n;
                }
                const auto all = rounding_free(pair_contexts(es, 1, 2, true, 1));
                const auto ctxs = rd == 3 ? all : every_other(all);
                const unsigned scalar_op = op == 0 ? 4 : op == 1 ? 5 : op == 2 ? 6 : 7;
                const unsigned n = check_batch(enc::fp2(scalar_op, sz, rd, rn, rm), ctxs, exact_opt(), std::string(name) + " scalar exact");
                FX_CHECK(n == ctxs.size());
                total += n;
            }
        }
    }
    std::printf("    %u contexts bit-exact\n", total);
}

// FMAXP/FMINP/FMAXNMP/FMINNMP (vector and scalar) and FMAXV/FMINV/FMAXNMV/
// FMINNMV NaN results bit for bit (FPCR.DN = 0): A64's operand order
// op(n0, n1) / Reduce = op(op(n0, n1), op(n2, n3)), quiet and signalling NaN
// payloads, +-0, denormals, infinities, with and without FZ.
FX_TEST(fp_special_pairwise_across_minmax_bit_exact) {
    unsigned total = 0;
    for (unsigned op = 0; op < 4; ++op) {   // MAX, MIN, MAXNM, MINNM
        const unsigned a = op & 1u, nm = op >> 1;
        const char* name = op == 0 ? "max" : op == 1 ? "min" : op == 2 ? "maxnm" : "minnm";
        for (unsigned es : {32u, 64u}) {
            const unsigned sz = es == 64 ? 1 : 0;
            // Vector pairwise, Vd distinct from and equal to a source.
            for (const auto [rd, rn, rm] : {std::tuple{3u, 1u, 2u}, std::tuple{1u, 1u, 2u}, std::tuple{2u, 1u, 2u}})
                for (unsigned q = es == 64 ? 1 : 0; q < 2; ++q) {
                    const auto all = rounding_free(pair_contexts(es, 1, 2, true, (q ? 128 : 64) / es));
                    const auto ctxs = rd == 3 ? all : every_other(all);
                    const std::uint32_t word = enc::v3(q, 1, (a << 1) | sz, nm ? 0x18 : 0x1e, rd, rn, rm);
                    const unsigned n = check_batch(word, ctxs, exact_opt(), std::string("fp") + name + "p vector exact");
                    FX_CHECK(n == ctxs.size());
                    total += n;
                }
            // Scalar pairwise: the pair in lanes 0 and 1 of Vn (Vd distinct and equal).
            const auto list = special_list(es, true);
            Rng rng(es * 7 + op);
            std::vector<GuestContext> ctxs;
            for (const auto fpcr : kMinMaxFpcr)
                for (const auto x : list)
                    for (const auto y : list) {
                        GuestContext c = testing::random_context(rng.next());
                        fill_vectors(c, rng, es, true);
                        c.fpcr = fpcr;
                        set_lane(c, 1, es, 0, x);
                        set_lane(c, 1, es, 1, y);
                        ctxs.push_back(c);
                    }
            for (const unsigned rd : {3u, 1u}) {
                const std::uint32_t word = 0x7e300800u | (a << 23) | (sz << 22) | ((nm ? 0x0cu : 0x0fu) << 12) | (1u << 5) | rd;
                const auto some = rd == 3 ? ctxs : every_other(ctxs);
                const unsigned n = check_batch(word, some, exact_opt(), std::string("fp") + name + "p scalar exact");
                FX_CHECK(n == some.size());
                total += n;
            }
        }
        // Across lanes (.4S): random quadruples of specials, Vd distinct and equal.
        const auto list = special_list(32, true);
        Rng rng(4099 + op);
        std::vector<GuestContext> ctxs;
        for (const auto fpcr : kMinMaxFpcr)
            for (unsigned k = 0; k < 400; ++k) {
                GuestContext c = testing::random_context(rng.next());
                fill_vectors(c, rng, 32, true);
                c.fpcr = fpcr;
                for (unsigned l = 0; l < 4; ++l) set_lane(c, 1, 32, l, list[rng.next() % list.size()]);
                ctxs.push_back(c);
            }
        for (const unsigned rd : {3u, 1u}) {
            const std::uint32_t word = 0x6e300800u | (a << 23) | ((nm ? 0x0cu : 0x0fu) << 12) | (1u << 5) | rd;
            const auto some = rd == 3 ? ctxs : every_other(ctxs);
            const unsigned n = check_batch(word, some, exact_opt(), std::string("fp") + name + "v exact");
            FX_CHECK(n == some.size());
            total += n;
        }
    }
    std::printf("    %u contexts bit-exact\n", total);
}
