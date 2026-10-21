// Hints, barriers, MRS/MSR, counters, cache maintenance and PSTATE flag
// instructions (owner: atomics/system).
#include "fx/translator/emit_system_test_util.hpp"

#include "fx/testing/test.hpp"

#include <windows.h>
#include <intrin.h>

#include <cstring>
#include <string>

using namespace transmute::fx;
using namespace transmute::fx::systest;
using testing::Buffer;

namespace {

// Expect the program to leave every architectural register unchanged.
void check_no_effect(std::uint32_t word, const std::string& label) {
    auto ctx = make_context(17);
    auto before = ctx;
    Program({word}).run(ctx);
    before.pc += 4;
    testing::compare_contexts(before, ctx, label);
}

unsigned emitted_bytes(std::uint32_t word, unsigned* consumed = nullptr) {
    x64::Assembler a;
    testing::MemoryEmitContext e(a, 0x1000);
    const unsigned n = emit_system(e, std::span(&word, 1));
    if (consumed) *consumed = n;
    e.end_instruction();
    return static_cast<unsigned>(a.size());
}

std::uint32_t mrs(std::uint32_t sysreg_base, unsigned rt) { return sysreg_base | rt; }

} // namespace

FX_TEST(system_hint_space_is_architecturally_a_nop) {
    for (unsigned imm = 0; imm < 128; ++imm) {
        const std::uint32_t word = 0xd503201fu | (imm << 5);
        check_no_effect(word, "hint #" + std::to_string(imm));
        unsigned consumed = 0;
        const auto bytes = emitted_bytes(word, &consumed);
        FX_CHECK_EQ(consumed, 1u);
        if (imm >= 1 && imm <= 3) FX_CHECK_EQ(bytes, 2u);   // pause
        else FX_CHECK_EQ(bytes, 0u);
    }
    // NOP/YIELD/PAC hints also agree with the oracle.
    Buffer buffer(4096);
    for (std::uint32_t word : {0xd503201fu, 0xd503203fu, 0xd503233fu, 0xd503237fu, 0xd50323bfu, 0xd50323ffu, 0xd50320ffu}) {
        differential({word}, make_context(3), buffer, "hint oracle");
    }
}

FX_TEST(system_barriers) {
    Buffer buffer(4096);
    for (unsigned crm = 0; crm < 16; ++crm) {
        for (unsigned op2 : {4u, 5u, 6u}) {
            const std::uint32_t word = 0xd503301fu | (crm << 8) | (op2 << 5);
            differential({word}, make_context(crm), buffer, "barrier crm=" + std::to_string(crm));
            const auto bytes = emitted_bytes(word);
            // DMB: only full (xx11) and reserved (xx00) types fence. DSB: every
            // type except SSBB/PSSBB (completion semantics, see NOTES_SYSTEM.md).
            const bool dmb_full = (crm & 3) == 3 || (crm & 3) == 0;
            const bool full = op2 == 5 ? dmb_full : op2 == 4 && crm != 0 && crm != 4;
            FX_CHECK_EQ(bytes != 0, full);
            {
                LegacyOrderingScope legacy(true);   // the mapping before ABI 11
                const bool old_full = op2 != 6 && (dmb_full && !(op2 == 4 && (crm == 0 || crm == 4)));
                FX_CHECK_EQ(emitted_bytes(word) != 0, old_full);
            }
        }
        check_no_effect(0xd50330ffu | (crm << 8), "sb");
    }
    for (unsigned domain = 0; domain < 4; ++domain) {   // DSB <option>nXS
        unsigned consumed = 0;
        emitted_bytes(0xd503323fu | (domain << 10), &consumed);
        FX_CHECK_EQ(consumed, 1u);
    }
    // CLREX clears the reservation.
    auto ctx = make_context(4);
    ctx.excl_size = 8;
    Program({a64::kClrex}).run(ctx);
    FX_CHECK_EQ(ctx.excl_size, 0u);
}

// A full DMB followed (straight line, same span) by only stores and
// register-only instructions up to another full barrier emits nothing.
FX_TEST(system_dmb_elision_before_a_later_barrier) {
    const auto first_bytes = [](std::vector<std::uint32_t> words) {
        x64::Assembler a;
        testing::MemoryEmitContext e(a, 0x1000);
        const unsigned n = emit_system(e, std::span(words.data(), words.size()));
        e.end_instruction();
        FX_CHECK_EQ(n, 1u);
        return static_cast<unsigned>(a.size());
    };
    const std::uint32_t str = a64::str_x(1, 0, 8), ldr = a64::ldr_x(1, 0, 8);
    const std::uint32_t stp = 0xa9000401u;          // stp x1, x1, [x0]
    const std::uint32_t ldp = 0xa9400401u;          // ldp x1, x1, [x0]
    const std::uint32_t str_q = 0x3d800000u;        // str q0, [x0]
    const std::uint32_t st1 = 0x4c007000u;          // st1 {v0.16b}, [x0]
    const std::uint32_t ld1 = 0x4c407000u;          // ld1 {v0.16b}, [x0]
    const std::uint32_t prfm = 0xf9800000u;         // prfm pldl1keep, [x0]
    const std::uint32_t ldaddal = a64::lse(0, 8, true, true, 1, 2, 0);
    const std::uint32_t fadd = 0x1e222820u;         // fadd s0, s1, s2
    const std::uint32_t bl = 0x94000002u, ret = 0xd65f03c0u;
    struct Case { const char* name; std::vector<std::uint32_t> words; bool elided; };
    const Case cases[] = {
        {"dmb; dmb", {a64::kDmbIsh, a64::kDmbIsh}, true},
        {"dmb; str; dmb", {a64::kDmbIsh, str, a64::kDmbIsh}, true},
        {"dmb; add; stp; str q; st1; fadd; dsb", {a64::kDmbSy, a64::add_imm(true, 2, 2, 1), stp, str_q, st1, fadd,
                                                  a64::kDsbIsh}, true},
        {"dmb; stlr; dmb", {a64::kDmbIsh, a64::stlr(8, 1, 0), a64::kDmbIsh}, true},
        {"dmb; nop; dmb sy", {a64::kDmbIsh, a64::kNop, a64::kDmbSy}, true},
        {"dmb; str; dmb ishst (not full)", {a64::kDmbIsh, str, a64::kDmbIshst}, false},
        {"dmb alone", {a64::kDmbIsh}, false},
        {"dmb; str (span ends)", {a64::kDmbIsh, str}, false},
        {"dmb; ldr; dmb", {a64::kDmbIsh, ldr, a64::kDmbIsh}, false},
        {"dmb; ldp; dmb", {a64::kDmbIsh, ldp, a64::kDmbIsh}, false},
        {"dmb; ld1; dmb", {a64::kDmbIsh, ld1, a64::kDmbIsh}, false},
        {"dmb; prfm; dmb", {a64::kDmbIsh, prfm, a64::kDmbIsh}, false},
        {"dmb; ldar; dmb", {a64::kDmbIsh, a64::ldar(8, 1, 0), a64::kDmbIsh}, false},
        {"dmb; ldapr; dmb", {a64::kDmbIsh, a64::ldapr(8, 1, 0), a64::kDmbIsh}, false},
        {"dmb; ldaddal (locked, not relied on)", {a64::kDmbIsh, ldaddal}, false},
        {"dmb; bl; dmb", {a64::kDmbIsh, bl, a64::kDmbIsh}, false},
        {"dmb; ret", {a64::kDmbIsh, ret}, false},
        {"dmb; isb; dmb", {a64::kDmbIsh, a64::kIsb, a64::kDmbIsh}, false},
        {"dmb; dc cvau; dsb", {a64::kDmbIsh, 0xd50b7b20u, a64::kDsbIsh}, false},
        {"dsb; dmb (DSB never elided)", {a64::kDsbIsh, a64::kDmbIsh}, false},
    };
    for (const auto& c : cases) {
        const auto bytes = first_bytes(c.words);
        if ((bytes == 0) != c.elided) testing::report_failure(__FILE__, __LINE__, std::string(c.name) + ": " +
                                                               std::to_string(bytes) + " bytes");
        LegacyOrderingScope legacy(true);
        FX_CHECK(first_bytes(c.words) != 0);   // legacy: every full barrier fences
    }
}

FX_TEST(system_tpidr_registers) {
    auto ctx = make_context(5);
    ctx.tpidr_el0 = 0x1111;
    ctx.tpidrro_el0 = 0x2222;
    const auto x3 = ctx.x[3];
    Program({0xd53bd040u | 4, 0xd51bd040u | 3, 0xd53bd040u | 5, 0xd53bd060u | 6, 0xd53bd040u | 31}).run(ctx);
    FX_CHECK_EQ(ctx.x[4], std::uint64_t{0x1111});
    FX_CHECK_EQ(ctx.tpidr_el0, x3);
    FX_CHECK_EQ(ctx.x[5], x3);
    FX_CHECK_EQ(ctx.x[6], std::uint64_t{0x2222});
    // MSR TPIDR_EL0, XZR
    Program({0xd51bd040u | 31}).run(ctx);
    FX_CHECK_EQ(ctx.tpidr_el0, std::uint64_t{0});
}

FX_TEST(system_nzcv_and_id_registers_match_oracle) {
    Buffer buffer(4096);
    for (std::uint64_t seed = 0; seed < 32; ++seed) {
        auto ctx = make_context(seed);
        differential({mrs(0xd53b4200u, 7)}, ctx, buffer, "mrs nzcv");
        differential({0xd51b4200u | 8}, ctx, buffer, "msr nzcv");
        differential({0xd51b4200u | 31}, ctx, buffer, "msr nzcv, xzr");
        differential({mrs(0xd53b00e0u, 9)}, ctx, buffer, "mrs dczid");
    }
    auto ctx = make_context(1);
    Program({mrs(0xd53b0020u, 1), mrs(0xd53be000u, 2)}).run(ctx);
    FX_CHECK_EQ(ctx.x[1], kSystemCtrEl0);
    FX_CHECK_EQ(ctx.x[2], kSystemCounterFrequency);
}

FX_TEST(system_counter_is_monotonic_and_runs_at_cntfrq) {
    for (bool inline_counter : {true, false}) {
        system_set_inline_counter(inline_counter);
        Program read({mrs(0xd53be040u, 1), mrs(0xd53be020u, 2), mrs(0xd53be0c0u, 3)});
        system_set_inline_counter(true);
        auto ctx = make_context(2);
        read.run(ctx);
        const auto t0 = ctx.x[1];
        FX_CHECK(ctx.x[2] >= t0);
        FX_CHECK(ctx.x[3] >= ctx.x[2]);
        LARGE_INTEGER f, q0, q1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&q0);
        Sleep(60);
        read.run(ctx);
        QueryPerformanceCounter(&q1);
        const double seconds = double(q1.QuadPart - q0.QuadPart) / double(f.QuadPart);
        const double rate = double(ctx.x[1] - t0) / seconds;
        std::printf("    counter (%s): %.4f MHz (nominal %.4f)\n", inline_counter ? "rdtsc" : "qpc", rate / 1e6,
                    kSystemCounterFrequency / 1e6);
        FX_CHECK(rate > kSystemCounterFrequency * 0.95 && rate < kSystemCounterFrequency * 1.05);
    }
}

FX_TEST(system_cycle_counter_reads_host_tsc_and_preserves_state) {
    unsigned consumed = 0;
    emitted_bytes(mrs(0xd53b9d00u, 8), &consumed);
    FX_REQUIRE(consumed == 1);
    for (unsigned reg : {0u, 8u, 19u, 30u, 31u}) {
        Program read({mrs(0xd53b9d00u, reg)});
        auto ctx = make_context(reg + 1);
        auto expected = ctx;
        const auto before = __rdtsc();
        read.run(ctx);
        const auto after = __rdtsc();
        if (reg != 31) {
            FX_CHECK(ctx.x[reg] >= before && ctx.x[reg] <= after);
            expected.x[reg] = ctx.x[reg];
        }
        expected.pc += 4;
        testing::compare_contexts(expected, ctx, "PMCCNTR_EL0 state");
    }
}

FX_TEST(system_rndr) {
    auto ctx = make_context(3);
    unsigned consumed = 0;
    emitted_bytes(mrs(0xd53b2400u, 1), &consumed);
    if (consumed == 0) { std::printf("    rdrand unavailable: RNDR declined\n"); return; }
    ctx.nzcv = 0xf0000000u;
    Program({mrs(0xd53b2400u, 1), mrs(0xd53b2420u, 2)}).run(ctx);
    FX_CHECK_EQ(ctx.nzcv & 0xf0000000u, std::uint64_t{0});
    FX_CHECK(ctx.x[1] != ctx.x[2]);
}

FX_TEST(system_declines_fp_and_privileged_registers) {
    for (std::uint32_t word : {0xd53b4400u, 0xd51b4400u, 0xd53b4420u, 0xd51b4420u,   // FPCR/FPSR (fp family)
                               0xd5380000u, 0xd5380400u, 0xd5384240u, 0xd51bd060u,   // MIDR, ID_AA64PFR0, CurrentEL, MSR TPIDRRO
                               0xd508871fu, 0xd5087e20u}) {                         // TLBI, DC CISW
        unsigned consumed = 1;
        emitted_bytes(word, &consumed);
        FX_CHECK_EQ(consumed, 0u);
        InstructionUse use;
        FX_CHECK(!describe_system(word, use));
    }
}

FX_TEST(system_dc_zva_and_maintenance) {
    Buffer buffer(8192);
    for (std::uint64_t offset : {0ULL, 1ULL, 63ULL, 100ULL, 4095ULL}) {
        fill_random(buffer, offset);
        auto ctx = make_context(offset);
        ctx.x[1] = buffer.address() + 1024 + offset;
        differential({0xd50b7420u | 1}, ctx, buffer, "dc zva +" + std::to_string(offset), 8192);
    }
    for (std::uint32_t word : {0xd50b7a20u, 0xd50b7b20u, 0xd50b7c20u, 0xd50b7d20u, 0xd50b7e20u}) {
        auto ctx = make_context(9);
        ctx.x[2] = buffer.address() + 77;
        auto before = ctx;
        Program({word | 2}).run(ctx);
        before.pc += 4;
        testing::compare_contexts(before, ctx, "dc maintenance");
    }
    invalidate_calls().clear();
    auto ctx = make_context(10);
    ctx.x[5] = 0x0000'0001'4000'1234ULL;
    const auto before = ctx;
    Program({0xd50b7520u | 5, 0xd508751fu, 0xd508711fu}).run(ctx);
    FX_REQUIRE(invalidate_calls().size() == 3);
    FX_CHECK_EQ(invalidate_calls()[0].address, 0x0000'0001'4000'1000ULL);
    FX_CHECK_EQ(invalidate_calls()[0].size, std::uint64_t{0x1000});
    FX_CHECK_EQ(invalidate_calls()[1].address, std::uint64_t{0});
    FX_CHECK_EQ(invalidate_calls()[1].size, ~std::uint64_t{0});
    auto expected = before;
    expected.pc += 12;
    testing::compare_contexts(expected, ctx, "ic", {});
}

FX_TEST(system_flag_manipulation) {
    for (unsigned nzcv = 0; nzcv < 16; ++nzcv) {
        const bool n = nzcv & 8, z = nzcv & 4, c = nzcv & 2, v = nzcv & 1;
        const auto pack = [](bool N, bool Z, bool C, bool V) {
            return (std::uint64_t{N} << 31) | (std::uint64_t{Z} << 30) | (std::uint64_t{C} << 29) | (std::uint64_t{V} << 28);
        };
        struct Case { std::uint32_t word; std::uint64_t expected; const char* name; };
        const Case cases[] = {
            {0xd500401fu, pack(n, z, !c, v), "cfinv"},
            {0xd500403fu, pack(!c && !z, z && c, c || z, !c && z), "xaflag"},
            {0xd500405fu, pack(false, z || v, c && !v, false), "axflag"},
        };
        for (const auto& k : cases) {
            auto ctx = make_context(nzcv);
            ctx.nzcv = pack(n, z, c, v);
            Program({k.word}).run(ctx);
            FX_CHECK_EQ(ctx.nzcv & 0xf0000000u, k.expected);
            if ((ctx.nzcv & 0xf0000000u) != k.expected) std::printf("    %s nzcv=%x\n", k.name, nzcv);
        }
    }
}

FX_TEST(system_describe) {
    InstructionUse use;
    FX_REQUIRE(describe_system(0xd50b7420u | 3, use));
    FX_CHECK_EQ(use.gpr_read, 1u << 3);
    FX_CHECK(use.may_fault);
    FX_REQUIRE(describe_system(mrs(0xd53b4200u, 4), use));
    FX_CHECK(use.reads_flags);
    FX_CHECK_EQ(use.gpr_write, 1u << 4);
    FX_REQUIRE(describe_system(0xd51b4200u | 5, use));
    FX_CHECK(use.writes_flags);
    FX_REQUIRE(describe_system(a64::kDmbIsh, use));
    FX_CHECK_EQ(use.gpr_read | use.gpr_write, 0u);
    FX_REQUIRE(describe_system(0xd500401fu, use));
    FX_CHECK(use.reads_flags && use.writes_flags);
}
