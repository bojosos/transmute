// End-to-end: NEON families inside real translated regions (translator core
// RegionEmitter, linking, fault map) against the oracle (owner: neon).
#include "fx/testing/harness.hpp"
#include "fx/testing/test.hpp"
#include "fx/translator/core_isel.hpp"
#include "fx/translator/core_test_gen.hpp"
#include "fx/translator/core_test_machine.hpp"
#include "fx/translator/emit_fp.hpp"
#include "fx/translator/emit_neon.hpp"
#include "fx/translator/emit_neon_internal.hpp"
#include "fx/translator/region_emitter.hpp"
#include "fx/translator/translator.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace transmute::fx;
using namespace transmute::fx::coretest;

namespace {

void ensure_registered() {
    static const bool once = [] {
        register_fp_emitters();
        register_neon_emitters();
        return true;
    }();
    (void)once;
}

constexpr std::uint32_t b_cond(unsigned cond, int offset_words) {
    return 0x54000000u | ((static_cast<std::uint32_t>(offset_words) & 0x7ffffu) << 5) | cond;
}
constexpr std::uint32_t kNe = 1;
constexpr std::uint32_t movz_x(unsigned rd, unsigned imm16) { return 0xd2800000u | (imm16 << 5) | rd; }
constexpr std::uint32_t subs_x_imm(unsigned rd, unsigned rn, unsigned imm12) {
    return 0xf1000000u | (imm12 << 10) | (rn << 5) | rd;
}

void run_program(const std::vector<std::uint32_t>& words, const char* label, unsigned seeds = 4) {
    ensure_registered();
    DiffRunner runner;
    for (std::uint64_t seed = 0; seed < seeds; ++seed) {
        const auto ctx = runner.context(seed);
        if (!runner.run(words, ctx, label)) return;
        // Every NEON word must have been translated, not interpreted.
        FX_CHECK_EQ(runner.last.interprets, 0u);
        FX_CHECK(runner.last.translations >= 1u);
        FX_CHECK(runner.oracle_steps >= words.size());
    }
}

} // namespace

FX_TEST(neon_region_vector_add_loop) {
    // x0 = 16; loop: ldp q0, q1, [x16], #32; add v2.4s, v0.4s, v1.4s;
    // eor v3.16b, v2.16b, v0.16b; str q3, [x17], #16; subs x0, x0, #1; b.ne loop
    run_program({movz_x(0, 16), 0xacc10600u, 0x4ea18402u, 0x6e201c43u, 0x3c810623u, subs_x_imm(0, 0, 1),
                 b_cond(kNe, -5)},
                "vector_add_loop");
}

FX_TEST(neon_region_shuffle_reduce_loop) {
    // x0 = 8; loop: ld1 {v0.16b, v1.16b}, [x16], #32; tbl v2.16b, {v0.16b}, v1.16b;
    // zip1 v3.16b, v2.16b, v0.16b; uzp2 v4.8h, v3.8h, v1.8h; ext v5.16b, v4.16b, v3.16b, #3;
    // addv s6, v5.4s; umov w1, v6.s[0]; add x2, x2, x1; subs x0, x0, #1; b.ne loop
    run_program({movz_x(0, 8), 0x4cdfa200u, 0x4e010002u, 0x4e003843u, 0x4e415864u, 0x6e031885u, 0x4eb1b8a6u,
                 0x0e043cc1u, 0x8b010042u, subs_x_imm(0, 0, 1), b_cond(kNe, -9)},
                "shuffle_reduce_loop");
}

FX_TEST(neon_region_structure_loop) {
    // x0 = 4; loop: ld4 {v0.16b-v3.16b}, [x16], #64; uqadd v4.16b, v0.16b, v1.16b;
    // urhadd v5.16b, v2.16b, v3.16b; sqdmulh v6.8h, v4.8h, v5.8h; st2 {v5.16b, v6.16b}, [x17], #32;
    // subs x0, x0, #1; b.ne loop
    run_program({movz_x(0, 4), 0x4cdf0200u, 0x6e210c04u, 0x6e231445u, 0x4e65b486u, 0x4c9f8225u,
                 subs_x_imm(0, 0, 1), b_cond(kNe, -6)},
                "structure_loop");
}

FX_TEST(neon_region_movemask_and_lanes) {
    // cmeq v1.16b, v0.16b, v2.16b; ushr v1.16b, v1.16b, #7; usra v1.8h, v1.8h, #7;
    // usra v1.4s, v1.4s, #14; usra v1.2d, v1.2d, #28; umov w0, v1.b[0]; umov w1, v1.b[8];
    // dup v3.4s, w0; ins v3.s[2], w1; ld1 {v3.s}[1], [x16]; ld1r {v4.8h}, [x17]; st1 {v3.d}[1], [x17], #8
    run_program({0x6e228c01u, 0x6f090421u, 0x6f191421u, 0x6f321421u, 0x6f641421u, 0x0e013c20u, 0x0e113c21u,
                 0x4e040c03u, 0x4e141c23u, 0x0d409203u, 0x4d40c624u, 0x4d9f8623u},
                "movemask_and_lanes");
}

FX_TEST(neon_region_guarded_movemask_flags_and_extract) {
    ensure_registered();
    struct RestoreKnobs { ~RestoreKnobs() { neon::neon_set_knobs_for_testing(-1); } } restore;
    DiffRunner runner;
    for (const unsigned knobs : {0u, unsigned(neon::neon_mask_no_rcx)}) {
      neon::neon_set_knobs_for_testing(knobs);
      for (unsigned seed = 0; seed < 32; ++seed) {
        auto ctx = runner.context(seed);
        if (seed < 16) {
            ctx.v[0][0] = ctx.v[0][1] = 0x0000000100000001ull;
            ctx.v[1][0] = 0x0000000100000000ull;
            ctx.v[1][1] = 0x0000000300000002ull;
        }
        ctx.x[0] = seed & 3;
        ctx.x[1] = 2;
        FX_REQUIRE(runner.run({
            0xeb01001fu, // cmp x0,x1: flags must survive the complete reduction
            0x4e201c42u, // and v2.16b,v2.16b,v0.16b
            0x6ea14442u, // ushl v2.4s,v2.4s,v1.4s
            0x4eb1b842u, // addv s2,v2.4s
            0x1e260040u | ((seed & 1) ? 31u : 13u), // fmov w13/sink WZR,s2
            0x1a9f17e3u, // cset w3,eq
        }, ctx, "guarded movemask and live compare flags"));
        FX_CHECK_EQ(runner.last.interprets, 0u);
        if (neon::features().avx2) {
            // Verify the real context actually selected the intended guard.
            // LEA RCX,[RCX-15]; JRCXZ identifies the flags-preserving form.
            const std::uint8_t signature[] = {0x48, 0x8d, 0x49, 0xf1, 0xe3};
            bool has_rcx_guard = false;
            for (const auto& region : runner.machine.regions)
                has_rcx_guard |= std::search(region.code.begin(), region.code.end(),
                                            std::begin(signature), std::end(signature)) != region.code.end();
            FX_CHECK_EQ(has_rcx_guard, knobs == 0);
        }
      }
    }
}

FX_TEST(neon_region_guarded_movemask_internal_entries) {
    ensure_registered();
    DiffRunner runner;
    for (unsigned entry = 1; entry < 4; ++entry) {
        for (unsigned take = 0; take < 2; ++take) {
            auto ctx = runner.context(1234 + entry);
            ctx.x[5] = take;
            ctx.v[0][0] = ctx.v[0][1] = 0x0000000100000001ull;
            ctx.v[1][0] = 0x0000000100000000ull;
            ctx.v[1][1] = 0x0000000300000002ull;
            FX_REQUIRE(runner.run({
                0xb5000005u | ((entry + 1) << 5), // cbnz x5, one of the internal instructions
                0x4e201c42u, 0x6ea14442u, 0x4eb1b842u, 0x1e26004du,
            }, ctx, "guarded movemask internal branch target"));
            FX_CHECK_EQ(runner.last.interprets, 0u);
        }
    }
}

// ---- fp + neon: upper-lane tracking and memory operands --------------------------
// Scalar results skip their zeroing when the first source is known to have
// zero upper lanes (EmitContext::vec_zero_above); unpinned sources are folded
// as memory operands (EmitContext::vsrc). Random contexts give every other
// register garbage upper lanes, so a wrong elision shows up as a mismatch.

FX_TEST(fp_region_scalar_chain_upper_lanes) {
    run_program({
        0x1e220020u, // scvtf s0, w1
        0x1e220041u, // scvtf s1, w2
        0x9e620062u, // scvtf d2, x3
        0x4e0c1c83u, // mov v3.s[1], w4
        0x4e1c1ca3u, // mov v3.s[3], w5
        0x1e2f1003u, // fmov s3, #1.50000000
        0x1e212804u, // fadd s4, s0, s1
        0x1e200885u, // fmul s5, s4, s0
        0x1f0110a6u, // fmadd s6, s5, s1, s4
        0x1e2338c7u, // fsub s7, s6, s3
        0x1e20c0a8u, // fabs s8, s5
        0x1e21c108u, // fsqrt s8, s8
        0x1e22c109u, // fcvt d9, s8
        0x1e62292au, // fadd d10, d9, d2
        0x1e6a094bu, // fmul d11, d10, d10
        0x1e62416cu, // fcvt s12, d11
        0x1e20c0edu, // fabs s13, s7
        0x1e2141aeu, // fneg s14, s13
        0x1e2449cfu, // fmax s15, s14, s4
        0x1e2559f0u, // fmin s16, s15, s5
        0x1e2d6a11u, // fmaxnm s17, s16, s13
        0x1e207a32u, // fminnm s18, s17, s0
        0x1e211a53u, // fdiv s19, s18, s1
        0x1e204274u, // fmov s20, s19
        0x1e342260u, // fcmp s19, s20
        0x1e27ce75u, // fcsel s21, s19, s7, gt
        0xbd400216u, // ldr s22, [x16]
        0xfd400617u, // ldr d23, [x16, #0x8]
        0x1e352ad8u, // fadd s24, s22, s21
        0x1e6a2af9u, // fadd d25, d23, d10
        0xbd000238u, // str s24, [x17]
        0xfd000639u, // str d25, [x17, #0x8]
    }, "scalar_chain", 6);
}

FX_TEST(fp_region_loop_resets_upper_lane_knowledge) {
    // s0 has zero upper lanes on entry to the loop but not on the back edge
    // (INS writes lane 2): the loop header must forget what it knew.
    run_program({
        0xd28000a0u, // mov x0, #0x5 // =5
        0xbd400200u, // ldr s0, [x16]
        0x1e220042u, // scvtf s2, w2
        0x1e222801u, // fadd s1, s0, s2
        0x1e220825u, // fmul s5, s1, s2
        0x4e141c60u, // mov v0.s[2], w3
        0x4e1c1c85u, // mov v5.s[3], w4
        0x1e2128a6u, // fadd s6, s5, s1
        0xf1000400u, // subs x0, x0, #0x1
        0x54ffff41u, // b.ne 0xc <loop>
        0x1e2028c7u, // fadd s7, s6, s0
        0x1e270808u, // fmul s8, s0, s7
    }, "loop_upper_lanes", 6);
}

FX_TEST(fp_region_vector_memory_operands) {
    // More live vector registers than pinning slots: many sources are in
    // GuestContext and get folded as memory operands / broadcasts.
    run_program({
        0x4e21da80u, // scvtf v0.4s, v20.4s
        0x4e21daa1u, // scvtf v1.4s, v21.4s
        0x4e21dac2u, // scvtf v2.4s, v22.4s
        0x4e21dae3u, // scvtf v3.4s, v23.4s
        0x4e21db04u, // scvtf v4.4s, v24.4s
        0x4e21db25u, // scvtf v5.4s, v25.4s
        0x4e21db46u, // scvtf v6.4s, v26.4s
        0x4e21db67u, // scvtf v7.4s, v27.4s
        0x4e21db88u, // scvtf v8.4s, v28.4s
        0x4e21dba9u, // scvtf v9.4s, v29.4s
        0x4e21dbcau, // scvtf v10.4s, v30.4s
        0x4e21dbebu, // scvtf v11.4s, v31.4s
        0x4e21da6cu, // scvtf v12.4s, v19.4s
        0x4ea1d40du, // fsub v13.4s, v0.4s, v1.4s
        0x4ea3d44eu, // fsub v14.4s, v2.4s, v3.4s
        0x4f8499afu, // fmul v15.4s, v13.4s, v4.s[2]
        0x4fa591d0u, // fmul v16.4s, v14.4s, v5.s[1]
        0x4fa718cfu, // fmla v15.4s, v6.4s, v7.s[3]
        0x4ea9cd10u, // fmls v16.4s, v8.4s, v9.4s
        0x4e30f5f1u, // fmax v17.4s, v15.4s, v16.4s
        0x4eaaf632u, // fmin v18.4s, v17.4s, v10.4s
        0x4e2bf631u, // fmax v17.4s, v17.4s, v11.4s
        0x4eb2f592u, // fmin v18.4s, v12.4s, v18.4s
        0x4e32c633u, // fmaxnm v19.4s, v17.4s, v18.4s
        0x4ea0c674u, // fminnm v20.4s, v19.4s, v0.4s
        0x6e21e695u, // fcmge v21.4s, v20.4s, v1.4s
        0x4ea0cab6u, // fcmgt v22.4s, v21.4s, #0.0
        0x4e1c0457u, // dup v23.4s, v2.s[3]
        0x4e23d6f8u, // fadd v24.4s, v23.4s, v3.4s
        0x4ea0fb19u, // fabs v25.4s, v24.4s
        0x6ea0fb3au, // fneg v26.4s, v25.4s
        0x4e3a1ebbu, // and v27.16b, v21.16b, v26.16b
        0x4ebb1f7cu, // mov v28.16b, v27.16b
        0x6e661f95u, // bsl v21.16b, v28.16b, v6.16b
        0x6e2cff1du, // fdiv v29.4s, v24.4s, v12.4s
        0x4e219bbeu, // frintm v30.4s, v29.4s
        0x4ea1bbdfu, // fcvtzs v31.4s, v30.4s
        0x0e0c3fe1u, // mov w1, v31.s[1]
        0x5e1407c2u, // mov s2, v30.s[2]
        0x6e1e23e3u, // ext v3.16b, v31.16b, v30.16b, #0x4
        0x4e9f3864u, // zip1 v4.4s, v3.4s, v31.4s
        0x4e9e5885u, // uzp2 v5.4s, v4.4s, v30.4s
        0x1e260043u, // fmov w3, s2
    }, "vector_memory_operands", 6);
}

// ---- ADD/SUB address folding into SIMD&FP accesses --------------------------------
// The translator core folds an ADD/SUB Xd whose result only feeds the base of
// the next SIMD&FP load/store (FamilyEmitter::address_base) and is dead after
// it into the access's x64 operand (EmitContext::address). Random programs
// compare with the reference interpreter; the folded access's fault sites
// report the producer's pc. Faults of folded accesses rebuild exact guest
// state (fault maps included) and resume like the reference.

namespace {
namespace vf {

constexpr std::uint32_t add_imm(unsigned rd, unsigned rn, unsigned imm) { return 0x91000000u | (imm << 10) | (rn << 5) | rd; }
constexpr std::uint32_t sub_imm(unsigned rd, unsigned rn, unsigned imm) { return 0xd1000000u | (imm << 10) | (rn << 5) | rd; }
constexpr std::uint32_t add_lsl(unsigned rd, unsigned rn, unsigned rm, unsigned amount) {
    return 0x8b000000u | (rm << 16) | (amount << 10) | (rn << 5) | rd;
}
constexpr std::uint32_t add_ext(unsigned rd, unsigned rn, unsigned rm, unsigned option, unsigned amount) {
    return 0x8b200000u | (rm << 16) | (option << 13) | (amount << 10) | (rn << 5) | rd;
}
constexpr std::uint32_t movz(unsigned rd, unsigned imm16) { return 0xd2800000u | (imm16 << 5) | rd; }
constexpr std::uint32_t ldr_x(unsigned rt, unsigned rn, unsigned index8) { return 0xf9400000u | (index8 << 10) | (rn << 5) | rt; }
constexpr std::uint32_t eor16b(unsigned rd, unsigned rn, unsigned rm) { return 0x6e201c00u | (rm << 16) | (rn << 5) | rd; }
constexpr std::uint32_t add4s(unsigned rd, unsigned rn, unsigned rm) { return 0x4ea08400u | (rm << 16) | (rn << 5) | rd; }
// SIMD&FP LDR/STR: size_log 0..4 (B H S D Q).
constexpr std::uint32_t kUimm[5][2] = {{0x3d000000u, 0x3d400000u}, {0x7d000000u, 0x7d400000u}, {0xbd000000u, 0xbd400000u},
                                       {0xfd000000u, 0xfd400000u}, {0x3d800000u, 0x3dc00000u}};
constexpr std::uint32_t kUnscaled[5][2] = {{0x3c000000u, 0x3c400000u}, {0x7c000000u, 0x7c400000u}, {0xbc000000u, 0xbc400000u},
                                           {0xfc000000u, 0xfc400000u}, {0x3c800000u, 0x3cc00000u}};
constexpr std::uint32_t ldst_uimm(unsigned size_log, bool load, unsigned index, unsigned rn, unsigned rt) {
    return kUimm[size_log][load] | (index << 10) | (rn << 5) | rt;
}
constexpr std::uint32_t ldst_unscaled(unsigned size_log, bool load, int imm9, unsigned rn, unsigned rt) {
    return kUnscaled[size_log][load] | ((static_cast<std::uint32_t>(imm9) & 0x1ffu) << 12) | (rn << 5) | rt;
}
constexpr std::uint32_t ldst_reg(unsigned size_log, bool load, unsigned rm, unsigned option, bool s, unsigned rn, unsigned rt) {
    return kUnscaled[size_log][load] | 0x00200800u | (rm << 16) | (option << 13) | (s ? 1u << 12 : 0u) | (rn << 5) | rt;
}
// LDP/STP (mode 2) or LDNP/STNP (mode 0); opc 0 S, 1 D, 2 Q.
constexpr std::uint32_t ldst_pair(unsigned opc, unsigned mode, bool load, int imm7, unsigned rt2, unsigned rn, unsigned rt) {
    return 0x2c000000u | (opc << 30) | (mode << 23) | (load ? 1u << 22 : 0u) | ((static_cast<std::uint32_t>(imm7) & 0x7fu) << 15) |
           (rt2 << 10) | (rn << 5) | rt;
}
constexpr std::uint32_t ldst_multiple(bool q, bool load, unsigned opcode, unsigned size, unsigned rn, unsigned rt) {
    return 0x0c000000u | (q ? 1u << 30 : 0u) | (load ? 1u << 22 : 0u) | (opcode << 12) | (size << 10) | (rn << 5) | rt;
}
constexpr std::uint32_t ldst_lane_s(bool load, unsigned lane, unsigned rn, unsigned rt) {
    return 0x0d008000u | ((lane >> 1) << 30) | (load ? 1u << 22 : 0u) | ((lane & 1u) << 12) | (rn << 5) | rt;
}
constexpr std::uint32_t ldst_lane_d(bool load, unsigned lane, unsigned rn, unsigned rt) {
    return 0x0d008400u | (lane << 30) | (load ? 1u << 22 : 0u) | (rn << 5) | rt;
}
constexpr std::uint32_t ld1r(bool q, unsigned size, unsigned rn, unsigned rt) {
    return 0x0d40c000u | (q ? 1u << 30 : 0u) | (size << 10) | (rn << 5) | rt;
}

std::string hex(const std::vector<std::uint32_t>& words) {
    std::string s;
    char buf[16];
    for (const auto w : words) {
        std::snprintf(buf, sizeof buf, " %08x", w);
        s += buf;
    }
    return s;
}

bool fold_enabled() { return core::isel_on(core::isel_no_fold) && neon::neon_on(neon::neon_no_fold); }

struct Program {
    std::vector<std::uint32_t> words;
    unsigned add_at{};
    bool expect_fold{};
};

constexpr unsigned kXd[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29};

// Producer Xd = base + imm / - imm / + Xm << k / + ext(Wm) << k, then one
// SIMD&FP access through Xd; Xd then dead (overwritten), dead only after a
// faulting filler, read, or live until the end.
Program make_program(coretest::Rng& rng) {
    Program p;
    auto& w = p.words;
    const unsigned xd = rng.pick(kXd);
    if (rng.chance(25))   // Xd hot: pinned
        for (unsigned k = 0; k < 5; ++k) w.push_back(add_imm(xd, xd, 1));
    if (rng.chance(40)) w.push_back(add4s(20 + rng.below(4), 21, 22));   // a dirty cached vector
    static constexpr unsigned kBases[] = {16, 17, 31};
    unsigned base = rng.pick(kBases);
    const unsigned producer = rng.below(4);   // 0 add imm, 1 sub imm, 2 add lsl, 3 add ext
    if (producer == 2 && base == 31) base = 16;   // Rn 31 is XZR for shifted ADD
    const unsigned index = rng.chance(50) ? 14u : 12u;   // x14 <= 255, x12 in 1..4
    p.add_at = static_cast<unsigned>(w.size());
    switch (producer) {
    case 0: w.push_back(add_imm(xd, base, rng.below(1024))); break;
    case 1: w.push_back(sub_imm(xd, base, rng.below(1024))); break;
    case 2: w.push_back(add_lsl(xd, base, index, rng.below(4))); break;
    default: {
        static constexpr unsigned o[] = {2, 3, 6, 7};
        w.push_back(add_ext(xd, base, index, rng.pick(o), rng.below(4)));
        break;
    }
    }
    const bool load = rng.chance(60);
    const unsigned rt = rng.below(29);
    bool indexed = false;
    switch (rng.below(7)) {
    case 0: w.push_back(ldst_uimm(rng.below(5), load, rng.below(16), xd, rt)); break;
    case 1: w.push_back(ldst_unscaled(rng.below(5), load, static_cast<int>(rng.below(128)) - 64, xd, rt)); break;
    case 2: {
        static constexpr unsigned o[] = {2, 3, 6, 7};
        w.push_back(ldst_reg(rng.below(5), load, 12, rng.pick(o), rng.chance(50), xd, rt));
        indexed = true;
        break;
    }
    case 3: {
        unsigned rt2 = rng.below(29);
        if (rt2 == rt) rt2 = rt + 1;
        w.push_back(ldst_pair(rng.below(3), rng.chance(70) ? 2u : 0u, load, static_cast<int>(rng.below(8)) - 4, rt2, xd, rt));
        break;
    }
    case 4: {
        static constexpr unsigned ops[] = {0x7, 0xa, 0x6, 0x2, 0x8, 0x4, 0x0};
        const unsigned opcode = rng.pick(ops);
        const bool q = rng.chance(60) || opcode == 0x8 || opcode == 0x4 || opcode == 0x0;
        w.push_back(ldst_multiple(q, load, opcode, rng.below(4), xd, rt));
        break;
    }
    case 5:
        if (rng.chance(50)) w.push_back(ldst_lane_s(load, rng.below(4), xd, rt));
        else w.push_back(ldst_lane_d(load, rng.below(2), xd, rt));
        break;
    default: w.push_back(ld1r(rng.chance(50), rng.below(4), xd, rt)); break;
    }
    if (rng.chance(50)) w.push_back(eor16b(24 + rng.below(4), rt, 24));
    const unsigned shape = rng.below(4);   // 0 dead, 1 dead after a faulting filler, 2 read, 3 live out
    const unsigned other = xd == 9 ? 10u : 9u;
    if (shape == 0) {
        if (rng.chance(50)) w.push_back(add_imm(xd == 7 ? 8 : 7, other, 3));   // non-faulting filler
        if (rng.chance(50)) w.push_back(movz(xd, 0x77));
        else w.push_back(add_imm(xd, other, 5));
        p.expect_fold = !(indexed && producer >= 2);
    } else if (shape == 1) {
        w.push_back(ldr_x(xd == 5 ? 6 : 5, 16, rng.below(8)));   // may fault: Xd still observable
        w.push_back(movz(xd, 0x55));
    } else if (shape == 2) {
        w.push_back(add_imm(0, xd, 1));
    }
    return p;
}

coretest::DiffRunner& runner() {
    static coretest::DiffRunner r;
    return r;
}

} // namespace vf
} // namespace

FX_TEST(neon_region_address_folding_differential) {
    ensure_registered();
    auto& r = vf::runner();
    coretest::Rng rng(5151);
    unsigned folded = 0, expected = 0, failures = 0;
    constexpr unsigned kPrograms = 4000;
    for (unsigned i = 0; i < kPrograms && failures < 8; ++i) {
        const auto p = vf::make_program(rng);
        if (!r.run(p.words, r.context(616000 + i), "vfold" + vf::hex(p.words))) {
            ++failures;
            continue;
        }
        FX_CHECK_EQ(r.last.interprets, 0u);
        const auto add_pc = r.program_base() + 4ull * p.add_at;
        bool fold = false;
        for (const auto& region : r.machine.regions)
            for (const auto& f : region.faults) fold |= f.guest_pc == add_pc;
        expected += p.expect_fold;
        folded += fold;
        if (vf::fold_enabled() && p.expect_fold && !fold) {
            testing::report_failure(__FILE__, __LINE__, "expected a fold:" + vf::hex(p.words));
            ++failures;
        }
        if (fold && !p.expect_fold) {
            testing::report_failure(__FILE__, __LINE__, "unexpected fold:" + vf::hex(p.words));
            ++failures;
        }
        if (!vf::fold_enabled() && fold) {
            testing::report_failure(__FILE__, __LINE__, "fold with nofold:" + vf::hex(p.words));
            ++failures;
        }
    }
    std::printf("    folded %u of %u programs (%u expected)\n", folded, kPrograms, expected);
    if (vf::fold_enabled()) FX_CHECK(folded == expected && expected > 600);
}

FX_TEST(neon_region_address_folding_loop) {
    // x0 = 6; loop: add x8, x16, x0, lsl #3; ldr q1, [x8, #0x10]; add v2.4s, v2.4s, v1.4s;
    // sub x9, x17, #0x40; stur q2, [x9, #-0x20]; sub x8, x17, x0; ld1 {v3.4s, v4.4s}, [x8];
    // eor v5.16b, v3.16b, v4.16b; add x9, x16, #0x100; ldp q6, q7, [x9, #-0x20];
    // movz x8, #0; movz x9, #0; subs x0, x0, #1; b.ne loop
    using namespace vf;
    run_program({movz(0, 6), add_lsl(8, 16, 0, 3), ldst_uimm(4, true, 1, 8, 1), add4s(2, 2, 1), sub_imm(9, 17, 0x40),
                 ldst_unscaled(4, false, -0x20, 9, 2), 0xcb000228u /* sub x8, x17, x0 */, ldst_multiple(true, true, 0xa, 2, 8, 3),
                 eor16b(5, 3, 4), add_imm(9, 16, 0x100), ldst_pair(2, 2, true, -2, 7, 9, 6), movz(8, 0), movz(9, 0),
                 subs_x_imm(0, 0, 1), b_cond(kNe, -13)},
                "address_folding_loop");
}

// ---- exact guest state at faults of folded SIMD&FP accesses -------------------------

namespace {
namespace vf {

struct Fault {
    const TranslatedRegion* region{};
    std::uintptr_t host{};
    bool hit{};
    std::uint64_t guest_pc{};
    FlagsLocation flags{};
    std::uint64_t x[32]{};
    std::uint64_t v[32][2]{};
    std::uint32_t in_host{}, vin_host{};
    const void* resume{};
} g_fault;

LONG CALLBACK fault_handler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !g_fault.region) return EXCEPTION_CONTINUE_SEARCH;
    const auto rip = static_cast<std::uintptr_t>(info->ContextRecord->Rip);
    if (rip < g_fault.host || rip >= g_fault.host + g_fault.region->code.size()) return EXCEPTION_CONTINUE_SEARCH;
    const auto off = static_cast<std::uint32_t>(rip - g_fault.host);
    const DWORD64* regs = &info->ContextRecord->Rax;
    const M128A* xmm = &info->ContextRecord->Xmm0;
    for (const auto& f : g_fault.region->faults) {
        if (off < f.host_begin || off >= f.host_end) continue;
        g_fault.hit = true;
        g_fault.guest_pc = f.guest_pc;
        g_fault.flags = f.flags;
        for_each_fault_register(g_fault.region->pins, f, g_fault.region->fault_maps, g_fault.region->fault_map_regs,
                                [&](unsigned guest, unsigned host, bool vector) {
                                    if (vector) {
                                        g_fault.v[guest & 31][0] = xmm[host & 15].Low;
                                        g_fault.v[guest & 31][1] = static_cast<std::uint64_t>(xmm[host & 15].High);
                                        g_fault.vin_host |= 1u << (guest & 31);
                                    } else {
                                        g_fault.x[guest & 31] = regs[host & 15];
                                        g_fault.in_host |= 1u << (guest & 31);
                                    }
                                });
    }
    info->ContextRecord->Rip = reinterpret_cast<DWORD64>(g_fault.resume);
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Runs `prog` with x9 = an inaccessible page + x9_offset: the access after
// word `fault_word` faults. The reported pc is the producer's (folded) or the
// access's; the reconstructed guest state equals the reference state before
// that pc, and resuming there with the page readable ends like the reference.
void check_fault(const std::vector<std::uint32_t>& prog, unsigned fault_word, const char* label, unsigned x9_offset = 0x400) {
    ensure_registered();
    auto& r = runner();
    auto* page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    FX_REQUIRE(page);
    for (unsigned k = 0; k < 8192; ++k) page[k] = static_cast<std::uint8_t>(k * 37 + 11);
    DWORD old = 0;
    VirtualProtect(page, 8192, PAGE_NOACCESS, &old);
    auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(r.program_base()));
    std::memcpy(program, prog.data(), prog.size() * 4);
    program[prog.size()] = 0xd4200000u | (0xdeadu << 5);
    const coretest::BufferCode code(r.code_base(), r.program_base() + (prog.size() + 1) * 4);
    auto ctx = r.context(73000 + fault_word);
    ctx.x[9] = reinterpret_cast<std::uint64_t>(page) + x9_offset;
    ctx.x[10] = 3;
    ctx.x[11] = 0xfffffffeu;   // SXTW: -2
    const auto initial = ctx;

    r.machine.reset();
    r.machine.prepare(ctx);
    const auto* host = r.machine.entry_for(r.program_base(), code);
    FX_REQUIRE(host != nullptr);
    g_fault = {};
    g_fault.region = &r.machine.regions.back();
    g_fault.host = reinterpret_cast<std::uintptr_t>(host) - r.machine.regions.back().entries[0].offset;
    g_fault.resume = ctx.stub_dispatch_exit;
    auto* handler = AddVectoredExceptionHandler(1, fault_handler);
    ctx.pc = r.program_base();
    r.machine.enter(ctx, host);
    RemoveVectoredExceptionHandler(handler);
    FX_REQUIRE(g_fault.hit);
    const auto want_pc = r.program_base() + 4ull * (fold_enabled() ? fault_word : fault_word + 1);
    FX_CHECK_EQ(g_fault.guest_pc, want_pc);

    auto state = ctx;
    for (unsigned g = 0; g < 32; ++g) {
        if ((g_fault.in_host >> g) & 1u) state.x[g] = g_fault.x[g];
        if ((g_fault.vin_host >> g) & 1u) {
            state.v[g][0] = g_fault.v[g][0];
            state.v[g][1] = g_fault.v[g][1];
        }
    }
    FX_CHECK(g_fault.flags == FlagsLocation::context);
    state.nzcv = ctx.nzcv;
    state.pc = g_fault.guest_pc;
    auto before = initial;
    before.pc = r.program_base();
    while (before.pc != g_fault.guest_pc) {
        const auto word = *reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(before.pc));
        FX_REQUIRE(testing::oracle_step(before, word));
    }
    testing::compare_contexts(before, state, std::string(label) + " (state at the fault)");

    VirtualProtect(page, 8192, PAGE_READWRITE, &old);
    auto expect = initial;
    expect.pc = r.program_base();
    const auto stop = r.program_base() + prog.size() * 4;
    for (unsigned steps = 0; expect.pc != stop && steps < 1000; ++steps) {
        const auto word = *reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(expect.pc));
        FX_REQUIRE(testing::oracle_step(expect, word));
    }
    std::vector<std::uint8_t> expect_page(page, page + 8192);
    for (unsigned k = 0; k < 8192; ++k) page[k] = static_cast<std::uint8_t>(k * 37 + 11);
    r.machine.reset();
    const auto res = r.machine.run(state, code, kReturnToHostPc);
    FX_CHECK(res.ok && res.reason == ExitReason::brk && res.arg == 0xdead);
    expect.pc = state.pc;
    testing::compare_contexts(expect, state, label);
    FX_CHECK(std::memcmp(expect_page.data(), page, 8192) == 0);
    VirtualFree(page, 0, MEM_RELEASE);
}

} // namespace vf
} // namespace

FX_TEST(neon_region_folded_access_faults_are_exact) {
    using namespace vf;
    // x9 = inaccessible page + 0x400, x10 = 3, x11 = -2 (as W).
    check_fault({add_imm(8, 9, 0x40), ldst_uimm(4, true, 0, 8, 3), movz(8, 7)}, 0, "add imm + ldr q");
    check_fault({add4s(20, 21, 22), sub_imm(8, 9, 0x10), ldst_unscaled(4, true, -0x100, 8, 3), add4s(21, 20, 3), movz(8, 1)},
                1, "dirty vector + sub imm + ldur q");
    check_fault({add_lsl(8, 9, 10, 3), ldst_multiple(true, true, 0xa, 2, 8, 3), movz(8, 1)}, 0, "add lsl + ld1 x2");
    check_fault({add_imm(8, 9, 0x20), ldst_pair(2, 2, true, 1, 4, 8, 3), add_imm(8, 3, 1)}, 0, "add imm + ldp q");
    check_fault({add_ext(8, 9, 11, 6, 2), ldst_uimm(2, true, 0x10, 8, 3), movz(8, 2)}, 0, "add sxtw + ldr s");
    check_fault({add_imm(8, 9, 8), ldst_lane_s(false, 2, 8, 3), movz(8, 3)}, 0, "add imm + st1 lane");
    check_fault({add_imm(8, 9, 0x40), ldst_reg(4, true, 10, 3, true, 8, 5), movz(8, 4)}, 0, "add imm + ldr q reg lsl 4");
    check_fault({add_imm(8, 9, 0x40), ldst_reg(3, false, 11, 6, true, 8, 5), movz(8, 4)}, 0, "add imm + str d sxtw");
    check_fault({add_imm(8, 8, 1), add_imm(8, 8, 1), add_imm(8, 8, 1), add_imm(8, 8, 1), add4s(23, 21, 22),
                 add_imm(8, 9, 0x30), ld1r(true, 2, 8, 5), add4s(24, 23, 5), add_imm(8, 3, 1), add_imm(8, 8, 1)},
                5, "pinned producer dest + ld1r");
    check_fault({add_imm(8, 9, 0x10), ldst_multiple(true, false, 0x0, 2, 8, 4), movz(8, 3)}, 0, "add imm + st4");
}

FX_TEST(neon_region_reused_destination_fault_state) {
    using namespace vf;
    // The arithmetic may reuse the old destination's cached host. The load
    // into that same guest vector must preserve the dirty pre-load value for
    // fault recovery, then resume to the same final state as the interpreter.
    // Also run with TRANSMUTE_FX_CORE=novecpins to force the cache path.
    check_fault({add4s(23, 21, 22), add4s(23, 23, 22), add_imm(8, 9, 0x40),
                 ldst_uimm(4, true, 0, 8, 23), movz(8, 7)},
                2, "reused dirty vector before faulting overwrite");
}

// ---- broadcasts from current context slots, TBL without a temp -------------------
// v18/v23/v19 are loaded (clean cached registers, read again soon): their
// by-element and DUP (element) uses broadcast straight from the slots
// (EmitContext::vec_slot); a slot may also be stale (v19 is computed: dirty).
// Single-table TBL writes its adjusted indices into the destination, also
// when the destination is the index register.
FX_TEST(neon_region_slot_broadcasts_and_tbl) {
    run_program({
        0x3dc00212u, // ldr q18, [x16]
        0x3dc00617u, // ldr q23, [x16, #0x10]
        0x4e37d653u, // fadd v19.4s, v18.4s, v23.4s
        0x4f929863u, // fmul v3.4s, v3.4s, v18.s[2]
        0x4f929021u, // fmul v1.4s, v1.4s, v18.s[0]
        0x4fb29042u, // fmul v2.4s, v2.4s, v18.s[1]
        0x4fb718a4u, // fmla v4.4s, v5.4s, v23.s[3]
        0x5fb790e6u, // fmul s6, s7, v23.s[1]
        0x5fd31928u, // fmla d8, d9, v19.d[1]
        0x4fd3996au, // fmul v10.2d, v11.2d, v19.d[1]
        0x4f9789acu, // mul v12.4s, v13.4s, v23.s[2]
        0x6fb209eeu, // mla v14.4s, v15.4s, v18.s[3]
        0x4e0c06f0u, // dup v16.4s, v23.s[1]
        0x4e180671u, // dup v17.2d, v19.d[1]
        0x5fb29ab4u, // fmul s20, s21, v18.s[3]
        0x4e170258u, // tbl v24.16b, {v18.16b}, v23.16b
        0x4e120339u, // tbl v25.16b, {v25.16b}, v18.16b
        0x4e1a02fau, // tbl v26.16b, {v23.16b}, v26.16b
        0x0e12027bu, // tbl v27.8b, {v19.16b}, v18.8b
        0x3d800223u, // str q3, [x17]
    }, "slot_broadcasts_and_tbl", 6);
}

// ---- mask reductions with vector value facts (emit_neon_fusion.cpp) --------------
// AND 1 / USHR #31, USHL {0,1,2,3}, ADDV, FMOV after every vector compare
// form, with the shift and mask vectors from MOVI (exact), a load (checked
// flag), a static constant mismatch, or the incoming context (inline guard),
// against the interpreter: full guest state and memory.

namespace {
namespace mf {

constexpr std::uint32_t adr(unsigned rd, std::int32_t off) {
    const auto u = static_cast<std::uint32_t>(off);
    return 0x10000000u | ((u & 3u) << 29) | (((u >> 2) & 0x7ffffu) << 5) | rd;
}
constexpr std::uint32_t movi4s(unsigned rd, unsigned imm8) {
    return 0x4f000400u | ((imm8 >> 5) << 16) | ((imm8 & 31u) << 5) | rd;
}
constexpr std::uint32_t ldr_q(unsigned rt, unsigned rn, unsigned offset) {
    return 0x3dc00000u | ((offset / 16) << 10) | (rn << 5) | rt;
}
constexpr std::uint32_t ldr_q_post(unsigned rt, unsigned rn, int imm) {
    return 0x3cc00400u | ((static_cast<std::uint32_t>(imm) & 0x1ffu) << 12) | (rn << 5) | rt;
}
constexpr std::uint32_t add_x_imm(unsigned rd, unsigned rn, unsigned imm) { return 0x91000000u | (imm << 10) | (rn << 5) | rd; }
constexpr std::uint32_t and16b(unsigned d, unsigned n, unsigned m) { return 0x4e201c00u | (m << 16) | (n << 5) | d; }
constexpr std::uint32_t ushr31(unsigned d, unsigned n) { return 0x6f210400u | (n << 5) | d; }
constexpr std::uint32_t ushl4s(unsigned d, unsigned n, unsigned m) { return 0x6ea04400u | (m << 16) | (n << 5) | d; }
constexpr std::uint32_t addv4s(unsigned d, unsigned n) { return 0x4eb1b800u | (n << 5) | d; }
constexpr std::uint32_t fmov_ws(unsigned wd, unsigned sn) { return 0x1e260000u | (sn << 5) | wd; }
constexpr std::uint32_t add_lsl4(unsigned rd, unsigned rn, unsigned rm) { return 0x8b001000u | (rm << 16) | (rn << 5) | rd; }
constexpr std::uint32_t cbz_x(unsigned rt, int words) { return 0xb4000000u | ((static_cast<std::uint32_t>(words) & 0x7ffffu) << 5) | rt; }
constexpr std::uint32_t add4s_v(unsigned d, unsigned n, unsigned m) { return 0x4ea08400u | (m << 16) | (n << 5) | d; }
constexpr std::uint32_t kNop = 0xd280000au;   // movz x10, #0 (filler; HINT needs the system family)

struct Compare {
    std::uint32_t base;
    bool zero;   // compare with zero (Vn only)
    const char* name;
};
constexpr Compare kCompares[] = {
    {0x6ea0e400u, false, "fcmgt"}, {0x4e20e400u, false, "fcmeq"}, {0x6e20e400u, false, "fcmge"},
    {0x6e20ec00u, false, "facge"}, {0x6ea0ec00u, false, "facgt"}, {0x4ea03400u, false, "cmgt"},
    {0x4ea03c00u, false, "cmge"},  {0x6ea03400u, false, "cmhi"},  {0x6ea03c00u, false, "cmhs"},
    {0x6ea08c00u, false, "cmeq"},  {0x4ea08c00u, false, "cmtst"}, {0x4ea0c800u, true, "fcmgt0"},
    {0x6ea0c800u, true, "fcmge0"}, {0x4ea0d800u, true, "fcmeq0"}, {0x6ea0d800u, true, "fcmle0"},
    {0x4ea0e800u, true, "fcmlt0"}, {0x4ea08800u, true, "cmgt0"},  {0x6ea08800u, true, "cmge0"},
    {0x4ea09800u, true, "cmeq0"},  {0x6ea09800u, true, "cmle0"},  {0x4ea0a800u, true, "cmlt0"},
    {0x4ee0e800u, true, "fcmlt0.2d"}, {0x6ee08c00u, false, "cmeq.2d"},
};

// Data below the program (DiffRunner's literal pool area), reached with ADR.
constexpr std::uint32_t kPool = 0x200;            // code_base() + kPool
constexpr std::uint32_t kCounts = 0, kMask = 16, kA = 32, kB = 32 + 256, kCountsB = 32 + 512;

void put128(std::uint8_t* p, std::uint32_t l0, std::uint32_t l1, std::uint32_t l2, std::uint32_t l3) {
    const std::uint32_t v[4] = {l0, l1, l2, l3};
    std::memcpy(p, v, 16);
}

// a_k lane i = bit i of k ? 2.0 : -0.5, b_k = 1.0 (every 4-bit mask from the
// ordered compares and the compares with zero), or special values (NaN, infinities, zeros, denormals).
void fill_inputs(std::uint8_t* pool, bool special) {
    static constexpr std::uint32_t kSpecial[] = {
        0x7fc00000u, 0xffc00001u, 0x7f800001u, 0x7f800000u, 0xff800000u, 0x00000000u, 0x80000000u,
        0x00000001u, 0x80000001u, 0x3f800000u, 0xbf800000u, 0x7f7fffffu, 0x00800000u, 0x3f800001u,
    };
    for (unsigned k = 0; k < 16; ++k)
        for (unsigned i = 0; i < 4; ++i) {
            std::uint32_t a, b;
            if (!special) {
                a = ((k >> i) & 1u) ? 0x40000000u : 0xbf000000u;   // 2.0 : -0.5
                b = 0x3f800000u;
            } else {
                a = kSpecial[(k * 7 + i * 3) % 14];
                b = kSpecial[(k * 5 + i * 11 + 3) % 14];
            }
            std::memcpy(pool + kA + 16 * k + 4 * i, &a, 4);
            std::memcpy(pool + kB + 16 * k + 4 * i, &b, 4);
        }
}

enum class Src { movi1, movi3, load, context };

struct Variant {
    bool ushr;          // USHR #31 form
    bool alias;         // the reduction overwrites the compare result (Vd == Vn)
    Src mask, counts;   // counts: movi1 means "load" here (MOVI cannot build {0,1,2,3})
    unsigned compare;
};

std::vector<std::uint32_t> program(const Variant& v, std::int32_t pool_off) {
    const auto& c = kCompares[v.compare];
    const unsigned d = v.alias ? 2u : 4u;
    std::vector<std::uint32_t> w;
    w.push_back(adr(5, pool_off));
    w.push_back(v.mask == Src::movi1 ? movi4s(0, 1) : v.mask == Src::movi3 ? movi4s(0, 3)
                : v.mask == Src::load ? ldr_q(0, 5, kMask) : kNop);
    w.push_back(v.counts == Src::context ? kNop : ldr_q(1, 5, kCounts));
    w.push_back(movz_x(0, 0));
    w.push_back(movz_x(6, 16));
    w.push_back(add_x_imm(7, 5, kA));
    // loop:
    w.push_back(ldr_q(2, 7, kB - kA));
    w.push_back(ldr_q_post(3, 7, 16));
    w.push_back(c.zero ? (c.base | (3u << 5) | 2u) : (c.base | (2u << 16) | (3u << 5) | 2u));
    w.push_back(v.ushr ? ushr31(d, 2) : and16b(d, 2, 0));
    w.push_back(ushl4s(d, d, 1));
    w.push_back(addv4s(d, d));
    w.push_back(fmov_ws(13, d));
    w.push_back(add_lsl4(0, 13, 0));
    w.push_back(subs_x_imm(6, 6, 1));
    w.push_back(b_cond(kNe, -9));
    return w;
}

bool contains(const std::vector<std::uint8_t>& code, std::initializer_list<std::uint8_t> bytes) {
    return std::search(code.begin(), code.end(), bytes.begin(), bytes.end()) != code.end();
}

} // namespace mf
} // namespace

FX_TEST(neon_region_mask_facts_differential) {
    using namespace mf;
    ensure_registered();
    struct RestoreKnobs { ~RestoreKnobs() { neon::neon_set_knobs_for_testing(-1); } } restore;
    neon::neon_set_knobs_for_testing(0);
    DiffRunner runner;
    auto* pool = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(runner.code_base() + kPool));
    const auto pool_off = static_cast<std::int32_t>(kPool) - static_cast<std::int32_t>(DiffRunner::kProgramOffset);
    struct Counts { std::uint32_t l[4]; } const counts[] = {
        {{0, 1, 2, 3}}, {{0x100, 1, 2, 3}}, {{0, 1, 2, 4}}, {{3, 2, 1, 0}}, {{0xff, 0x81, 31, 32}}, {{0xffffffe0u, 7, 0x7f, 0x80}},
    };
    unsigned runs = 0, flag_paths = 0;
    for (unsigned compare = 0; compare < std::size(kCompares); ++compare)
        for (const bool special : {false, true})
            for (unsigned shape = 0; shape < 10; ++shape) {
                Variant v{};
                v.compare = compare;
                v.ushr = shape >= 7;
                v.alias = (shape & 1u) != 0;
                v.mask = shape < 7 ? std::array{Src::movi1, Src::movi1, Src::movi3, Src::load, Src::load, Src::context,
                                                Src::movi1}[shape]
                                   : Src::movi1;
                v.counts = shape == 6 ? Src::context : Src::load;
                const auto words = program(v, pool_off);
                for (unsigned cv = 0; cv < std::size(counts); ++cv) {
                    fill_inputs(pool, special);
                    const auto& cn = counts[cv];
                    put128(pool + kCounts, cn.l[0], cn.l[1], cn.l[2], cn.l[3]);
                    if (shape == 4) put128(pool + kMask, 1, 1, 0x101, 1);   // loaded, not {1,1,1,1}
                    else put128(pool + kMask, 1, 1, 1, 1);
                    auto ctx = runner.context(9000 + compare * 64 + shape * 8 + cv);
                    if (v.counts == Src::context && (cv & 1u) == 0) {
                        ctx.v[1][0] = 0x0000000100000000ull;
                        ctx.v[1][1] = 0x0000000300000002ull;
                    }
                    if (v.mask == Src::context && (cv & 1u) == 0) ctx.v[0][0] = ctx.v[0][1] = 0x0000000100000001ull;
                    const std::string label = std::string("mask facts ") + kCompares[compare].name + (special ? " special" : "") +
                                              " shape " + std::to_string(shape) + " counts " + std::to_string(cv);
                    if (!runner.run(words, ctx, label)) return;
                    if (runner.last.interprets != 0) {
                        testing::report_failure(__FILE__, __LINE__, label + ": interpreted");
                        return;
                    }
                    ++runs;
                    // FCMGT on the ordered inputs produced each of the 16 masks once
                    // (shapes 2 and 4 AND with other masks).
                    if (compare == 0 && !special && cv == 0 && shape != 2 && shape != 4) FX_CHECK_EQ(runner.actual.x[0], 0x0123456789abcdefull);
                    if (!neon::features().avx2) continue;
                    bool flag = false, inline_guard = false;
                    for (const auto& r : runner.machine.regions) {
                        flag |= contains(r.code, {0x41, 0x0f, 0xb7, 0x8f});   // movzx ecx, word [r15 + disp32]
                        inline_guard |= contains(r.code, {0x48, 0x8d, 0x49, 0xf1, 0xe3});   // lea rcx, [rcx - 15]; jrcxz
                    }
                    // Loaded counts with an exact mask (MOVI #1 or USHR) use the checked flag.
                    const bool want_flag = v.counts == Src::load && (v.ushr || v.mask == Src::movi1);
                    FX_CHECK_EQ(flag, want_flag);
                    FX_CHECK_EQ(inline_guard, !want_flag);
                    flag_paths += flag;
                }
            }
    std::printf("    %u runs, %u through checked flags\n", runs, flag_paths);
}

// Definitions on two paths reach the reduction: loads write the same flag,
// whichever ran last decides; an arithmetic redefinition on one path makes
// the value unknown (inline comparison). MOVI #1 on one path and MOVI #2 on
// the other: constant flag stores.
FX_TEST(neon_region_mask_facts_join) {
    using namespace mf;
    ensure_registered();
    DiffRunner runner;
    auto* pool = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(runner.code_base() + kPool));
    const auto pool_off = static_cast<std::int32_t>(kPool) - static_cast<std::int32_t>(DiffRunner::kProgramOffset);
    fill_inputs(pool, false);
    for (unsigned variant = 0; variant < 4; ++variant)
        for (unsigned other = 0; other < 2; ++other)
            for (unsigned take = 0; take < 2; ++take) {
                put128(pool + kCounts, 0, 1, 2, 3);
                if (other) put128(pool + kCountsB, 1, 0, 3, 2);
                else put128(pool + kCountsB, 0, 1, 2, 3);
                std::vector<std::uint32_t> w;
                w.push_back(adr(5, pool_off));                         // 0
                w.push_back(ldr_q(1, 5, kCounts));                      // 1
                w.push_back(movi4s(0, 1));                              // 2
                w.push_back(cbz_x(8, 3));                               // 3: x8 == 0 skips 4..5
                w.push_back(variant == 1 ? add4s_v(1, 1, 1) : ldr_q(1, 5, kCountsB));   // 4
                w.push_back(variant == 2 ? movi4s(0, 2) : kNop);        // 5
                w.push_back(ldr_q(2, 5, kA + 16 * (5 + variant)));      // 6
                w.push_back(ldr_q(3, 5, kB));                            // 7
                w.push_back(0x6ea0e400u | (3u << 16) | (2u << 5) | 2u); // 8: fcmgt v2.4s, v2.4s, v3.4s
                w.push_back(variant == 3 ? ushr31(2, 2) : and16b(2, 2, 0));
                w.push_back(ushl4s(2, 2, 1));
                w.push_back(addv4s(2, 2));
                w.push_back(fmov_ws(9, 2));
                auto ctx = runner.context(4400 + variant * 4 + other * 2 + take);
                ctx.x[8] = take;
                const std::string label = "mask facts join variant " + std::to_string(variant) + " other " +
                                          std::to_string(other) + " take " + std::to_string(take);
                if (!runner.run(w, ctx, label)) return;
                FX_CHECK_EQ(runner.last.interprets, 0u);
            }
}

// A fault right after the fused span: the fault map holds its results (the
// ADDV scalar and Wt may still be dirty in registers) and the checked flag
// state is irrelevant to recovery.
FX_TEST(neon_region_mask_facts_fault_state) {
    using namespace vf;
    using mf::adr;
    auto& r = runner();
    auto* pool = reinterpret_cast<std::uint8_t*>(static_cast<std::uintptr_t>(r.code_base() + mf::kPool));
    mf::put128(pool + mf::kCounts, 0, 1, 2, 3);
    mf::fill_inputs(pool, true);
    const auto pool_off = static_cast<std::int32_t>(mf::kPool) - static_cast<std::int32_t>(coretest::DiffRunner::kProgramOffset);
    check_fault({adr(5, pool_off), mf::movi4s(0, 1), mf::ldr_q(1, 5, mf::kCounts), mf::ldr_q(2, 5, mf::kA + 48),
                 mf::ldr_q(3, 5, mf::kB + 48), 0x6ea0e400u | (3u << 16) | (2u << 5) | 2u, mf::and16b(2, 2, 0),
                 mf::ushl4s(2, 2, 1), mf::addv4s(2, 2), mf::fmov_ws(13, 2), ldst_uimm(4, true, 0, 9, 4), movz(8, 7)},
                10, "mask reduction then faulting ldr q");
    check_fault({adr(5, pool_off), mf::ldr_q(1, 5, mf::kCounts), mf::ldr_q(2, 5, mf::kA + 80),
                 mf::ushr31(4, 2), mf::ushl4s(4, 4, 1), mf::addv4s(4, 4), ldst_uimm(4, true, 0, 9, 5), movz(8, 7)},
                6, "ushr reduction then faulting ldr q");
}

// ---- tier 2: vector copy aliases (register and slot aliases) ----------------------
// Random blocks of MOV Vd.16B, Vn.16B (rotations, copies back, chains) mixed
// with arithmetic, scalar results (upper-lane zeroing), loads and stores (fault
// sites) and forward branches, in optimized (tier-2) and plain translations,
// against the interpreter: full guest state and memory.

namespace {
namespace ta {

constexpr std::uint32_t mov16b(unsigned d, unsigned n) { return 0x4ea01c00u | (n << 16) | (n << 5) | d; }
constexpr std::uint32_t add4s(unsigned d, unsigned n, unsigned m) { return 0x4ea08400u | (m << 16) | (n << 5) | d; }
constexpr std::uint32_t eor16b(unsigned d, unsigned n, unsigned m) { return 0x6e201c00u | (m << 16) | (n << 5) | d; }
// Integer only: FP results would differ from the interpreter in NaN signs
// (default NaNs; docs/FX_ENGINE.md), random inputs make NaNs.
constexpr std::uint32_t sub4s(unsigned d, unsigned n, unsigned m) { return 0x6ea08400u | (m << 16) | (n << 5) | d; }
constexpr std::uint32_t add_d(unsigned d, unsigned n, unsigned m) { return 0x5ee08400u | (m << 16) | (n << 5) | d; }
constexpr std::uint32_t str_q(unsigned rt, unsigned rn, unsigned offset) {
    return 0x3d800000u | ((offset / 16) << 10) | (rn << 5) | rt;
}

std::vector<std::uint32_t> make_program(coretest::Rng& rng) {
    std::vector<std::uint32_t> w;
    std::vector<std::size_t> branches;
    const bool loop = rng.chance(40);
    if (loop) w.push_back(movz_x(0, 3));
    const auto body = w.size();
    if (loop) w.push_back(subs_x_imm(0, 0, 1));   // first: forward branches cannot skip it
    // A small register set makes copies collide; a wide one leaves some unpinned.
    const unsigned span = rng.chance(50) ? 6u : 32u, base = rng.below(32 - std::min(span, 26u));
    const auto reg = [&] { return (base + rng.below(span)) & 31u; };
    const unsigned n = 8 + rng.below(40);
    for (unsigned i = 0; i < n; ++i) {
        const unsigned kind = rng.below(100);
        if (kind < 40) {
            w.push_back(mov16b(reg(), reg()));
        } else if (kind < 46) {   // a rotation and its way back (the noise_fbm pattern)
            const unsigned a = reg(), b = (a + 1) & 31u, c = (a + 2) & 31u, t = reg();
            w.push_back(mov16b(t, c));
            w.push_back(mov16b(c, b));
            w.push_back(mov16b(b, a));
            if (rng.chance(50)) w.push_back(add4s(a, reg(), reg()));
            w.push_back(mov16b(a, b));
            w.push_back(mov16b(b, c));
            w.push_back(mov16b(c, t));
        } else if (kind < 58) {
            w.push_back(add4s(reg(), reg(), reg()));
        } else if (kind < 66) {
            w.push_back(eor16b(reg(), reg(), reg()));
        } else if (kind < 72) {
            w.push_back(sub4s(reg(), reg(), reg()));
        } else if (kind < 78) {
            w.push_back(add_d(reg(), reg(), reg()));   // scalar: zeroes bits 64..127
        } else if (kind < 86) {
            w.push_back(mf::ldr_q(reg(), 16, 16 * rng.below(16)));
        } else if (kind < 93) {
            w.push_back(str_q(reg(), 17, 16 * rng.below(16)));
        } else if (i + 3 < n) {   // forward branch over the next 1..3 words
            branches.push_back(w.size());
            w.push_back(mf::cbz_x(1 + rng.below(6), 2 + static_cast<int>(rng.below(3))));
        }
    }
    if (loop) {
        w.push_back(0xf100001fu);   // cmp x0, #0
        w.push_back(b_cond(kNe, -static_cast<int>(w.size() - body)));
    }
    for (const auto k : branches) {   // land inside the program (or on its BRK)
        const auto room = static_cast<std::uint32_t>(w.size() - k);
        const auto off = (w[k] >> 5) & 0x7ffffu;
        if (off > room) w[k] = (w[k] & ~(0x7ffffu << 5)) | (room << 5);
    }
    return w;
}

std::string hex(const std::vector<std::uint32_t>& words) {
    std::string s;
    char buf[16];
    for (const auto w : words) {
        std::snprintf(buf, sizeof buf, " %08x", w);
        s += buf;
    }
    return s;
}

} // namespace ta
} // namespace

FX_TEST(neon_region_tier2_alias_differential) {
    ensure_registered();
    DiffRunner runner;
    coretest::Rng rng(20261003);
    unsigned failures = 0;
    for (unsigned i = 0; i < 3000 && failures < 6; ++i) {
        const auto words = ta::make_program(rng);
        for (const int optimize : {1, 0}) {
            runner.machine.options.optimize = static_cast<std::int8_t>(optimize);
            auto ctx = runner.context(880000 + i);
            for (unsigned g = 1; g < 7; ++g) ctx.x[g] &= 1;   // CBZ takes both ways
            if (!runner.run(words, ctx, "tier2 aliases" + std::string(optimize ? " opt" : " plain") + ta::hex(words))) {
                ++failures;
                break;
            }
            if (runner.last.interprets != 0) {
                testing::report_failure(__FILE__, __LINE__, "interpreted:" + ta::hex(words));
                ++failures;
                break;
            }
        }
    }
    runner.machine.options.optimize = -1;
}

// The rotation leaves one copy: the optimized loop body has fewer context
// stores and loads than the plain one, and both match the interpreter.
FX_TEST(neon_region_tier2_rotation_traffic) {
    ensure_registered();
    // A loop over a noise_fbm-like rotation of unpinned registers (more than
    // the 10 vector pins are in use), with arithmetic between.
    std::vector<std::uint32_t> w{movz_x(0, 5)};
    for (unsigned rep = 0; rep < 3; ++rep)   // hot: these get the 10 vector pins
        for (unsigned r = 0; r < 16; ++r) w.push_back(ta::add4s(r, r, (r + 1) & 15u));
    w.push_back(ta::mov16b(8 + 16, 31));
    w.push_back(ta::mov16b(31, 30));
    w.push_back(ta::mov16b(30, 29));
    w.push_back(ta::mov16b(29, 28));
    w.push_back(ta::eor16b(28, 2, 13));
    w.push_back(ta::add4s(20, 28, 28));
    w.push_back(ta::mov16b(28, 29));
    w.push_back(ta::mov16b(29, 30));
    w.push_back(ta::mov16b(30, 31));
    w.push_back(ta::mov16b(31, 8 + 16));
    w.push_back(subs_x_imm(0, 0, 1));
    w.push_back(b_cond(kNe, -static_cast<int>(w.size() - 1)));
    DiffRunner runner;
    unsigned traffic[2] = {};
    for (const int optimize : {0, 1}) {
        runner.machine.options.optimize = static_cast<std::int8_t>(optimize);
        FX_REQUIRE(runner.run(w, runner.context(31337), optimize ? "rotation opt" : "rotation plain"));
        FX_CHECK_EQ(runner.last.interprets, 0u);
        // movdqu xmm, [r15 + disp32] / movdqu [r15 + disp32], xmm: F3 41 0F 6F/7F;
        // movups (Plan::cold_layout): 41 0F 10/11 87+8*reg disp32
        // The region bodies (the cold section holds exit and edge stubs).
        for (const auto& r : runner.machine.regions)
            for (std::size_t k = 0; k + 3 < (r.stats.hot_bytes ? r.stats.hot_bytes : r.code.size()); ++k)
                traffic[optimize] += (r.code[k] == 0xf3 && (r.code[k + 1] & 0xf0) == 0x40 && r.code[k + 2] == 0x0f &&
                                      (r.code[k + 3] == 0x6f || r.code[k + 3] == 0x7f)) ||
                                     ((r.code[k] & 0xf1) == 0x41 && r.code[k + 1] == 0x0f &&
                                      (r.code[k + 2] == 0x10 || r.code[k + 2] == 0x11) && (r.code[k + 3] & 0xc7) == 0x87);
    }
    runner.machine.options.optimize = -1;
    std::printf("    context vector loads/stores: plain %u, tier 2 %u\n", traffic[0], traffic[1]);
    FX_CHECK(traffic[1] < traffic[0]);
}

// Aliases at a fault: slot aliases are copied into their slots before the
// faulting instruction, dirty register aliases are in the fault map.
FX_TEST(neon_region_tier2_alias_fault_state) {
    using namespace vf;
    auto& r = runner();
    r.machine.options.optimize = 1;
    check_fault({ta::mov16b(24, 31), ta::mov16b(31, 30), ta::mov16b(30, 24), ta::add4s(3, 30, 31),
                 ta::mov16b(25, 3), ldst_uimm(4, true, 0, 9, 4), movz(8, 7)},
                5, "slot and register aliases at a faulting ldr q");
    check_fault({ta::mov16b(26, 27), ta::mov16b(27, 26), ta::mov16b(28, 26), ldst_uimm(4, true, 0, 9, 26), movz(8, 7)},
                3, "copy back, then a faulting load into an alias");
    r.machine.options.optimize = -1;
}

// Tier-up from a hot loop: counted first-tier code exits at the loop header
// when the per-thread iteration counter runs out, and the eager (optimized)
// translation of the header continues the loop with the same state.
FX_TEST(neon_region_tier2_loop_tier_up) {
    ensure_registered();
    std::vector<std::uint32_t> w{movz_x(0, 300)};
    for (unsigned r = 0; r < 16; ++r) w.push_back(ta::add4s(r, r, (r + 1) & 15u));
    w.push_back(ta::mov16b(24, 31));
    w.push_back(ta::mov16b(31, 30));
    w.push_back(ta::mov16b(30, 24));
    w.push_back(ta::eor16b(20, 30, 31));
    w.push_back(subs_x_imm(0, 0, 1));
    w.push_back(b_cond(kNe, -static_cast<int>(w.size() - 1)));
    DiffRunner runner;
    runner.machine.options.lazy = true;
    runner.machine.options.optimize = 2;   // with loop-iteration counting
    runner.machine.options.tier_threshold = 16;
    auto ctx = runner.context(4242);
    ctx.spill[55] = 0;   // the loop counter slot (region_emitter.cpp kLoopCounterSlot)
    FX_REQUIRE(runner.run(w, ctx, "tier-2 loop tier-up"));
    FX_CHECK_EQ(runner.last.interprets, 0u);
    FX_CHECK(runner.last.tier_ups >= 1u);
    runner.machine.options = TranslateOptions{};
}

// ---- random NEON regions ------------------------------------------------------------
// Random blocks of integer SIMD words from every data-processing group the neon
// family decodes (random fields, a small register set so results feed later
// instructions, pinned and unpinned registers), structure and single-element
// loads/stores, lane moves to and from general registers, loops and forward
// branches, against the interpreter: full guest state and memory, in optimized
// (tier-2) and plain translations. Inputs mix random lanes with saturation edges.
// NEON_RANDOM_PROGRAMS / NEON_RANDOM_SEED scale it, NEON_RANDOM_TRACE prints each
// program, NEON_RANDOM_ONLY=<i> runs program i translated-only first (crashes).

namespace {
namespace rn {

struct Group {
    std::uint32_t mask, value;
};
// emit_neon_dispatch.cpp dispatch_data, in order.
constexpr Group kGroups[] = {
    {0x9f200400u, 0x0e200400u}, {0x9f200c00u, 0x0e200000u}, {0x9f3e0c00u, 0x0e200800u}, {0x9f3e0c00u, 0x0e300800u},
    {0x9fe08400u, 0x0e000400u}, {0xbfe08c00u, 0x0e000000u}, {0xbf208c00u, 0x0e000800u}, {0xbfe08400u, 0x2e000000u},
    {0x9f208400u, 0x0e008400u}, {0x9ff80400u, 0x0f000400u}, {0x9f800400u, 0x0f000400u}, {0x9f000400u, 0x0f000000u},
    {0xdf200400u, 0x5e200400u}, {0xdf200c00u, 0x5e200000u}, {0xdf208400u, 0x5e008400u}, {0xdf3e0c00u, 0x5e200800u},
    {0xdf3e0c00u, 0x5e300800u}, {0xdfe08400u, 0x5e000400u}, {0xdf800400u, 0x5f000400u}, {0xdf000400u, 0x5f000000u},
};

// General registers a generated word may write: not x0 (loop counter), x12/x14
// (register strides), x16/x17 (bases), x30, SP.
constexpr std::uint32_t kGprWritable = (0x0000fffeu & ~((1u << 12) | (1u << 14))) | (0x1fffu << 18);

struct Gen {
    coretest::Rng& rng;
    unsigned base, span;
    unsigned reg() { return (base + rng.below(span)) & 31u; }

    // A data-processing word handled by the neon family (not fp), accepted by the
    // oracle, writing no reserved general register.
    std::uint32_t data() {
        for (;;) {
            const auto& g = kGroups[rng.below(static_cast<unsigned>(std::size(kGroups)))];
            std::uint32_t w = g.value | (static_cast<std::uint32_t>(rng.next()) & ~g.mask);
            w = (w & ~0x3ffu) | (reg() << 5) | reg();
            // Rm (bits 20:16) for the three-register groups and TBL; others keep their random fields.
            if (g.value == 0x0e200400u || g.value == 0x0e200000u || g.value == 0x0e008400u || g.value == 0x5e200400u ||
                g.value == 0x5e200000u || g.value == 0x5e008400u || g.value == 0x0e000800u || g.value == 0x2e000000u ||
                g.value == 0x0e000000u)
                w = (w & ~(31u << 16)) | (reg() << 16);
            InstructionUse use{}, fuse{};
            if (!describe_neon(w, use) || fp_describe(w, fuse)) continue;
            if (use.gpr_write & ~kGprWritable) continue;
            // The oracle's UQRSHRN #1 from 64-bit lanes overflows (NOTES_NEON.md).
            if ((w & 0xafbffc00u) == 0x2f3f9c00u) continue;   // vector and scalar
            GuestContext probe{};
            if (!testing::oracle_step(probe, w)) continue;
            return w;
        }
    }

    // A SIMD&FP load or store from x16/x17: no offset, unsigned or signed
    // immediate offsets, pre/post-index by immediate, post-index by register
    // (x12: 1..4, x14: 0..255) and register offsets.
    std::uint32_t memory() {
        const unsigned rn = 16 + rng.below(2);
        const unsigned stride = rng.chance(50) ? 12u : 14u;
        for (;;) {
            std::uint32_t w;
            switch (rng.below(7)) {
            case 0: {   // LD1-LD4 / ST1-ST4 multiple structures
                static constexpr unsigned kOpc[] = {0x0, 0x2, 0x4, 0x6, 0x7, 0x8, 0xa};
                w = 0x0c000000u | (rng.below(2) << 30) | (rng.below(2) << 22) | (kOpc[rng.below(7)] << 12) |
                    (rng.below(4) << 10) | (rn << 5) | reg();
                if (rng.chance(60)) w |= 0x00800000u | ((rng.chance(50) ? 31u : stride) << 16);   // post-index
                break;
            }
            case 1: {   // single structure (and LDnR)
                w = 0x0d000000u | (rng.below(2) << 30) | (rng.below(2) << 22) | (rng.below(2) << 21) |
                    (rng.below(8) << 13) | (rng.below(2) << 12) | (rng.below(4) << 10) | (rn << 5) | reg();
                if (rng.chance(60)) w |= 0x00800000u | ((rng.chance(50) ? 31u : stride) << 16);
                break;
            }
            case 2:
                w = vf::ldst_uimm(rng.below(5), rng.chance(60), rng.below(32), rn, reg());
                break;
            case 3: {   // LDUR/STUR, or pre/post-index (bits 11:10 = 01 post, 11 pre)
                const int imm9 = static_cast<int>(rng.below(512)) - 256;
                w = vf::ldst_unscaled(rng.below(5), rng.chance(60), imm9, rn, reg());
                const unsigned mode = rng.below(3);
                if (mode) w |= (mode == 1 ? 1u : 3u) << 10;
                break;
            }
            case 4: {   // register offset: LSL #0/#size or SXTW/UXTW of x12/x14
                static constexpr unsigned kOption[] = {2, 3, 6, 7};   // UXTW LSL SXTW SXTX
                w = vf::ldst_reg(rng.below(5), rng.chance(60), stride, kOption[rng.below(4)], rng.chance(50), rn, reg());
                break;
            }
            default: {   // LDP/STP (offset, pre-index, post-index) and LDNP/STNP
                const unsigned rt = reg(), rt2 = reg();
                const bool load = rng.chance(60);
                if (load && rt == rt2) continue;   // CONSTRAINED UNPREDICTABLE
                static constexpr unsigned kMode[] = {0, 1, 2, 3};   // non-temporal, post, offset, pre
                w = vf::ldst_pair(rng.below(3), kMode[rng.below(4)], load, static_cast<int>(rng.below(32)) - 16, rt2, rn, rt);
                break;
            }
            }
            InstructionUse use{};
            if (!describe_vector_memory(w, use)) continue;
            if (use.gpr_write & ~kGprWritable & ~(1u << rn)) continue;
            alignas(16) static std::uint8_t scratch[4096];
            GuestContext probe{};
            probe.x[16] = probe.x[17] = reinterpret_cast<std::uint64_t>(scratch) + 2048;
            probe.x[12] = 4;
            probe.x[14] = 255;
            if (!testing::oracle_step(probe, w)) continue;
            return w;
        }
    }
};

std::vector<std::uint32_t> make_program(coretest::Rng& rng) {
    std::vector<std::uint32_t> w;
    std::vector<std::size_t> branches;
    const bool loop = rng.chance(40);
    if (loop) w.push_back(movz_x(0, 3));
    const auto body = w.size();
    if (loop) w.push_back(subs_x_imm(0, 0, 1));
    Gen g{rng, rng.below(32), rng.chance(60) ? 5u : 12u + rng.below(21)};
    const unsigned n = 6 + rng.below(30);
    for (unsigned i = 0; i < n; ++i) {
        const unsigned kind = rng.below(100);
        if (kind < 72) {
            w.push_back(g.data());
        } else if (kind < 90) {
            w.push_back(g.memory());
        } else if (kind < 94) {
            w.push_back(ta::mov16b(g.reg(), g.reg()));
        } else if (i + 3 < n) {
            branches.push_back(w.size());
            w.push_back(mf::cbz_x(1 + rng.below(6), 2 + static_cast<int>(rng.below(3))));
        }
    }
    if (loop) {
        w.push_back(0xf100001fu);   // cmp x0, #0
        w.push_back(b_cond(kNe, -static_cast<int>(w.size() - body)));
    }
    for (const auto k : branches) {
        const auto room = static_cast<std::uint32_t>(w.size() - k);
        const auto off = (w[k] >> 5) & 0x7ffffu;
        if (off > room) w[k] = (w[k] & ~(0x7ffffu << 5)) | (room << 5);
    }
    return w;
}

void edge_lanes(GuestContext& ctx, coretest::Rng& rng) {
    static constexpr std::uint64_t kEdges[] = {
        0, ~0ull, 0x8000000000000000ull, 0x7fffffffffffffffull, 0x8000000080000000ull, 0x7fffffff7fffffffull,
        0x8000800080008000ull, 0x7fff7fff7fff7fffull, 0x8080808080808080ull, 0x7f7f7f7f7f7f7f7full,
        0x0001000100010001ull, 0xffff0000ffff0000ull, 0x00000000ffffffffull, 0x0f0e0d0c0b0a0908ull,
        0x1f1e1d1c13121110ull, 0x00ff00ff00ff00ffull,
    };
    for (unsigned r = 0; r < 32; ++r)
        for (unsigned h = 0; h < 2; ++h)
            if (rng.chance(30)) ctx.v[r][h] = kEdges[rng.below(static_cast<unsigned>(std::size(kEdges)))];
    for (unsigned r = 1; r < 16; ++r)   // x12 and x14 stay small strides
        if (r != 12 && r != 14 && rng.chance(30)) ctx.x[r] = kEdges[rng.below(static_cast<unsigned>(std::size(kEdges)))];
}

} // namespace rn
} // namespace

FX_TEST(neon_region_random_differential) {
    ensure_registered();
    DiffRunner runner;
    // Random immediates (MOVI, pool constants) can look like image addresses to
    // the relocation audit; the semantics are what this test checks.
    runner.check_relocatable = false;
    const char* env = std::getenv("NEON_RANDOM_PROGRAMS");
    const unsigned programs = env ? static_cast<unsigned>(std::strtoul(env, nullptr, 10)) : 1500u;
    const char* seed_env = std::getenv("NEON_RANDOM_SEED");
    coretest::Rng rng(seed_env ? std::strtoull(seed_env, nullptr, 10) : 20261020u);
    unsigned failures = 0, interpreted = 0;
    for (unsigned i = 0; i < programs && failures < 6; ++i) {
        const auto words = rn::make_program(rng);
        static const bool trace = std::getenv("NEON_RANDOM_TRACE") != nullptr;
        if (trace) {
            std::printf("program %u:%s\n", i, ta::hex(words).c_str());
            std::fflush(stdout);
        }
        auto ctx = runner.context(990000 + i);
        rn::edge_lanes(ctx, rng);
        for (unsigned g = 1; g < 7; ++g) ctx.x[g] &= 1;   // CBZ takes both ways
        if (const char* only = std::getenv("NEON_RANDOM_ONLY")) {   // debugging: one program, translated run first
            if (std::strtoul(only, nullptr, 10) != i) continue;
            runner.machine.options.optimize = 1;
            std::printf("translated: %d\n", runner.run_translated(words, ctx) ? 1 : 0);
            std::fflush(stdout);
        }
        for (const int optimize : {1, 0}) {
            runner.machine.options.optimize = static_cast<std::int8_t>(optimize);
            // Quiet run: DiffRunner compares only general registers and memory without
            // reporting, so the vector registers are compared here.
            const auto fails = [&](const std::vector<std::uint32_t>& program) {
                runner.report = false;
                runner.last = {};
                const bool equal = runner.run(program, ctx, "");
                runner.report = true;
                if (!runner.last.ok || runner.expected.pc != runner.actual.pc) return false;   // oracle or run failed
                return !equal || std::memcmp(runner.expected.v, runner.actual.v, sizeof runner.expected.v) != 0;
            };
            if (fails(words)) {
                // Minimize: replace words with NOPs (branch offsets stay valid) while it still fails.
                auto small = words;
                for (std::size_t k = 0; k < small.size(); ++k) {
                    const auto keep = small[k];
                    if (keep == 0xd503201fu) continue;
                    small[k] = 0xd503201fu;
                    if (!fails(small)) small[k] = keep;
                }
                runner.run(small, ctx, "random neon" + std::string(optimize ? " opt" : " plain") + " (minimized)" +
                                           ta::hex(small) + "; full" + ta::hex(words));
                ++failures;
                break;
            }
            if (!runner.last.ok || runner.expected.pc != runner.actual.pc) {   // report it as it is
                runner.run(words, ctx, "random neon" + std::string(optimize ? " opt" : " plain") + ta::hex(words));
                ++failures;
                break;
            }
            interpreted += runner.last.interprets != 0;
        }
    }
    runner.machine.options.optimize = -1;
    std::printf("    %u programs, %u runs with interpreted words\n", programs, interpreted);
}

// ---- zero-above facts across labels (Plan::vzero_join) ---------------------------------------
// A block starts with the facts all its predecessors agree on: forward
// predecessors from this pass, back edges from tier 2's pass 1. Loop-carried
// scalar FP values then need no upper-lane zeroing in the loop body; a back
// edge that leaves garbage above the scalar (fp_region_loop_resets_upper_lane_knowledge,
// the random programs below) keeps the zeroing.

namespace {
namespace vz {

constexpr std::uint32_t fp3(std::uint32_t base, unsigned d, unsigned n, unsigned m) {
    return base | (m << 16) | (n << 5) | d;
}
constexpr std::uint32_t fp2(std::uint32_t base, unsigned d, unsigned n) { return base | (n << 5) | d; }
constexpr std::uint32_t kFaddS = 0x1e202800u, kFsubS = 0x1e203800u, kFmulS = 0x1e200800u, kFmaxS = 0x1e204800u,
                        kFminS = 0x1e205800u, kFmaxnmS = 0x1e206800u, kFminnmS = 0x1e207800u, kFaddD = 0x1e602800u,
                        kFmulD = 0x1e600800u, kFabsS = 0x1e20c000u, kFnegS = 0x1e214000u, kFmovS = 0x1e204000u,
                        kScvtfSW = 0x1e220000u, kFcvtDS = 0x1e22c000u, kFcvtSD = 0x1e624000u, kFaddV4s = 0x4e20d400u,
                        kFaddV2s = 0x0e20d400u;
constexpr std::uint32_t fmov_imm_s(unsigned d, unsigned imm8) { return 0x1e201000u | (imm8 << 13) | d; }
constexpr std::uint32_t ins_s(unsigned d, unsigned lane, unsigned n) {
    return 0x4e001c00u | (((lane << 3) | 4u) << 16) | (n << 5) | d;
}

// vinsertps xmm, xmm, xmm, 0x0e (VEX.128.66.0F3A 21 /r ib): the scalar upper-lane zeroing.
unsigned count_zeroing(const std::vector<std::uint8_t>& code, std::size_t hot) {
    unsigned n = 0;
    for (std::size_t k = 0; k + 5 < hot && k + 5 < code.size(); ++k)
        n += code[k] == 0xc4 && (code[k + 1] & 0x1f) == 0x03 && code[k + 3] == 0x21 && (code[k + 4] & 0xc0) == 0xc0 &&
             code[k + 5] == 0x0e;
    return n;
}

bool has_nan(const GuestContext& c) {
    for (unsigned r = 0; r < 32; ++r)
        for (unsigned h = 0; h < 2; ++h)
            for (unsigned l = 0; l < 2; ++l) {
                const auto bits = static_cast<std::uint32_t>(c.v[r][h] >> (32 * l));
                if ((bits & 0x7f800000u) == 0x7f800000u) return true;   // NaN or infinity (a double's top half too)
            }
    return false;
}

// Finite floats in every lane and in x1-x3 (INS sources); x4-x6 small integers (SCVTF).
void finite_inputs(GuestContext& ctx, coretest::Rng& rng) {
    const auto f = [&] {
        const float v = (static_cast<float>(rng.below(4000)) - 2000.0f) / 256.0f;
        std::uint32_t b;
        std::memcpy(&b, &v, 4);
        return b;
    };
    for (unsigned r = 0; r < 32; ++r)
        for (unsigned h = 0; h < 2; ++h) ctx.v[r][h] = f() | (std::uint64_t{f()} << 32);
    for (unsigned r = 1; r <= 3; ++r) ctx.x[r] = f();
    for (unsigned r = 4; r <= 6; ++r) ctx.x[r] = rng.below(200);
    ctx.fpcr = 0;
    ctx.fpsr = 0;
}

std::vector<std::uint32_t> fp_program(coretest::Rng& rng) {
    std::vector<std::uint32_t> w;
    const auto reg = [&] { return rng.below(8); };
    const auto op = [&] {
        switch (rng.below(16)) {
        case 0: return fp3(kFaddS, reg(), reg(), reg());
        case 1: return fp3(kFsubS, reg(), reg(), reg());
        case 2: return fp3(kFmulS, reg(), reg(), reg());
        // Min/max in double precision: the oracle's single-precision forms
        // leak source bits 63:32 into the result (NOTES_FP.md).
        case 3: return fp3(kFmaxS | 0x00400000u, reg(), reg(), reg());
        case 4: return fp3(kFminnmS | 0x00400000u, reg(), reg(), reg());
        case 5: return fp3(kFmaxnmS | 0x00400000u, reg(), reg(), reg());
        case 6: return fp3(kFminS | 0x00400000u, reg(), reg(), reg());
        case 7: return fp2(rng.chance(50) ? kFabsS : kFnegS, reg(), reg());
        case 8: return fp2(kFmovS, reg(), reg());
        case 9: return fp2(kScvtfSW, reg(), 4 + rng.below(3));
        case 10: return fp2(rng.chance(50) ? kFcvtDS : kFcvtSD, reg(), reg());
        case 11: return fp3(rng.chance(50) ? kFaddD : kFmulD, reg(), reg(), reg());
        case 12: return ins_s(reg(), rng.below(4), 1 + rng.below(3));   // garbage above the scalar
        case 13: return fp3(rng.chance(50) ? kFaddV4s : kFaddV2s, reg(), reg(), reg());
        case 14: return ta::mov16b(reg(), reg());
        default: return fmov_imm_s(reg(), 0x60u + rng.below(32));
        }
    };
    for (unsigned i = rng.below(4); i > 0; --i) w.push_back(op());   // preheader
    const bool loop = rng.chance(70);
    if (loop) w.push_back(movz_x(0, 3));
    const auto body = w.size();
    if (loop) w.push_back(subs_x_imm(0, 0, 1));
    std::vector<std::size_t> branches;
    const unsigned n = 4 + rng.below(16);
    for (unsigned i = 0; i < n; ++i) {
        if (i + 3 < n && rng.chance(12)) {
            branches.push_back(w.size());
            w.push_back(mf::cbz_x(4 + rng.below(3), 2 + static_cast<int>(rng.below(3))));
        } else {
            w.push_back(op());
        }
    }
    if (loop) {
        w.push_back(0xf100001fu);   // cmp x0, #0
        w.push_back(b_cond(kNe, -static_cast<int>(w.size() - body)));
    }
    for (const auto k : branches) {
        const auto room = static_cast<std::uint32_t>(w.size() - k);
        const auto off = (w[k] >> 5) & 0x7ffffu;
        if (off > room) w[k] = (w[k] & ~(0x7ffffu << 5)) | (room << 5);
    }
    return w;
}

struct OverrideGuard {
    core::PlanOverrides o{};
    explicit OverrideGuard(bool join) {
        o = core::default_plan_overrides();
        o.no_vzero_join = !join;
        core::set_overrides_for_testing(&o);
    }
    ~OverrideGuard() { core::set_overrides_for_testing(nullptr); }
};

} // namespace vz
} // namespace

FX_TEST(fp_region_vzero_join_loop_needs_no_zeroing) {
    using namespace vz;
    ensure_registered();
    if (!fp_features().avx) return;   // the count looks for the VEX zeroing
    // s0 and s1 are known zero above the scalar before the loop (FMOV) and on
    // the back edge (scalar results): the loop body needs no zeroing.
    // s5 is loop-invariant (FMOV before the loop, never written in it): its
    // fact needs the fixpoint over the back edge (vzero_solve).
    const std::vector<std::uint32_t> w{
        movz_x(0, 50), fmov_imm_s(0, 0x70), fmov_imm_s(1, 0x60), fmov_imm_s(5, 0x78),
        fp3(kFaddS, 0, 0, 1),   // loop: fadd s0, s0, s1
        fp3(kFmulS, 2, 0, 1),   //       fmul s2, s0, s1
        fp3(kFsubS, 0, 2, 1),   //       fsub s0, s2, s1
        fp3(kFmaxnmS, 3, 0, 2), //       fmaxnm s3, s0, s2
        fp3(kFsubS, 4, 5, 3),   //       fsub s4, s5, s3 (invariant first source)
        subs_x_imm(0, 0, 1), b_cond(kNe, -6),
    };
    unsigned zeroing[2] = {};
    for (const bool join : {true, false}) {
        OverrideGuard guard(join);
        for (const int optimize : {1, 0}) {
            DiffRunner runner;
            runner.machine.options.optimize = static_cast<std::int8_t>(optimize);
            auto ctx = runner.context(7100 + optimize);
            FX_REQUIRE(runner.run(w, ctx, std::string("vzero join loop") + (join ? " join" : " nojoin") + (optimize ? " opt" : " plain")));
            FX_CHECK_EQ(runner.last.interprets, 0u);
            if (optimize)
                for (const auto& r : runner.machine.regions)
                    zeroing[join ? 0 : 1] += count_zeroing(r.code, r.stats.hot_bytes ? r.stats.hot_bytes : r.code.size());
        }
    }
    std::printf("    upper-lane zeroing in the tier-2 loop: join %u, no join %u\n", zeroing[0], zeroing[1]);
    FX_CHECK(zeroing[0] < zeroing[1]);
    FX_CHECK_EQ(zeroing[0], 0u);
}

FX_TEST(fp_region_vzero_join_random_differential) {
    using namespace vz;
    ensure_registered();
    const char* env = std::getenv("VZERO_RANDOM_PROGRAMS");
    const unsigned programs = env ? static_cast<unsigned>(std::strtoul(env, nullptr, 10)) : 1500u;
    coretest::Rng rng(20261021);
    unsigned failures = 0, nan_skipped = 0;
    DiffRunner runner;
    runner.check_relocatable = false;
    for (unsigned i = 0; i < programs && failures < 6; ++i) {
        const auto words = fp_program(rng);
        auto ctx = runner.context(720000 + i);
        finite_inputs(ctx, rng);
        for (const int optimize : {1, 0}) {
            runner.machine.options.optimize = static_cast<std::int8_t>(optimize);
            runner.report = false;
            runner.last = {};
            const bool equal = runner.run(words, ctx, "");
            runner.report = true;
            const bool ran = runner.last.ok && runner.expected.pc == runner.actual.pc;
            const bool same = ran && equal && std::memcmp(runner.expected.v, runner.actual.v, sizeof runner.expected.v) == 0;
            if (same) continue;
            if (ran && (has_nan(runner.expected) || has_nan(runner.actual))) {   // NaN payloads are host policy
                ++nan_skipped;
                continue;
            }
            runner.run(words, ctx, "vzero join random" + std::string(optimize ? " opt" : " plain") + ta::hex(words));
            ++failures;
            break;
        }
    }
    runner.machine.options.optimize = -1;
    std::printf("    %u programs, %u skipped (NaN or infinity)\n", programs, nan_skipped);
}

// The mask reduction's general path has internal labels: every register it
// writes must be allocated before them. Allocating Vd after the labels could
// evict a dirty cached register there, which is a write-back retry and makes
// the whole region write-through (every vector result stored at once): seen
// in the hottest region of the Cycles render. Pinned idiom sources, dirty
// unpinned registers right before the idiom, inline guard (sources from the
// context): no retry, and the same state as the interpreter.
FX_TEST(neon_region_mask_reduce_no_writeback_retry) {
    using namespace mf;
    ensure_registered();
    DiffRunner runner;
    unsigned variants = 0;
    // form 0: AND Vd, Vn, Vm; 1: AND Vd, Vd, Vm (as in the render); 2: USHR #31.
    // Vd (v28) is cold (only in the context); the mask and counts are pinned or cold.
    for (unsigned dirty = 0; dirty <= 8; ++dirty)
        for (unsigned form = 0; form < 3; ++form)
            for (const bool pinned_sources : {true, false}) {
            const unsigned rd = 28, rn = form == 1 ? 28u : 21u;
            std::vector<std::uint32_t> w{movz_x(0, 4)};
            // loop: the idiom's sources (or not) and more registers are used often (pins)
            const auto body = w.size();
            for (unsigned rep = 0; rep < 3; ++rep) {
                for (unsigned r : {0u, 1u, 2u, 3u, 4u, 5u, 6u}) w.push_back(ta::add4s(r, r, r));
                if (pinned_sources)
                    for (unsigned r : {21u, 22u, 23u}) w.push_back(ta::add4s(9, r, r));   // read, not written
                else
                    for (unsigned r : {7u, 8u, 9u}) w.push_back(ta::add4s(r, r, r));
            }
            for (unsigned k = 0; k < dirty; ++k) w.push_back(ta::add4s(10 + k, 10 + k, 0));   // dirty, unpinned
            w.push_back(form == 2 ? ushr31(rd, rn) : and16b(rd, rn, 22));
            w.push_back(ushl4s(rd, rd, 23));
            w.push_back(addv4s(rd, rd));
            w.push_back(fmov_ws(13, rd));
            for (unsigned k = 0; k < dirty; ++k) w.push_back(ta::add4s(10 + k, 10 + k, 10 + k));   // still live
            w.push_back(subs_x_imm(0, 0, 1));
            w.push_back(b_cond(kNe, -static_cast<int>(w.size() - body)));
            for (const int optimize : {1, 0}) {
                runner.machine.options.optimize = static_cast<std::int8_t>(optimize);
                auto ctx = runner.context(5300 + dirty * 8 + form * 2 + (pinned_sources ? 1 : 0));
                ctx.v[23][0] = 0x0000000100000000ull;   // {0,1,2,3}: the short path is taken at run time
                ctx.v[23][1] = 0x0000000300000002ull;
                const auto before = core::core_counters().retries.load();
                const std::string label = "mask reduce, " + std::to_string(dirty) + " dirty, form " + std::to_string(form) +
                                          (pinned_sources ? " pinned" : " cold") + (optimize ? " opt" : " plain");
                if (!runner.run(w, ctx, label)) return;
                FX_CHECK_EQ(runner.last.interprets, 0u);
                if (core::core_counters().retries.load() != before)
                    testing::report_failure(__FILE__, __LINE__, label + ": the translation retried");
                ++variants;
            }
        }
    runner.machine.options.optimize = -1;
    std::printf("    %u variants\n", variants);
}

// LD3/LD4/ST3/ST4 beside 10 pinned vector registers: the structure lowerings fit
// the 6 XMM registers left (LD4 Q needed 10 before). Running out of XMM
// registers retries the whole region with 4 pins (core_translator.cpp).
FX_TEST(neon_region_structure_access_with_pins_no_retry) {
    ensure_registered();
    struct RestoreKnobs { ~RestoreKnobs() { neon::neon_set_knobs_for_testing(-1); } } restore;
    DiffRunner runner;
    unsigned retried[2] = {};
    for (const int knobs : {0, static_cast<int>(neon::neon_old_struct)}) {
        neon::neon_set_knobs_for_testing(knobs);
        for (unsigned form = 0; form < 8; ++form)
            for (const unsigned size : {0u, 1u, 2u}) {
                // ld4 Q, ld4 D, ld3 Q, st4 Q, st4 D, ld2 Q, st3 Q, st3 D into v16.. (cold), base x16, post-index by the size.
                static constexpr std::uint32_t kForms[] = {0x4cdf0000u, 0x0cdf0000u, 0x4cdf4000u, 0x4c9f0000u, 0x0c9f0000u,
                                                           0x4cdf8000u, 0x4c9f4000u, 0x0c9f4000u};
                std::vector<std::uint32_t> w{movz_x(0, 3)};
                const auto body = w.size();
                for (unsigned rep = 0; rep < 3; ++rep)
                    for (unsigned r = 0; r < 10; ++r) w.push_back(ta::add4s(r, r, (r + 1) % 10));   // pins
                w.push_back(kForms[form] | (size << 10) | (16u << 5) | 16u);
                w.push_back(ta::add4s(0, 16, 17));
                w.push_back(subs_x_imm(0, 0, 1));
                w.push_back(b_cond(kNe, -static_cast<int>(w.size() - body)));
                for (const int optimize : {1, 0}) {
                    runner.machine.options.optimize = static_cast<std::int8_t>(optimize);
                    auto ctx = runner.context(6600 + form * 8 + size);
                    const auto before = core::core_counters().retries.load();
                    if (!runner.run(w, ctx, "structure with pins form " + std::to_string(form) + " size " + std::to_string(size)))
                        return;
                    FX_CHECK_EQ(runner.last.interprets, 0u);
                    const bool again = core::core_counters().retries.load() != before;
                    retried[knobs ? 1 : 0] += again;
                    if (again && !knobs)
                        testing::report_failure(__FILE__, __LINE__, "structure form " + std::to_string(form) + " size " +
                                                                        std::to_string(size) + (optimize ? " opt" : " plain") +
                                                                        ": the translation retried");
                }
            }
    }
    runner.machine.options.optimize = -1;
    std::printf("    translations that retried: %u (old lowering %u)\n", retried[0], retried[1]);
    FX_CHECK_EQ(retried[0], 0u);
}
