// fp family tests: scalar floating point (owner: fp).
#include "fx/translator/emit_fp_tests.hpp"

#include <cstring>

using namespace transmute::fx;
using namespace transmute::fx::fptest;

namespace {

unsigned ftype_of(unsigned es) { return es == 64 ? 1 : es == 32 ? 0 : 3; }

Spec two_source(const char* name, unsigned es, std::initializer_list<unsigned> opcodes, bool snan) {
    std::vector<unsigned> ops(opcodes);
    Spec s{name, [ops, es](Rng& r) {
               return enc::fp2(ops[r.below(static_cast<unsigned>(ops.size()))], ftype_of(es), reg(r), reg(r), reg(r));
           }, es, fp_opt(es)};
    s.snan = snan;
    s.words = 96;
    s.opt.reference = [](std::uint32_t w, GuestContext& c) {
        const unsigned opcode = (w >> 12) & 15;
        if (opcode >= 4 && opcode <= 7) return ref_scalar_minmax(w, c);
        const auto pc = c.pc;
        const bool ok = testing::oracle_step(c, w);
        c.pc = pc;
        return ok;
    };
    return s;
}

// FMOV Xd, Vn.D[1] / FMOV Vd.D[1], Xn (missing from the reference interpreter).
bool ref_fmov_d1(std::uint32_t w, GuestContext& c) {
    const unsigned rd = w & 31, rn = (w >> 5) & 31;
    if (((w >> 16) & 7) == 6) {
        if (rd != 31) c.x[rd] = c.v[rn][1];
    } else {
        c.v[rd][1] = rn == 31 ? 0 : c.x[rn];
    }
    return true;
}

} // namespace

FX_TEST(fp_scalar_arith_matches_oracle) {
    run_specs({
        two_source("fmul/fdiv/fadd/fsub S", 32, {0, 1, 2, 3, 8}, true),
        two_source("fmul/fdiv/fadd/fsub D", 64, {0, 1, 2, 3, 8}, true),
    });
}

FX_TEST(fp_scalar_minmax_matches_oracle) {
    run_specs({
        two_source("fmax/fmin S", 32, {4, 5}, true),
        two_source("fmax/fmin D", 64, {4, 5}, true),
        two_source("fmaxnm/fminnm S", 32, {6, 7}, false),
        two_source("fmaxnm/fminnm D", 64, {6, 7}, false),
    });
}

FX_TEST(fp_scalar_fused_matches_oracle) {
    for (unsigned es : {32u, 64u}) {
        Spec s{es == 32 ? "fmadd family S" : "fmadd family D", [es](Rng& r) {
                   return enc::fp3(r.below(2), r.below(2), ftype_of(es), reg(r), reg(r), reg(r), reg(r));
               }, es, fp_opt(es)};
        s.words = 96;
        run_specs({s});
    }
}

// Both factors and the destination in one register (Chrome's `fmsub s0, s0, s0, s29`, 0x1f00f400): with the
// register in memory, the emitter loaded the second factor after claiming the destination and stopped with
// "fp: guest vector read after write". The memory-sources aliasing variant is that case.
FX_TEST(fp_fused_factors_alias_destination) {
    FX_CHECK_EQ(enc::fp3(0, 1, 0, 0, 0, 0, 29), 0x1f00f400u);
    unsigned checked = 0;
    for (unsigned es : {32u, 64u}) {
        for (unsigned op = 0; op < 4; ++op) {
            const unsigned o1 = op >> 1, o0 = op & 1;
            // rd = rn = rm (ra apart); rd = rn = rm = ra; rd = rm with rn = rm; rd = ra = rn.
            for (const auto word : {enc::fp3(o1, o0, ftype_of(es), 0, 0, 0, 29), enc::fp3(o1, o0, ftype_of(es), 7, 7, 7, 7),
                                    enc::fp3(o1, o0, ftype_of(es), 3, 3, 3, 9), enc::fp3(o1, o0, ftype_of(es), 5, 5, 2, 5)}) {
                for (std::uint64_t seed = 1; seed <= 4; ++seed) {
                    GuestContext ctx = testing::random_context(seed * 977 + word);
                    Rng rng(seed);
                    fill_vectors(ctx, rng, es, false);
                    char label[64];
                    std::snprintf(label, sizeof(label), "aliased fused 0x%08x", word);
                    check_word(word, ctx, fp_opt(es), label);
                    ++checked;
                }
            }
        }
        // FMLA/FMLS (vector): the accumulator and both factors in one register.
        for (const unsigned fmls : {0u, 1u}) {
            const auto word = enc::v3(1, 0, (fmls << 1) | (es == 64 ? 1u : 0u), 25, 4, 4, 4);
            GuestContext ctx = testing::random_context(word);
            Rng rng(word);
            fill_vectors(ctx, rng, es, false);
            check_word(word, ctx, fp_opt(es), "aliased fmla/fmls vector");
            ++checked;
        }
    }
    FX_CHECK(checked == 132);
}

FX_TEST(fp_scalar_one_source_matches_oracle) {
    for (unsigned es : {32u, 64u}) {
        Spec s{es == 32 ? "fmov/fabs/fneg/fsqrt/frint S" : "fmov/fabs/fneg/fsqrt/frint D", [es](Rng& r) {
                   static const unsigned ops[] = {0, 1, 2, 3, 8, 9, 10, 11, 12, 14, 15};
                   return enc::fp1(ops[r.below(11)], ftype_of(es), reg(r), reg(r));
               }, es, fp_opt(es)};
        s.words = 120;
        run_specs({s});
    }
}

FX_TEST(fp_scalar_precision_conversions_match_oracle) {
    run_specs({
        Spec{"fcvt S->D", [](Rng& r) { return enc::fp1(5, 0, reg(r), reg(r)); }, 32, fp_opt(64)},
        Spec{"fcvt D->S", [](Rng& r) { return enc::fp1(4, 1, reg(r), reg(r)); }, 64, fp_opt(32)},
    });
}

FX_TEST(fp_scalar_compare_matches_oracle) {
    for (unsigned es : {32u, 64u}) {
        Spec s{es == 32 ? "fcmp/fcmpe S" : "fcmp/fcmpe D", [es](Rng& r) {
                   return enc::fcmp(ftype_of(es), reg(r), reg(r), r.below(4) * 8);
               }, es, fp_opt(es)};
        s.words = 64;
        Spec c{es == 32 ? "fccmp/fccmpe S" : "fccmp/fccmpe D", [es](Rng& r) {
                   return enc::fccmp(ftype_of(es), reg(r), reg(r), r.below(16), r.below(2), r.below(16));
               }, es, fp_opt(es)};
        c.words = 96;
        Spec sel{es == 32 ? "fcsel S" : "fcsel D", [es](Rng& r) {
                     return enc::fcsel(ftype_of(es), reg(r), reg(r), reg(r), r.below(16));
                 }, es, exact_opt()};
        sel.words = 96;
        run_specs({s, c, sel});
    }
}

FX_TEST(fp_scalar_fmov_immediate_all_values) {
    for (unsigned es : {32u, 64u}) {
        Spec s{es == 32 ? "fmov imm S" : "fmov imm D", [es](Rng& r) {
                   return enc::fmov_imm(ftype_of(es), reg(r), r.below(256));
               }, es, exact_opt()};
        s.words = 256;
        s.contexts = 1;
        run_specs({s});
    }
}

FX_TEST(fp_scalar_int_conversions_match_oracle) {
    for (unsigned es : {32u, 64u}) {
        // FCVT{N,A,P,M,Z}{S,U} to W/X
        Spec f2i{es == 32 ? "fcvt*s/u S->gpr" : "fcvt*s/u D->gpr", [es](Rng& r) {
                     static const unsigned rm_opc[][2] = {{0, 0}, {0, 1}, {0, 4}, {0, 5}, {1, 0},
                                                          {1, 1}, {2, 0}, {2, 1}, {3, 0}, {3, 1}};
                     const auto& p = rm_opc[r.below(10)];
                     return enc::fpint(r.below(2), ftype_of(es), p[0], p[1], reg(r), reg(r));
                 }, es, exact_opt()};
        f2i.words = 160;
        Spec i2f{es == 32 ? "scvtf/ucvtf gpr->S" : "scvtf/ucvtf gpr->D", [es](Rng& r) {
                     return enc::fpint(r.below(2), ftype_of(es), 0, 2 + r.below(2), reg(r), reg(r));
                 }, es, fp_opt(es)};
        i2f.words = 96;
        Spec fix{es == 32 ? "fixed point S" : "fixed point D", [es](Rng& r) {
                     const unsigned sf = r.below(2);
                     const unsigned scale = sf ? r.below(64) : 32 + r.below(32);
                     static const unsigned rm_opc[][2] = {{0, 2}, {0, 3}, {3, 0}, {3, 1}};
                     const auto& p = rm_opc[r.below(4)];
                     return enc::fpfix(sf, ftype_of(es), p[0], p[1], scale, reg(r), reg(r));
                 }, es, fp_opt(es)};
        fix.words = 160;
        run_specs({f2i, i2f, fix});
    }
}

FX_TEST(fp_scalar_gpr_moves_match_oracle) {
    run_specs({
        Spec{"fmov W<->S, X<->D", [](Rng& r) {
                 const unsigned sf = r.below(2);
                 return enc::fpint(sf, sf ? 1 : 0, 0, 6 + r.below(2), reg(r), reg(r));
             }, 32, exact_opt()},
        [] {
            Spec s{"fmov X<->V.D[1] (ref)", [](Rng& r) { return enc::fpint(1, 2, 1, 6 + r.below(2), reg(r), reg(r)); },
                   32, exact_opt()};
            s.reference = ref_fmov_d1;
            return s;
        }(),
    });
}

FX_TEST(fp_fjcvtzs_matches_reference) {
    Spec s{"fjcvtzs (ref)", [](Rng& r) { return 0x1e7e0000u | reg(r) << 5 | reg(r); }, 64, exact_opt()};
    s.reference = ref_fjcvtzs;
    s.words = 128;
    s.prepare = [](GuestContext& c, Rng& r) {
        static const double vals[] = {0.0, -0.0, 1.0, -1.0, 1.5, -1.5, 2147483647.0, 2147483648.0, -2147483648.0,
                                      -2147483649.0, 4294967296.0, 4294967297.0, 9.2233720368547758e18,
                                      -9.2233720368547758e18, 1.8446744073709552e19, 1e300, -1e300, 3.5e15, 0.5};
        for (auto& v : c.v)
            if (r.below(2)) {
                const double d = vals[r.below(static_cast<unsigned>(std::size(vals)))];
                std::memcpy(&v[0], &d, 8);
            }
    };
    run_specs({s});
}
