// Translator core unit tests: per-class differential tests against the
// reference interpreter, flags, exits, region metadata (owner: translator core).
#include "fx/common/x64_assembler.hpp"
#include "fx/testing/harness.hpp"
#include "fx/testing/test.hpp"
#include "fx/translator/core_decode.hpp"
#include "fx/translator/core_test_encode.hpp"
#include "fx/translator/core_test_gen.hpp"
#include "fx/translator/core_test_machine.hpp"
#include "fx/translator/region_emitter.hpp"
#include "fx/translator/translator.hpp"

#include <windows.h>

#include <bit>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>

using namespace transmute::fx;
using namespace transmute::fx::coretest;

namespace {

DiffRunner& runner() {
    static DiffRunner r;
    return r;
}

std::string hex(std::uint32_t w) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08x", w);
    return buf;
}

// Runs `count` random encodings from `gen`, each from `contexts` random states.
void differential(const char* name, unsigned count, unsigned contexts,
                  const std::function<std::uint32_t(InstGen&)>& gen, std::uint64_t salt) {
    auto& r = runner();
    Rng rng(salt);
    InstGen g(rng);
    int failures = 0;
    for (unsigned i = 0; i < count && failures < 8; ++i) {
        const auto word = gen(g);
        for (unsigned c = 0; c < contexts; ++c) {
            const auto ctx = r.context(salt * 7919 + i * 31 + c);
            if (!r.run({word}, ctx, std::string(name) + " " + hex(word) + " ctx " + std::to_string(c))) {
                ++failures;
                break;
            }
        }
    }
}

} // namespace

FX_TEST(core_decode_bitmask_matches_reference_rules) {
    // A few known logical immediates.
    std::uint64_t m;
    FX_CHECK(core::decode_bit_masks(1, 0, 0, true, m) && m == 1);
    FX_CHECK(core::decode_bit_masks(0, 0b111100, 0, true, m) && m == 0x5555555555555555ULL);
    FX_CHECK(core::decode_bit_masks(1, 0b111110, 0, true, m) && m == 0x7fffffffffffffffULL);
    FX_CHECK(!core::decode_bit_masks(1, 0b111111, 0, true, m));
    FX_CHECK(core::decode_bit_masks(0, 7, 0, false, m) && m == 0xff);
    FX_CHECK(!core::decode_bit_masks(1, 0, 0, false, m));
}

FX_TEST(core_add_sub_immediate) { differential("addsub_imm", 600, 3, [](InstGen& g) { return g.add_sub_imm(); }, 1); }
FX_TEST(core_logic_immediate) { differential("logic_imm", 600, 3, [](InstGen& g) { return g.logic_imm(); }, 2); }
FX_TEST(core_move_wide) { differential("movw", 400, 2, [](InstGen& g) { return g.move_wide(); }, 3); }
FX_TEST(core_adr_adrp) { differential("adr", 200, 1, [](InstGen& g) { return g.adr(); }, 4); }
FX_TEST(core_bitfield) { differential("bitfield", 1500, 3, [](InstGen& g) { return g.bitfield(); }, 5); }
FX_TEST(core_extr) { differential("extr", 400, 3, [](InstGen& g) { return g.extr(); }, 6); }
FX_TEST(core_add_sub_shifted) { differential("addsub_shift", 1000, 3, [](InstGen& g) { return g.add_sub_shift(); }, 7); }
FX_TEST(core_add_sub_extended) { differential("addsub_ext", 1000, 3, [](InstGen& g) { return g.add_sub_ext(); }, 8); }
FX_TEST(core_adc_sbc) { differential("adc", 600, 4, [](InstGen& g) { return g.adc(); }, 9); }
FX_TEST(core_logic_shifted) { differential("logic_shift", 1000, 3, [](InstGen& g) { return g.logic_shift(); }, 10); }
FX_TEST(core_ccmp) { differential("ccmp", 1000, 4, [](InstGen& g) { return g.ccmp(); }, 11); }
FX_TEST(core_csel) { differential("csel", 1000, 4, [](InstGen& g) { return g.csel(); }, 12); }
FX_TEST(core_dp1) { differential("dp1", 600, 3, [](InstGen& g) { return g.dp1(); }, 13); }
FX_TEST(core_dp2) { differential("dp2", 800, 4, [](InstGen& g) { return g.dp2(); }, 14); }
FX_TEST(core_dp3) { differential("dp3", 800, 3, [](InstGen& g) { return g.dp3(); }, 15); }
FX_TEST(core_load_store) { differential("ldst", 2000, 2, [](InstGen& g) { return g.load_store(); }, 16); }
FX_TEST(core_load_store_pair) { differential("ldstp", 1500, 2, [](InstGen& g) { return g.load_store_pair(); }, 17); }
FX_TEST(core_prefetch_is_nop) { differential("prfm", 200, 1, [](InstGen& g) { return g.prefetch(); }, 18); }

FX_TEST(core_load_literal) {
    auto& r = runner();
    Rng rng(19);
    for (unsigned i = 0; i < 300; ++i) {
        static constexpr unsigned opcs[] = {0, 1, 2, 3};
        const auto opc = rng.pick(opcs);
        // Program at +4096; pool at [0, 4096): offsets -1..-1000 words.
        const auto off = -static_cast<std::int32_t>(1 + rng.below(1000));
        const auto word = a64::ldr_literal(opc, off, rng.pick(kDest));
        r.run({word}, r.context(i), "ldr literal " + hex(word));
    }
}

FX_TEST(core_cls_matches_reference) {
    auto& r = runner();
    // CLS has no oracle support: compare with a C++ reference.
    for (unsigned i = 0; i < 400; ++i) {
        const bool sf = i & 1;
        const unsigned rn = 1 + (i % 9), rd = 20 + (i % 7);
        auto ctx = r.context(1000 + i);
        const auto word = a64::dp1(sf, 5, rn, rd);
        auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(r.program_base()));
        program[0] = word;
        program[1] = a64::brk(0xdead);
        const BufferCode code(r.code_base(), r.program_base() + 8);
        auto actual = ctx;
        actual.pc = r.program_base();
        r.machine.reset();
        const auto res = r.machine.run(actual, code, kReturnToHostPc);
        FX_REQUIRE(res.ok && res.reason == ExitReason::brk);
        const auto x = ctx.x[rn];
        unsigned expect = 0;
        if (sf) {
            const auto v = (x ^ (x << 1)) | 1;
            expect = static_cast<unsigned>(std::countl_zero(v));
        } else {
            const auto w = static_cast<std::uint32_t>(x);
            const auto v = (w ^ (w << 1)) | 1u;
            expect = static_cast<unsigned>(std::countl_zero(v));
        }
        FX_CHECK_EQ(actual.x[rd], static_cast<std::uint64_t>(expect));
    }
}

// Flags producers x every condition x consumers (B.cond, CSEL, CSET, CCMP).
FX_TEST(core_flags_all_conditions) {
    auto& r = runner();
    Rng rng(20);
    const std::uint32_t producers[] = {
        a64::add_shift(true, true, true, 0, 2, 0, 1, 31),    // cmp x1, x2
        a64::add_shift(false, true, true, 0, 2, 0, 1, 31),   // cmp w1, w2
        a64::add_shift(true, false, true, 0, 2, 0, 1, 31),   // cmn x1, x2
        a64::add_shift(false, false, true, 0, 2, 0, 1, 3),   // adds w3, w1, w2
        a64::add_imm(true, true, true, 31, 1, 1),            // cmp x1, #1
        a64::add_imm(true, false, true, 31, 1, 1),           // cmn x1, #1
        a64::logic_shift(true, 3, 0, false, 2, 0, 1, 31),    // tst x1, x2
        a64::logic_imm(false, 3, 0, 0, 0, 1, 31),            // tst w1, #1
        a64::adc(true, true, true, 2, 1, 3),                 // sbcs x3, x1, x2
        a64::adc(false, false, true, 2, 1, 3),               // adcs w3, w1, w2
        a64::ccmp(true, true, false, 2, 0, 1, 0b0110),       // ccmp x1, x2, #6, eq
        a64::ccmp(true, false, true, 5, 1, 1, 0b1001),       // ccmn x1, #5, #9, ne
        a64::msr_nzcv(4),                                    // msr nzcv, x4 (interpreted)
    };
    for (unsigned p = 0; p < std::size(producers); ++p) {
        for (unsigned cond = 0; cond < 16; ++cond) {
            for (unsigned k = 0; k < 6; ++k) {
                auto ctx = r.context(p * 1000 + cond * 10 + k);
                static constexpr std::uint64_t specials[] = {0, 1, 2, 0x7fffffffffffffffULL, 0x8000000000000000ULL,
                                                             ~0ULL, 0x7fffffff, 0x80000000, 0xffffffff};
                if (k & 1) ctx.x[1] = specials[rng.below(9)];
                if (k & 2) ctx.x[2] = specials[rng.below(9)];
                if (k == 4) ctx.x[2] = ctx.x[1];
                ctx.x[4] = rng.next();
                const std::vector<std::uint32_t> branch = {
                    producers[p], a64::bcond(cond, 3), a64::movw(true, 2, 0, 1, 0), a64::b(2),
                    a64::movw(true, 2, 0, 2, 0),
                };
                r.run(branch, ctx, "b.cond p" + std::to_string(p) + " c" + std::to_string(cond));
                const std::vector<std::uint32_t> select = {
                    producers[p], a64::csel(true, false, 6, cond, 0, 5, 0),      // csel x0, x5, x6
                    a64::csel(false, false, 31, cond, 1, 31, 7),                 // cset w7
                    a64::csel(true, true, 31, cond, 0, 31, 8),                   // csetm x8
                    a64::csel(true, true, 6, cond, 1, 5, 9),                     // csneg x9
                    a64::ccmp(true, true, false, 6, cond, 5, 0b1010),            // ccmp x5, x6
                };
                r.run(select, ctx, "select p" + std::to_string(p) + " c" + std::to_string(cond));
            }
        }
    }
}

// Flags that stay live across instructions that would clobber EFLAGS and
// across blocks (single-predecessor inheritance, merges, loops).
FX_TEST(core_flags_survive_clobbers) {
    auto& r = runner();
    using namespace a64;
    const std::uint32_t clobbers[] = {
        add_imm(true, false, false, 3, 3, 1),                 // add x3, x3, #1 (lea)
        add_shift(true, false, false, 0, 4, 2, 3, 5),         // add x5, x3, x4, lsl #2
        add_shift(true, true, false, 0, 4, 0, 3, 5),          // sub x5, x3, x4
        logic_shift(true, 0, 0, false, 4, 0, 3, 5),           // and x5, x3, x4
        logic_shift(true, 1, 1, false, 4, 7, 3, 5),           // orr x5, x3, x4, lsr #7
        bitfield(true, 2, 3, 10, 4, 5),                       // ubfx x5, x4, #3, #8
        bitfield(true, 2, 60, 59, 4, 5),                      // lsl x5, x4, #4
        bitfield(true, 1, 8, 7, 4, 5),                        // bfi x5, x4, #56, #8
        extr(true, 4, 9, 3, 5),                               // extr x5, x3, x4, #9
        dp3(true, 0, false, 4, 3, 6, 5),                      // madd x5, x6, x4, x3
        dp3(true, 6, false, 4, 31, 6, 5),                     // umulh x5, x6, x4
        dp2(true, 2, 4, 6, 5),                                // udiv x5, x6, x4
        dp2(true, 8, 4, 6, 5),                                // lsl x5, x6, x4
        dp1(true, 4, 6, 5),                                   // clz x5, x6
        dp1(true, 0, 6, 5),                                   // rbit x5, x6
        movw(true, 3, 2, 0x1234, 5),                          // movk x5, #0x1234, lsl #32
        adc(true, false, false, 4, 3, 5),                     // adc x5, x3, x4
        ldst_imm9(3, 1, 8, 1, 16, 5),                         // ldr x5, [x16], #8
        ldst_pair(2, 3, false, -2, 4, 31, 3),                 // stp x3, x4, [sp, #-16]!
        cbz(true, false, 1, 9),                               // cbz x9, next
        tbz(false, 40, 1, 9),                                 // tbz x9, #40, next
    };
    for (unsigned c = 0; c < std::size(clobbers); ++c) {
        for (unsigned cond = 0; cond < 14; ++cond) {
            for (unsigned k = 0; k < 4; ++k) {
                auto ctx = r.context(50000 + c * 100 + cond * 4 + k);
                if (k == 1) ctx.x[2] = ctx.x[1];
                if (k == 2) ctx.x[1] = 0;
                const std::vector<std::uint32_t> prog = {
                    add_shift(true, true, true, 0, 2, 0, 1, 31),   // cmp x1, x2
                    clobbers[c],
                    bcond(cond, 3),
                    movw(true, 2, 0, 1, 0),
                    b(2),
                    movw(true, 2, 0, 2, 0),
                    csel(true, false, 6, cond, 0, 7, 8),           // csel x8, x7, x6 (flags still live)
                };
                r.run(prog, ctx, "clobber " + std::to_string(c) + " cond " + std::to_string(cond));
            }
        }
    }
}

FX_TEST(core_flags_across_blocks) {
    auto& r = runner();
    using namespace a64;
    for (unsigned cond = 0; cond < 14; ++cond) {
        for (unsigned k = 0; k < 6; ++k) {
            auto ctx = r.context(70000 + cond * 10 + k);
            if (k & 1) ctx.x[2] = ctx.x[1];
            // cmp; b.eq L1; b.lt L2; csel ...; L1: cset; L2 (merge, flags live): csinc
            const std::vector<std::uint32_t> prog = {
                add_shift(true, true, true, 0, 2, 0, 1, 31),   // 0 cmp x1, x2
                bcond(0, 4),                                   // 1 b.eq 5
                bcond(11, 5),                                  // 2 b.lt 7
                csel(true, false, 6, cond, 0, 7, 8),           // 3 csel x8, x7, x6, cond
                b(3),                                          // 4 b 7
                csel(false, false, 31, cond, 1, 31, 9),        // 5 cset w9, !cond
                add_imm(true, false, false, 10, 10, 3),        // 6 add x10, x10, #3
                csel(true, false, 5, cond, 1, 4, 11),          // 7 csinc x11, x4, x5, cond  (merge)
                cbz(true, true, 2, 3),                         // 8 cbnz x3, 10
                bcond(cond, 2),                                // 9 b.cond 11
                add_imm(true, false, false, 13, 13, 1),        // 10
                csel(true, true, 5, cond, 0, 4, 13),           // 11 csinv x13
            };
            r.run(prog, ctx, "blocks cond " + std::to_string(cond));
        }
    }
}

FX_TEST(core_loops_and_counters) {
    auto& r = runner();
    using namespace a64;
    for (unsigned k = 0; k < 40; ++k) {
        auto ctx = r.context(80000 + k);
        ctx.x[12] = 1 + k % 9;
        const std::vector<std::uint32_t> prog = {
            add_shift(true, false, false, 0, 2, 0, 1, 3),    // L: add x3, x1, x2
            ldst_imm9(3, 1, 8, 1, 16, 4),                    // ldr x4, [x16], #8
            add_shift(true, false, false, 0, 4, 0, 3, 5),    // add x5, x3, x4
            ldst_imm9(3, 0, 8, 1, 17, 5),                    // str x5, [x17], #8
            add_shift(true, true, true, 0, 4, 0, 3, 31),     // cmp x3, x4  (flags live across the back edge)
            add_imm(true, true, true, 12, 12, 1),            // subs x12, x12, #1
            bcond(1, -6),                                    // b.ne L
            csel(true, false, 2, 3, 0, 1, 6),                // csel x6, x1, x2, lo  (uses subs flags)
        };
        r.run(prog, ctx, "loop " + std::to_string(k));
        const std::vector<std::uint32_t> prog2 = {
            add_imm(true, false, false, 3, 3, 7),            // L: add x3, x3, #7
            add_shift(true, true, true, 0, 3, 0, 1, 31),     // cmp x1, x3
            add_imm(true, true, false, 12, 12, 1),           // sub x12, x12, #1 (flag-free)
            cbz(true, true, -3, 12),                         // cbnz x12, L  (clobbers EFLAGS: flags live)
            csel(true, false, 2, 8, 0, 1, 6),                // csel x6, x1, x2, hi
        };
        r.run(prog2, ctx, "loop2 " + std::to_string(k));
    }
}

FX_TEST(core_calls_returns_rsb) {
    auto& r = runner();
    using namespace a64;
    for (unsigned k = 0; k < 30; ++k) {
        auto ctx = r.context(90000 + k);
        // main: stp x29,x30; bl f; bl f; cmp; bl f; ldp; b end   f: add; ret
        const std::vector<std::uint32_t> prog = {
            ldst_pair(2, 3, false, -2, 30, 31, 29),          // 0 stp x29, x30, [sp, #-16]!
            bl(8),                                           // 1 bl f (9)
            bl(7),                                           // 2 bl f
            add_shift(true, true, true, 0, 2, 0, 1, 31),     // 3 cmp x1, x2 (flags live into the call)
            bl(5),                                           // 4 bl f
            csel(true, false, 4, 0, 0, 3, 5),                // 5 csel x5, x3, x4, eq (after return)
            ldst_pair(2, 1, true, 2, 30, 31, 29),            // 6 ldp x29, x30, [sp], #16
            b(4),                                            // 7 b end (11)
            nop(),                                           // 8
            add_imm(true, false, false, 3, 3, 5),            // 9 f: add x3, x3, #5
            ret(),                                           // 10 ret
        };
        r.run(prog, ctx, "calls " + std::to_string(k));
        FX_CHECK(r.last.lookups <= 1);
        // BLR / BR / RSB miss (x30 changed before RET).
        const std::vector<std::uint32_t> prog2 = {
            adr(false, 7 * 4, 9),                            // 0 adr x9, f (7)
            blr(9),                                          // 1 blr x9
            adr(false, 4 * 4, 10),                           // 2 adr x10, 6
            br(10),                                          // 3 br x10
            add_imm(true, false, false, 4, 4, 1),            // 4 (skipped)
            add_imm(true, false, false, 4, 4, 2),            // 5 (skipped)
            b(6),                                            // 6 b end (12)
            add_imm(true, false, false, 3, 3, 9),            // 7 f: add x3, x3, #9
            logic_shift(true, 1, 0, false, 30, 0, 31, 11),   // 8 mov x11, x30
            adr(false, 2 * 4, 30),                           // 9 adr x30, 11
            ret(),                                           // 10 ret -> 11 (RSB miss)
            br(11),                                          // 11 br x11 -> 2
        };
        r.run(prog2, ctx, "indirect " + std::to_string(k));
    }
}

FX_TEST(core_interpret_exits_and_entries) {
    auto& r = runner();
    using namespace a64;
    for (unsigned k = 0; k < 30; ++k) {
        auto ctx = r.context(95000 + k);
        ctx.x[12] = 1 + k % 4;
        const std::vector<std::uint32_t> prog = {
            add_shift(true, true, true, 0, 2, 0, 1, 31),     // L: cmp x1, x2
            mrs_nzcv(3),                                     // mrs x3, nzcv  (interpreted)
            neon_add2d(1, 2, 3),                             // add v1.2d, v2.2d, v3.2d (interpreted)
            add_shift(true, false, false, 0, 3, 0, 4, 4),    // add x4, x4, x3
            msr_nzcv(5),                                     // msr nzcv, x5
            csel(true, false, 7, 0, 0, 6, 8),                // csel x8, x6, x7, eq
            add_imm(true, true, true, 12, 12, 1),            // subs x12, x12, #1
            bcond(1, -7),                                    // b.ne L
        };
        r.run(prog, ctx, "interpret " + std::to_string(k));
        FX_CHECK(r.last.interprets >= 3);
    }
}

FX_TEST(core_interrupt_checks) {
    auto& r = runner();
    using namespace a64;
    auto ctx = r.context(97000);
    ctx.x[12] = 50;
    ctx.interrupt_pending = 1;
    const std::vector<std::uint32_t> prog = {
        add_imm(true, false, false, 3, 3, 1),
        add_imm(true, true, true, 12, 12, 1),
        bcond(1, -2),
    };
    r.run(prog, ctx, "interrupt");
    FX_CHECK(r.last.interrupts >= 1);
}

FX_TEST(core_traps) {
    auto& r = runner();
    auto ctx = r.context(98000);
    ctx.pc = r.program_base();
    auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(r.program_base()));
    program[0] = a64::add_shift(true, true, true, 0, 2, 0, 1, 31);
    program[1] = a64::svc(0x42);
    program[2] = a64::brk(0x77);
    const BufferCode code(r.code_base(), r.program_base() + 12);
    r.machine.reset();
    auto res = r.machine.run(ctx, code, kReturnToHostPc);
    FX_CHECK(res.ok && res.reason == ExitReason::svc && res.arg == 0x42);
    FX_CHECK_EQ(ctx.pc, r.program_base() + 8);
    res = r.machine.run(ctx, code, kReturnToHostPc);
    FX_CHECK(res.ok && res.reason == ExitReason::brk && res.arg == 0x77);
    FX_CHECK_EQ(ctx.pc, r.program_base() + 8);
}

FX_TEST(core_region_metadata) {
    auto& r = runner();
    using namespace a64;
    const std::vector<std::uint32_t> prog = {
        adr(true, 3, 0),                                     // 0 adrp x0
        add_imm(true, false, false, 0, 0, 0x123),            // 1 add x0, x0, #0x123 (fused)
        ldst_uimm(3, 1, 1, 16, 2),                           // 2 ldr x2, [x16, #8]
        add_imm(true, true, true, 12, 12, 1),                // 3 subs x12, x12, #1
        bcond(1, -2),                                        // 4 b.ne 2
        bl(100),                                             // 5 bl far (outside the code)
        add_imm(true, false, false, 2, 2, 1),                // 6 continuation entry
        ret(),                                               // 7
    };
    auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(r.program_base()));
    std::memcpy(program, prog.data(), prog.size() * 4);
    const BufferCode code(r.code_base(), r.program_base() + prog.size() * 4);
    const auto region = translate_region(r.program_base(), code);
    FX_CHECK_EQ(region.guest_start, r.program_base());
    FX_REQUIRE(region.entries.size() == 2);
    FX_CHECK_EQ(region.entries[0].guest_pc, r.program_base());
    FX_CHECK_EQ(region.entries[1].guest_pc, r.program_base() + 24);
    FX_CHECK(!region.exits.empty());
    for (const auto& e : region.exits) {
        FX_CHECK_EQ(e.rel32_offset % 4, 0u);
        FX_CHECK(e.rel32_offset >= 1 && region.code[e.rel32_offset - 1] == 0xe9);
    }
    bool saw_bl_target = false;
    for (const auto& e : region.exits) saw_bl_target |= e.target_pc == r.program_base() + 20 + 400;
    FX_CHECK(saw_bl_target);
    for (const auto& f : region.fixups) {
        std::uint64_t v;
        std::memcpy(&v, region.code.data() + f.offset, 8);
        FX_CHECK_EQ(v, f.value);
    }
    bool saw_adrp = false;
    for (const auto& f : region.fixups) saw_adrp |= f.value == ((r.program_base() & ~0xfffULL) + 3 * 4096 + 0x123);
    FX_CHECK(saw_adrp);
    FX_REQUIRE(region.faults.size() == 1);
    FX_CHECK_EQ(region.faults[0].guest_pc, r.program_base() + 8);
    FX_CHECK(!region.pins.empty());
    for (const auto& p : region.pins) {
        // Callee-saved hosts, or r10/r11 (no family instructions, hence no
        // helper calls; the cross-region convention takes the callee-saved ones).
        const bool callee_saved = p.host == 3 || p.host == 5 || p.host == 6 || p.host == 7 ||
                                  (p.host >= 12 && p.host <= 14);
        FX_CHECK(callee_saved || p.host == 10 || p.host == 11);
    }
    FX_CHECK_EQ(region.guest_ranges.size(), 1u);
    FX_CHECK_EQ(region.stats.guest_instructions, 8u);
    // Layout (Plan::cold_layout; TRANSMUTE_FX_CORE=oldlayout restores the
    // previous one): the body is followed by the cold section; every exit's
    // initial rel32 reaches its unlinked tail there, and the BL continuation's
    // entry stub is laid out in the body, after the call's exit jmp.
    FX_CHECK(region.stats.hot_bytes != 0 && region.stats.hot_bytes <= region.code.size());
    if ((core::codegen_knob_bits() & (1u << 30)) == 0) {
        for (const auto& e : region.exits) {
            std::int32_t rel;
            std::memcpy(&rel, region.code.data() + e.rel32_offset, 4);
            const auto tail = static_cast<std::int64_t>(e.rel32_offset) + 4 + rel;
            FX_CHECK(tail >= static_cast<std::int64_t>(region.stats.hot_bytes) &&
                     tail < static_cast<std::int64_t>(region.code.size()));
        }
        if ((core::codegen_layout_bits() & 1u) == 0) FX_CHECK(region.entries[1].offset < region.stats.hot_bytes);
    }
}

// A region covers its whole .pdata function (up to 16384 instructions), not
// just max_instructions of it: regions started inside a cut function used to
// translate most of it again. Small limits (forced splits) and code without
// function bounds are still cut; TRANSMUTE_FX_CORE=oldcap restores the cut.
FX_TEST(core_function_regions_cover_whole_functions) {
    auto& r = runner();
    using namespace a64;
    std::vector<std::uint32_t> prog;
    for (unsigned i = 0; i < 1500; ++i) prog.push_back(add_imm(true, false, false, 3 + i % 8, 3 + i % 8, 1 + i % 5));
    prog.push_back(ret());
    auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(r.program_base()));
    std::memcpy(program, prog.data(), prog.size() * 4);
    BufferCode code(r.code_base(), r.program_base() + prog.size() * 4);
    code.fn_begin = r.program_base();
    code.fn_end = r.program_base() + prog.size() * 4;
    TranslateOptions options;
    options.max_instructions = 1024;
    const bool whole = (core::codegen_knob_bits() & (1u << 31)) == 0;
    FX_CHECK_EQ(translate_region(r.program_base(), code, options).stats.guest_instructions, whole ? 1501u : 1024u);
    options.max_instructions = 512;
    FX_CHECK_EQ(translate_region(r.program_base(), code, options).stats.guest_instructions, 512u);
    options.max_instructions = 1024;
    const BufferCode unbounded(r.code_base(), r.program_base() + prog.size() * 4);
    FX_CHECK_EQ(translate_region(r.program_base(), unbounded, options).stats.guest_instructions, 1024u);
    r.run(prog, r.context(1501), "whole function region");
}

// A fault inside translated code is described by its FaultSite and the pins.
namespace {
struct FaultCapture {
    const TranslatedRegion* region{};
    std::uintptr_t host{};
    std::uint64_t guest_pc{};
    FlagsLocation flags{};
    std::uint64_t pinned_values[32]{};   // guest registers held in host registers at the site
    std::uint32_t in_host{};             // which of pinned_values are valid (pins + fault map)
    bool hit{};
    const void* resume{};
} g_fault;

LONG CALLBACK fault_handler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !g_fault.region) return EXCEPTION_CONTINUE_SEARCH;
    const auto rip = static_cast<std::uintptr_t>(info->ContextRecord->Rip);
    if (rip < g_fault.host || rip >= g_fault.host + g_fault.region->code.size()) return EXCEPTION_CONTINUE_SEARCH;
    const auto off = static_cast<std::uint32_t>(rip - g_fault.host);
    const FaultSite* site = nullptr;
    for (const auto& f : g_fault.region->faults) {
        if (off >= f.host_begin && off < f.host_end) {
            g_fault.hit = true;
            g_fault.guest_pc = f.guest_pc;
            g_fault.flags = f.flags;
            site = &f;
        }
    }
    const DWORD64* regs = &info->ContextRecord->Rax;   // rax..r15 in x64 order
    if (site) {
        for_each_fault_register(g_fault.region->pins, *site, g_fault.region->fault_maps, g_fault.region->fault_map_regs,
                                [&](unsigned guest, unsigned host, bool vector) {
                                    if (vector) return;
                                    g_fault.pinned_values[guest & 31] = regs[host & 15];
                                    g_fault.in_host |= 1u << (guest & 31);
                                });
    }
    info->ContextRecord->Rip = reinterpret_cast<DWORD64>(g_fault.resume);
    return EXCEPTION_CONTINUE_EXECUTION;
}
} // namespace

FX_TEST(core_fault_sites_and_pins) {
    auto& r = runner();
    using namespace a64;
    auto* guard = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS));
    FX_REQUIRE(guard);
    const std::vector<std::uint32_t> prog = {
        add_imm(true, false, false, 19, 19, 5),             // add x19, x19, #5
        add_imm(true, false, false, 19, 19, 5),             // add x19, x19, #5  (pinned, dirty)
        add_shift(true, true, true, 0, 2, 0, 1, 31),        // cmp x1, x2
        ldst_uimm(3, 1, 1, 9, 3),                           // ldr x3, [x9, #8]  -> faults
        csel(true, false, 19, 0, 0, 3, 4),                  // csel x4, x3, x19, eq
    };
    auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(r.program_base()));
    std::memcpy(program, prog.data(), prog.size() * 4);
    program[prog.size()] = brk(0xdead);
    const BufferCode code(r.code_base(), r.program_base() + (prog.size() + 1) * 4);
    r.machine.reset();
    auto ctx = r.context(99000);
    ctx.x[9] = reinterpret_cast<std::uint64_t>(guard);
    const auto initial19 = ctx.x[19];
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
    VirtualFree(guard, 0, MEM_RELEASE);
    FX_REQUIRE(g_fault.hit);
    FX_CHECK_EQ(g_fault.guest_pc, r.program_base() + 12);
    FX_CHECK(g_fault.flags == FlagsLocation::host_sub);
    bool x19_pinned = false;
    for (const auto& p : g_fault.region->pins) x19_pinned |= p.guest == 19;
    FX_CHECK(x19_pinned);
    if (x19_pinned) FX_CHECK_EQ(g_fault.pinned_values[19], initial19 + 10);
}

// Write-back register cache: results of unpinned registers may stay dirty in
// host registers; every fault site finds them in a pin, its fault map or the
// context.
FX_TEST(core_write_back_exact_at_faults) {
    auto& r = runner();
    using namespace a64;
    auto* guard = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS));
    FX_REQUIRE(guard);
    static constexpr unsigned kRegs[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 11, 13, 15, 20, 21, 22};
    for (unsigned variant = 0; variant < 4; ++variant) {
        std::vector<std::uint32_t> prog;
        for (unsigned k = 0; k < std::size(kRegs); ++k) {
            const unsigned g = kRegs[k];
            prog.push_back(add_imm(true, false, false, g, g, 3 + k));        // dirty in the cache
            if (variant & 1) prog.push_back(add_imm(true, false, false, g, g, 1));   // overwritten again
        }
        if (variant & 2) prog.push_back(add_shift(true, true, true, 0, 2, 0, 1, 31));   // cmp: host flags
        prog.push_back(ldst_uimm(3, 1, 1, 9, 23));                           // ldr x23, [x9, #8] -> faults
        prog.push_back(add_imm(true, false, false, 0, 0, 1));
        auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(r.program_base()));
        std::memcpy(program, prog.data(), prog.size() * 4);
        program[prog.size()] = brk(0xdead);
        const BufferCode code(r.code_base(), r.program_base() + (prog.size() + 1) * 4);
        r.machine.reset();
        auto ctx = r.context(99100 + variant);
        ctx.x[9] = reinterpret_cast<std::uint64_t>(guard);
        const auto initial = ctx;
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
        if (core::codegen_knob_bits() == 0) {   // some unpinned results are only in the fault map
            std::uint32_t pinned = 0;
            for (const auto& p : g_fault.region->pins) pinned |= p.vector ? 0u : 1u << p.guest;
            FX_CHECK((g_fault.in_host & ~pinned) != 0);
        }
        for (unsigned k = 0; k < std::size(kRegs); ++k) {
            const unsigned g = kRegs[k];
            const bool in_host = (g_fault.in_host >> g) & 1u;
            const auto value = in_host ? g_fault.pinned_values[g] : ctx.x[g];
            FX_CHECK_EQ(value, initial.x[g] + 3 + k + (variant & 1));
        }
    }
    VirtualFree(guard, 0, MEM_RELEASE);
}

FX_TEST(core_small_regions_linked_and_unlinked) {
    auto& r = runner();
    Rng rng(123);
    InstGen g(rng);
    const auto saved = r.machine.options;
    for (unsigned k = 0; k < 200; ++k) {
        r.machine.options.max_instructions = 1 + k % 5;   // force region splits
        r.machine.link = (k & 1) != 0;
        std::vector<std::uint32_t> prog;
        for (unsigned i = 0; i < 6; ++i) prog.push_back(g.data_or_memory());
        prog.push_back(a64::add_shift(true, true, true, 0, 2, 0, 1, 31));   // cmp x1, x2 (flags cross regions)
        prog.push_back(a64::b(1));
        prog.push_back(a64::csel(true, false, 4, k % 14, 0, 3, 5));
        for (unsigned i = 0; i < 6; ++i) prog.push_back(g.data_or_memory());
        r.run(prog, r.context(k), "small regions " + std::to_string(k));
    }
    r.machine.options = saved;
    r.machine.link = true;
}

FX_TEST(core_function_bounds_limit_regions) {
    auto& r = runner();
    using namespace a64;
    const std::vector<std::uint32_t> prog = {
        add_imm(true, false, false, 3, 3, 1),   // 0  function A
        b(2),                                   // 1  -> 3 (outside A's bounds)
        add_imm(true, false, false, 4, 4, 1),   // 2
        add_imm(true, false, false, 5, 5, 1),   // 3  function B
        ret(),                                  // 4
    };
    auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(r.program_base()));
    std::memcpy(program, prog.data(), prog.size() * 4);
    BufferCode code(r.code_base(), r.program_base() + prog.size() * 4);
    code.fn_begin = r.program_base();
    code.fn_end = r.program_base() + 12;
    const auto region = translate_region(r.program_base(), code);
    FX_CHECK_EQ(region.stats.guest_instructions, 2u);
    FX_REQUIRE(region.exits.size() == 1);
    FX_CHECK_EQ(region.exits[0].target_pc, r.program_base() + 12);
    const auto unbounded = translate_region(r.program_base(), BufferCode(r.code_base(), r.program_base() + prog.size() * 4));
    FX_CHECK_EQ(unbounded.stats.guest_instructions, 4u);
}

FX_TEST(core_neon_hooks_absent_without_neon) {
    // fx_translator_tests does not link fx_emit_neon: the weak defaults are used.
    FX_CHECK(!core::neon_region_hooks_linked());
}

FX_TEST(core_has_entry_stops_regions) {
    auto& r = runner();
    using namespace a64;
    const std::vector<std::uint32_t> prog = {
        add_imm(true, false, false, 3, 3, 1),   // 0
        cbz(true, false, 3, 5),                 // 1 cbz x5, 4  (taken target covered elsewhere)
        add_imm(true, false, false, 6, 6, 1),   // 2
        b(2),                                   // 3 b 5        (covered)
        add_imm(true, false, false, 7, 7, 1),   // 4  covered
        add_imm(true, false, false, 8, 8, 1),   // 5  covered
    };
    auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(r.program_base()));
    std::memcpy(program, prog.data(), prog.size() * 4);
    program[prog.size()] = brk(0xdead);
    BufferCode code(r.code_base(), r.program_base() + (prog.size() + 1) * 4);
    code.covered = {r.program_base() + 16, r.program_base() + 20, r.program_base()};
    const auto region = translate_region(r.program_base(), code);   // the start itself is never cut
    FX_CHECK_EQ(region.stats.guest_instructions, 4u);
    FX_CHECK_EQ(region.exits.size(), 2u);
    // Still runs correctly (the covered code is translated as its own regions).
    r.run(prog, r.context(5), "has_entry");
}

// ---- first-tier (lazy) formation and tier-up -------------------------------------------------

FX_TEST(core_lazy_formation_metadata) {
    auto& r = runner();
    using namespace a64;
    const std::vector<std::uint32_t> prog = {
        add_imm(true, false, false, 3, 3, 1),   // 0
        bl(4),                                  // 1 bl f (5)
        add_imm(true, false, false, 4, 4, 1),   // 2 continuation
        add_imm(true, false, false, 5, 5, 1),   // 3
        ret(),                                  // 4
        add_imm(true, false, false, 6, 6, 1),   // 5 f
        br(30),                                 // 6 (BR X30, not RET: f is never inlined)
    };
    auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(r.program_base()));
    std::memcpy(program, prog.data(), prog.size() * 4);
    BufferCode code(r.code_base(), r.program_base() + prog.size() * 4);
    code.fn_begin = r.program_base();
    code.fn_end = r.program_base() + 20;
    const auto base = r.program_base();
    // Eager (default): the continuation is an entry of the same region; final tier.
    // (Tier 2 pinned off: TRANSMUTE_FX_CORE=tier2 counts every lazy region.)
    TranslateOptions plain;
    plain.optimize = 0;
    const auto eager = translate_region(base, code, plain);
    FX_CHECK_EQ(eager.stats.guest_instructions, 5u);
    FX_CHECK_EQ(eager.tier, 1u);
    FX_CHECK_EQ(eager.tier_counter, 0u);
    FX_CHECK_EQ(eager.entries.size(), 2u);
    // Lazy without splitting and under the budget: identical to eager.
    TranslateOptions lazy;
    lazy.lazy = true;
    lazy.optimize = 0;
    const auto same = translate_region(base, code, lazy);
    FX_CHECK(same.code == eager.code);
    FX_CHECK_EQ(same.tier, 1u);
    {   // Tier 2: a complete lazy region without loops is final; with the
        // prototype's policy (optimize 3) it counts its entries (tier 0).
        TranslateOptions counted = lazy;
        counted.optimize = 1;
        const auto t1 = translate_region(base, code, counted);
        FX_CHECK_EQ(t1.tier, 1u);
        FX_CHECK_EQ(t1.tier_counter, 0u);
        counted.optimize = 3;
        const auto t0 = translate_region(base, code, counted);
        FX_CHECK_EQ(t0.tier, 0u);
        FX_CHECK(t0.tier_counter != 0 && t0.tier_counter + 4 <= t0.code.size());
        FX_CHECK_EQ(t0.stats.guest_instructions, 5u);
    }
    // Splitting at calls: the region ends at BL; the RSB entry is a linkable
    // direct exit to the continuation; tier 0 with an entry counter.
    lazy.lazy_split_calls = true;
    const auto split = translate_region(base, code, lazy);
    FX_CHECK_EQ(split.stats.guest_instructions, 2u);
    FX_CHECK_EQ(split.tier, 0u);
    FX_CHECK(split.tier_counter != 0 && split.tier_counter + 4 <= split.code.size());
    FX_CHECK(split.tier_counter_slot + 4 <= split.code.size());
    std::int32_t rel;
    std::memcpy(&rel, split.code.data() + split.tier_counter, 4);
    FX_CHECK_EQ(static_cast<std::uint32_t>(static_cast<std::int32_t>(split.tier_counter + 4) + rel), split.tier_counter_slot);
    bool exit_to_continuation = false, exit_to_callee = false;
    for (const auto& e : split.exits) {
        exit_to_continuation |= e.target_pc == base + 8;
        exit_to_callee |= e.target_pc == base + 20;
    }
    FX_CHECK(exit_to_continuation);
    FX_CHECK(exit_to_callee);
    FX_CHECK_EQ(split.entries.size(), 1u);
    // A covered continuation is not a truncation (a larger region would stop there too).
    code.covered = {base + 8};
    FX_CHECK_EQ(translate_region(base, code, lazy).tier, 1u);
    code.covered.clear();
    // The first-tier budget truncates: tier 0.
    TranslateOptions budget;
    budget.lazy = true;
    budget.lazy_max_instructions = 2;
    const auto cut = translate_region(base, code, budget);
    FX_CHECK_EQ(cut.stats.guest_instructions, 2u);
    FX_CHECK_EQ(cut.tier, 0u);
    // The cache's relocation audit sees no unrecorded addresses in tier-0 code.
    FX_CHECK(r.check_relocatable);
}

FX_TEST(core_window_bounds_without_function_bounds) {
    auto& r = runner();
    using namespace a64;
    std::vector<std::uint32_t> prog;
    for (unsigned i = 0; i < 40; ++i) prog.push_back(add_imm(true, false, false, 3, 3, 1));
    prog.push_back(ret());
    auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(r.program_base()));
    std::memcpy(program, prog.data(), prog.size() * 4);
    BufferCode code(r.code_base(), r.program_base() + prog.size() * 4);   // no function bounds
    TranslateOptions o;
    o.window_forward = 64;
    o.window_backward = 16;
    const auto windowed = translate_region(r.program_base(), code, o);
    FX_CHECK_EQ(windowed.stats.guest_instructions, 16u);
    FX_REQUIRE(windowed.exits.size() == 1);
    FX_CHECK_EQ(windowed.exits[0].target_pc, r.program_base() + 64);
    FX_CHECK_EQ(translate_region(r.program_base(), code).stats.guest_instructions, 41u);
    // Known bounds win over the window.
    code.fn_begin = r.program_base();
    code.fn_end = r.program_base() + prog.size() * 4;
    FX_CHECK_EQ(translate_region(r.program_base(), code, o).stats.guest_instructions, 41u);
}

// Differential runs with first-tier regions split at every call, a tiny
// budget and a threshold of 2 entries: tier-0 code, its counters, the
// continuation exits and tier-up retranslations must all be exact.
FX_TEST(core_tiered_execution_matches_oracle) {
    auto& r = runner();
    using namespace a64;
    const auto saved = r.machine.options;
    r.machine.options.lazy = true;
    r.machine.options.lazy_split_calls = true;
    r.machine.options.tier_threshold = 2;
    unsigned tier_ups = 0;
    for (unsigned k = 0; k < 40; ++k) {
        r.machine.options.lazy_max_instructions = 3 + k % 6;
        auto ctx = r.context(97000 + k);
        ctx.x[12] = 3 + k % 4;
        // loop: bl f; cmp; subs x12; b.ne loop.   f: add; csel; ret
        const std::vector<std::uint32_t> prog = {
            ldst_pair(2, 3, false, -2, 30, 31, 29),          // 0 stp x29, x30, [sp, #-16]!
            bl(8),                                           // 1 L: bl f (9)
            add_shift(true, true, true, 0, 2, 0, 1, 31),     // 2 cmp x1, x2
            csel(true, false, 4, 0, 0, 3, 5),                // 3 csel x5, x3, x4, eq
            add_imm(true, true, true, 12, 12, 1),            // 4 subs x12, x12, #1
            bcond(1, -4),                                    // 5 b.ne L
            ldst_pair(2, 1, true, 2, 30, 31, 29),            // 6 ldp x29, x30, [sp], #16
            b(5),                                            // 7 b end (12)
            nop(),                                           // 8
            add_imm(true, false, false, 3, 3, 5),            // 9 f: add x3, x3, #5
            add_shift(true, false, false, 0, 3, 0, 6, 6),    // 10 add x6, x6, x3
            ret(),                                           // 11 ret
        };
        r.run(prog, ctx, "tiered " + std::to_string(k));
        tier_ups += r.last.tier_ups;
    }
    FX_CHECK(tier_ups > 0);
    r.machine.options = saved;
}
