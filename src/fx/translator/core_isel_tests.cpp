// Instruction-selection tests (owner: translator core): address folding
// (ADD/SUB + LDR/STR/LDP/STP), zero/sign-extension facts, MOVK constants and
// cached three-operand sources (core_isel.hpp), differential against the
// reference interpreter, plus exact guest state at faults of folded accesses.
#include "fx/testing/harness.hpp"
#include "fx/testing/test.hpp"
#include "fx/translator/core_isel.hpp"
#include "fx/translator/core_test_encode.hpp"
#include "fx/translator/core_test_gen.hpp"
#include "fx/translator/core_test_machine.hpp"
#include "fx/translator/translator.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace transmute::fx;
using namespace transmute::fx::coretest;

namespace {

DiffRunner& runner() {
    static DiffRunner r;
    return r;
}

std::string hex_words(const std::vector<std::uint32_t>& words) {
    std::string s;
    char buf[16];
    for (const auto w : words) {
        std::snprintf(buf, sizeof buf, " %08x", w);
        s += buf;
    }
    return s;
}

constexpr unsigned kTemp[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 15, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29};

// Index register setups: x13 with known (or deliberately unknown) extension.
enum class Index { ctx14, movz, movn, sxtb, garbage_upper, ldrb };

struct FoldProgram {
    std::vector<std::uint32_t> words;
    unsigned add_at{};      // word index of the producer
    bool expect_fold{};     // knob on: the producer and the access fold
};

// Producer Xd = f(base, index) + adjacent access [Xd, #off], then Xd is
// rewritten by the load itself, overwritten later (dead) or read (live).
FoldProgram make_fold_program(Rng& rng) {
    using namespace a64;
    FoldProgram p;
    auto& w = p.words;
    const unsigned xd = rng.pick(kTemp);
    static constexpr unsigned kBases[] = {16, 17, 31};
    unsigned base = rng.pick(kBases);
    const unsigned producer = rng.below(4);   // 0 add imm, 1 sub imm, 2 add lsl, 3 add ext
    if (producer == 2 && base == 31) base = 16;   // Rn 31 is XZR for shifted ADD
    // Index setup (into x13 unless ctx14).
    auto mode = static_cast<Index>(rng.below(6));
    unsigned index = 13;
    switch (mode) {
    case Index::ctx14: index = 14; break;
    case Index::movz: w.push_back(movw(rng.chance(50), 2, 0, rng.below(256), 13)); break;
    case Index::movn: w.push_back(movw(false, 0, 0, rng.below(128), 13)); break;            // w13 = ~k: negative as SXTW
    case Index::sxtb: w.push_back(bitfield(true, 0, 0, 7, 14, 13)); break;                  // sxtb x13, w14
    case Index::garbage_upper: w.push_back(logic_imm(true, 1, 1, 32, 31, 14, 13)); break;   // orr x13, x14, #0xffffffff00000000
    case Index::ldrb: w.push_back(ldst_uimm(0, 1, rng.below(64), 17, 13)); break;           // ldrb w13, [x17, #o]
    }
    if (rng.chance(30)) w.push_back(add_imm(true, false, false, 2, 3, 1));   // unrelated filler
    // Extension options valid for the index value (the address must stay in the data buffer).
    unsigned option = 3;
    switch (mode) {
    case Index::ctx14:
    case Index::movz:
    case Index::ldrb: { static constexpr unsigned o[] = {2, 3, 6, 7}; option = rng.pick(o); break; }
    case Index::movn: option = 6; break;
    case Index::sxtb: { static constexpr unsigned o[] = {3, 6, 7}; option = rng.pick(o); break; }
    case Index::garbage_upper: { static constexpr unsigned o[] = {2, 6}; option = rng.pick(o); break; }
    }
    const unsigned amount = rng.below(4);
    p.add_at = static_cast<unsigned>(w.size());
    switch (producer) {
    case 0: w.push_back(add_imm(true, false, false, xd, base, rng.below(512))); break;
    case 1: w.push_back(add_imm(true, true, false, xd, base, rng.below(512))); break;
    case 2:
        // LSL needs a 64-bit index value that is small: x14 or a non-negative x13.
        if (mode == Index::movn || mode == Index::garbage_upper || mode == Index::sxtb) index = 14;
        w.push_back(add_shift(true, false, false, 0, index, amount, base, xd));
        break;
    default: w.push_back(add_ext(true, false, false, index, option, amount, base, xd)); break;
    }
    // The access through Xd.
    const unsigned shape = rng.below(3);   // 0 self (load into Xd), 1 dead later, 2 live
    const bool pair = rng.chance(30);
    bool load = shape == 0 || rng.chance(50);
    unsigned rt = shape == 0 ? xd : rng.pick(kTemp);
    if (rt == xd && shape != 0) rt = xd == 0 ? 1 : 0;
    if (!pair) {
        unsigned size = rng.below(4), opc = load ? 1 : 0;
        if (load && size < 3 && rng.chance(40)) opc = size == 2 ? 2 : 2 + rng.below(2);   // LDRSB/H/W
        if (load && size == 2 && opc == 3) opc = 2;
        w.push_back(ldst_uimm(size, opc, rng.below(32), xd, rt));
    } else {
        unsigned opc = rng.chance(50) ? 2u : 0u;
        if (load && rng.chance(20)) opc = 1;   // LDPSW
        unsigned rt2 = rng.pick(kTemp);
        if (shape == 0 && rng.chance(50)) std::swap(rt, rt2);   // Xd as the second destination
        if (rt2 == rt) rt2 = rt == 1 ? 2 : 1;
        if (shape != 0 && rt2 == xd) rt2 = xd == 3 ? 4 : 3;
        w.push_back(ldst_pair(opc, 2, load, static_cast<std::int32_t>(rng.below(16)) - 4, rt2, xd, rt));
    }
    const bool store_uses_xd = !load && (rt == xd);
    p.expect_fold = shape == 0 && load;
    if (shape == 1) {
        // Non-faulting fillers that do not read Xd, then the overwrite.
        const unsigned fillers = rng.below(3);
        const unsigned other = xd == 9 ? 10 : 9;   // a source that is not Xd
        bool faulting_filler = false;
        for (unsigned f = 0; f < fillers; ++f) {
            if (rng.chance(25)) {
                w.push_back(ldst_uimm(3, 1, rng.below(8), 16, xd == 5 ? 6 : 5));   // may fault: Xd stays live
                faulting_filler = true;
            } else {
                w.push_back(add_imm(true, false, false, xd == 7 ? 8 : 7, other, 3));
            }
        }
        if (rng.chance(50)) w.push_back(movw(true, 2, 0, 0x77, xd));
        else w.push_back(add_imm(true, false, false, xd, other, 5));
        p.expect_fold = !faulting_filler && !store_uses_xd;
    } else if (shape == 2) {
        w.push_back(add_imm(true, false, false, 0, xd, 1));   // reads Xd
        p.expect_fold = false;
    }
    if (rng.chance(30)) w.push_back(add_shift(true, false, false, 0, xd, 0, 2, 3));   // add x3, x2, xd
    return p;
}

} // namespace

// Random fold-shaped programs: translated == reference, and the expected
// pairs fold (the folded access's fault site reports the producer's pc).
FX_TEST(isel_address_folding_differential) {
    auto& r = runner();
    Rng rng(4242);
    unsigned folded = 0, expected = 0, failures = 0;
    for (unsigned i = 0; i < 3000 && failures < 8; ++i) {
        const auto p = make_fold_program(rng);
        const auto ctx = r.context(777000 + i);
        if (!r.run(p.words, ctx, "fold" + hex_words(p.words))) {
            ++failures;
            continue;
        }
        const auto add_pc = r.program_base() + 4ull * p.add_at;
        bool fold = false;
        for (const auto& region : r.machine.regions)
            for (const auto& f : region.faults) fold |= f.guest_pc == add_pc;
        expected += p.expect_fold;
        folded += fold;
        if (core::isel_on(core::isel_no_fold) && p.expect_fold && !fold) {
            testing::report_failure(__FILE__, __LINE__, "expected a fold:" + hex_words(p.words));
            ++failures;
        }
        if (!core::isel_on(core::isel_no_fold) && fold) {
            testing::report_failure(__FILE__, __LINE__, "fold with TRANSMUTE_FX_CORE=nofold:" + hex_words(p.words));
            ++failures;
        }
    }
    std::printf("    folded %u of %u programs (%u expected)\n", folded, 3000u, expected);
    if (core::isel_on(core::isel_no_fold)) FX_CHECK(folded >= expected && expected > 500);
}

// Known extensions: UXTW/SXTW register offsets, extended ADD/SUB and
// SMADDL/UMADDL operands after producers with known and unknown upper bits.
FX_TEST(isel_extension_facts_differential) {
    using namespace a64;
    auto& r = runner();
    Rng rng(4343);
    unsigned failures = 0;
    for (unsigned i = 0; i < 2500 && failures < 8; ++i) {
        std::vector<std::uint32_t> w;
        const unsigned src = rng.pick(kTemp);
        const unsigned x = rng.below(64);
        // A producer of Xsrc with known (or unknown) extension.
        switch (rng.below(14)) {
        case 0: w.push_back(movw(rng.chance(50), rng.chance(50) ? 2 : 0, rng.below(2), rng.below(65536), src)); break;
        case 1: w.push_back(add_imm(false, rng.chance(50), false, src, 9, rng.below(4096))); break;   // W add
        case 2: w.push_back(ldst_uimm(2, 1, x, 17, src)); break;          // ldr w
        case 3: w.push_back(ldst_uimm(2, 2, x, 17, src)); break;          // ldrsw
        case 4: w.push_back(ldst_uimm(1, 3, x, 17, src)); break;          // ldrsh w
        case 5: w.push_back(ldst_uimm(0, 2, x, 17, src)); break;          // ldrsb x
        case 6: w.push_back(bitfield(true, 0, 0, 31, 9, src)); break;     // sxtw
        case 7: w.push_back(bitfield(true, 2, 0, 15, 9, src)); break;     // ubfx #0, #16
        case 8: w.push_back(bitfield(true, 2, 40, 63, 9, src)); break;    // lsr #40
        case 9: w.push_back(bitfield(true, 0, 33, 63, 9, src)); break;    // asr #33
        case 10: w.push_back(logic_imm(true, 0, 1, 0, 30, 9, src)); break;   // and x, x9, #0x7fffffff
        case 11: w.push_back(logic_imm(true, 1, 1, 32, 31, 9, src)); break;  // orr: upper garbage
        case 12: w.push_back(ldst_pair(1, 2, true, 0, src == 1 ? 2 : 1, 17, src)); break;   // ldpsw
        default: w.push_back(add_shift(true, false, false, 0, 10, 0, 9, src)); break;      // 64-bit add: unknown
        }
        if (rng.chance(30)) w.push_back(add_imm(true, false, false, src == 2 ? 3 : 2, 4, 1));   // filler
        const unsigned dst = rng.pick(kTemp);
        switch (rng.below(5)) {
        case 0: {   // register-offset access with UXTW/SXTW of Xsrc (masked first: small index)
            w.push_back(logic_imm(false, 0, 0, 0, 7, src, 13));   // and w13, wsrc, #0xff (known zext)
            static constexpr unsigned o[] = {2, 6};
            w.push_back(ldst_reg(3, 1, rng.chance(50) ? 13u : 14u, rng.pick(o), rng.chance(50), 16, dst));
            break;
        }
        case 1:   // ADD/SUB extended (UXTW/SXTW #0..4)
            w.push_back(add_ext(true, rng.chance(50), rng.chance(30), src, rng.chance(50) ? 2 : 6, rng.below(5), 9, dst));
            break;
        case 2:   // SMADDL / UMADDL / SMSUBL / UMSUBL
            w.push_back(dp3(true, rng.chance(50) ? 1 : 5, rng.chance(30), rng.chance(50) ? src : 10u, 11,
                            rng.chance(50) ? src : 9u, dst));
            break;
        case 3:   // with live flags (cmp before, b.cond-free consumer: csel after)
            w.push_back(add_shift(true, true, true, 0, 2, 0, 1, 31));   // cmp x1, x2
            w.push_back(dp3(true, rng.chance(50) ? 1 : 5, false, src, 11, 9, dst));
            w.push_back(csel(true, false, 3, rng.below(14), 0, 4, 5));
            break;
        default:   // extended register offset through a folded ADD
            w.push_back(logic_imm(false, 0, 0, 0, 7, src, 13));
            w.push_back(add_ext(true, false, false, 13, rng.chance(50) ? 2 : 6, rng.below(4), 16, 8));
            w.push_back(ldst_uimm(3, 1, rng.below(16), 8, 8));
            break;
        }
        if (!r.run(w, r.context(888000 + i), "ext" + hex_words(w))) ++failures;
    }
}

// MOVK of a register with a known constant; cached sources of LEA/MOVSX/RORX
// forms (bitfield, extract, add/sub immediate, CSINC) right after a write.
FX_TEST(isel_constants_and_cached_sources_differential) {
    using namespace a64;
    auto& r = runner();
    Rng rng(4444);
    InstGen g(rng);
    unsigned failures = 0;
    for (unsigned i = 0; i < 3000 && failures < 8; ++i) {
        std::vector<std::uint32_t> w;
        const unsigned t = rng.pick(kTemp);
        const bool sf = rng.chance(50);
        if (rng.chance(50)) {
            w.push_back(movw(sf, rng.chance(70) ? 2 : 0, rng.below(sf ? 4 : 2), rng.below(65536), t));
            for (unsigned k = rng.below(3); k > 0; --k) w.push_back(g.data_or_memory());
            if (rng.chance(30)) w.push_back(movw(sf, 3, rng.below(sf ? 4 : 2), rng.below(65536), t));
            w.push_back(movw(sf, 3, rng.below(sf ? 4 : 2), rng.below(65536), t));
            w.push_back(add_shift(true, false, false, 0, t, 0, 1, 2));   // add x2, x1, xt
        } else {
            w.push_back(add_imm(true, false, false, t, 9, rng.below(64)));   // cached, dirty
            const unsigned d = rng.pick(kTemp);
            switch (rng.below(6)) {
            case 0: w.push_back(add_imm(rng.chance(50), rng.chance(50), false, d, t, rng.below(4096))); break;
            case 1: w.push_back(g.bitfield() & ~(31u << 5) | (t << 5)); break;
            case 2: w.push_back(extr(sf, t, rng.below(sf ? 64 : 32), t, d)); break;
            case 3: w.push_back(csel(sf, false, t, rng.below(14), 1, rng.chance(50) ? t : 10u, d)); break;
            case 4: w.push_back(add_shift(sf, false, false, 0, t, rng.below(4), rng.chance(50) ? t : 10u, d)); break;
            default: w.push_back(add_shift(sf, rng.chance(50), false, rng.below(3), t, rng.below(sf ? 64 : 32), 10, d)); break;
            }
            if (rng.chance(50)) w.push_back(add_shift(true, true, true, 0, 2, 0, d, 31));   // cmp xd, x2: flags live across
        }
        if (!r.run(w, r.context(999000 + i), "src" + hex_words(w))) ++failures;
    }
}

// ---- exact guest state at faults of folded accesses ----------------------------

namespace {
struct FoldFault {
    const TranslatedRegion* region{};
    std::uintptr_t host{};
    bool hit{};
    std::uint64_t guest_pc{};
    FlagsLocation flags{};
    std::uint64_t values[32]{};
    std::uint32_t in_host{};
    const void* resume{};
} g_ff;

LONG CALLBACK fold_fault_handler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !g_ff.region) return EXCEPTION_CONTINUE_SEARCH;
    const auto rip = static_cast<std::uintptr_t>(info->ContextRecord->Rip);
    if (rip < g_ff.host || rip >= g_ff.host + g_ff.region->code.size()) return EXCEPTION_CONTINUE_SEARCH;
    const auto off = static_cast<std::uint32_t>(rip - g_ff.host);
    const DWORD64* regs = &info->ContextRecord->Rax;
    for (const auto& f : g_ff.region->faults) {
        if (off < f.host_begin || off >= f.host_end) continue;
        g_ff.hit = true;
        g_ff.guest_pc = f.guest_pc;
        g_ff.flags = f.flags;
        for_each_fault_register(g_ff.region->pins, f, g_ff.region->fault_maps, g_ff.region->fault_map_regs,
                                [&](unsigned guest, unsigned host, bool vector) {
                                    if (vector) return;
                                    g_ff.values[guest & 31] = regs[host & 15];
                                    g_ff.in_host |= 1u << (guest & 31);
                                });
    }
    info->ContextRecord->Rip = reinterpret_cast<DWORD64>(g_ff.resume);
    return EXCEPTION_CONTINUE_EXECUTION;
}

// Runs `prog` with x9 pointing at a PAGE_NOACCESS page: the access faults.
// Checks the reported pc and that the reconstructed guest state, resumed at
// that pc with the page made readable, finishes exactly like the reference
// interpreter run from the start.
void check_fold_fault(const std::vector<std::uint32_t>& prog, unsigned fault_word, const char* label,
                      bool folded = true, unsigned x9_offset = 0, unsigned protect_from = 0) {
    auto& r = runner();
    auto* page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    FX_REQUIRE(page);
    for (unsigned k = 0; k < 8192; ++k) page[k] = static_cast<std::uint8_t>(k * 37 + 11);
    DWORD old = 0;
    VirtualProtect(page + protect_from, 8192 - protect_from, PAGE_NOACCESS, &old);
    auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(r.program_base()));
    std::memcpy(program, prog.data(), prog.size() * 4);
    program[prog.size()] = a64::brk(0xdead);
    const BufferCode code(r.code_base(), r.program_base() + (prog.size() + 1) * 4);
    auto ctx = r.context(55000 + fault_word);
    ctx.x[9] = reinterpret_cast<std::uint64_t>(page) + x9_offset;
    ctx.x[10] = 3;
    ctx.x[11] = 0xfffffffeu;   // SXTW: -2
    const auto initial = ctx;

    r.machine.reset();
    r.machine.prepare(ctx);
    const auto* host = r.machine.entry_for(r.program_base(), code);
    FX_REQUIRE(host != nullptr);
    g_ff = {};
    g_ff.region = &r.machine.regions.back();
    g_ff.host = reinterpret_cast<std::uintptr_t>(host) - r.machine.regions.back().entries[0].offset;
    g_ff.resume = ctx.stub_dispatch_exit;
    auto* handler = AddVectoredExceptionHandler(1, fold_fault_handler);
    ctx.pc = r.program_base();
    r.machine.enter(ctx, host);
    RemoveVectoredExceptionHandler(handler);
    FX_REQUIRE(g_ff.hit);
    const bool fold = folded && core::isel_on(core::isel_no_fold);
    const auto want_pc = r.program_base() + 4ull * (fold ? fault_word : fault_word + 1);
    FX_CHECK_EQ(g_ff.guest_pc, want_pc);

    // Guest state at the fault: context, then host registers (pins, fault map).
    auto state = ctx;
    for (unsigned g = 0; g < 32; ++g)
        if ((g_ff.in_host >> g) & 1u) state.x[g] = g_ff.values[g];
    if (g_ff.flags == FlagsLocation::context) state.nzcv = ctx.nzcv;
    else FX_CHECK(false);   // these programs keep NZCV in the context at the access
    state.pc = g_ff.guest_pc;
    // Every register except the folded producer's destination equals the
    // reference state before the faulting guest instruction.
    auto before = initial;
    before.pc = r.program_base();
    while (before.pc != g_ff.guest_pc) {
        const auto word = *reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(before.pc));
        FX_REQUIRE(testing::oracle_step(before, word));
    }
    for (unsigned g = 0; g < 32; ++g) FX_CHECK_EQ(state.x[g], before.x[g]);

    // Resume at the reported pc with the page readable: same result as the
    // reference from the start.
    VirtualProtect(page + protect_from, 8192 - protect_from, PAGE_READWRITE, &old);
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
} // namespace

FX_TEST(isel_folded_access_faults_are_exact) {
    using namespace a64;
    // x9 = inaccessible page, x10 = 3, x11 = -2 (as W).
    struct Case {
        const char* label;
        std::vector<std::uint32_t> prog;
        unsigned fault_word;   // index of the producer (folded) / the access is the next word
    };
    const std::vector<Case> cases = {
        {"add lsl + ldr self",
         {add_imm(true, false, false, 19, 19, 5), add_shift(true, false, false, 0, 10, 3, 9, 8), ldst_uimm(3, 1, 2, 8, 8),
          add_imm(true, false, false, 0, 8, 1)},
         1},
        {"add imm + ldrb, dead later",
         {add_imm(true, false, false, 20, 20, 7), add_imm(true, false, false, 8, 9, 0x40), ldst_uimm(0, 1, 3, 8, 0),
          add_imm(true, false, false, 2, 3, 1), movw(true, 2, 0, 9, 8)},
         1},
        {"sub imm + str",
         {add_imm(true, true, false, 8, 9, 0x10), ldst_uimm(3, 0, 6, 8, 1), movw(true, 2, 0, 5, 8)},
         0},
        {"add sxtw + ldr w self",
         {add_ext(true, false, false, 11, 6, 2, 9, 8), ldst_uimm(2, 1, 4, 8, 8)},
         0},
        {"add uxtw + ldp self",
         {movw(false, 2, 0, 3, 12), add_ext(true, false, false, 12, 2, 3, 9, 8), ldst_pair(2, 2, true, 2, 8, 8, 1)},
         1},
        {"add + stp, dead",
         {add_shift(true, false, false, 0, 10, 0, 9, 8), ldst_pair(2, 2, false, 2, 3, 8, 2), add_imm(true, false, false, 8, 3, 1)},
         0},
        {"add lsl + ldrsw, pinned producer dest",
         {add_imm(true, false, false, 21, 21, 1), add_imm(true, false, false, 21, 21, 1), add_imm(true, false, false, 21, 21, 1),
          add_shift(true, false, false, 0, 10, 2, 9, 21), ldst_uimm(2, 2, 1, 21, 21), add_imm(true, false, false, 21, 21, 1),
          add_imm(true, false, false, 21, 21, 1)},
         3},
    };
    for (const auto& c : cases) check_fold_fault(c.prog, c.fault_word, c.label);
    // LDP with a pinned first and an unpinned second destination loads the
    // second word first: the second word faults (next page), the first does
    // not; no destination may change. fault_word + 1 = the LDP.
    std::vector<std::uint32_t> pinned_first;
    for (unsigned k = 0; k < 6; ++k) pinned_first.push_back(add_imm(true, false, false, 21, 21, 1));   // x21 hot: pinned
    pinned_first.push_back(ldst_pair(2, 2, true, 0, 1, 9, 21));                                       // ldp x21, x1, [x9]
    pinned_first.push_back(add_imm(true, false, false, 21, 21, 1));
    pinned_first.push_back(add_imm(true, false, false, 2, 1, 1));
    check_fold_fault(pinned_first, 5, "ldp pinned first, second faults", false, 4096 - 8, 4096);
    std::vector<std::uint32_t> pinned_first_w = pinned_first;
    pinned_first_w[6] = ldst_pair(0, 2, true, 0, 1, 9, 21);                                           // ldp w21, w1, [x9]
    check_fold_fault(pinned_first_w, 5, "ldp w pinned first, second faults", false, 4096 - 4, 4096);
    std::vector<std::uint32_t> pinned_first_b = pinned_first;
    check_fold_fault(pinned_first_b, 5, "ldp pinned first, first faults", false, 4096, 4096);
}

// ---- flags at call and return exits ---------------------------------------------

namespace {
bool contains(const TranslatedRegion& region, const std::vector<std::uint8_t>& bytes) {
    return std::search(region.code.begin(), region.code.end(), bytes.begin(), bytes.end()) != region.code.end();
}
std::vector<std::uint8_t> with_disp32(std::vector<std::uint8_t> bytes, std::int32_t disp, std::vector<std::uint8_t> tail = {}) {
    for (unsigned k = 0; k < 4; ++k) bytes.push_back(static_cast<std::uint8_t>(static_cast<std::uint32_t>(disp) >> (8 * k)));
    bytes.insert(bytes.end(), tail.begin(), tail.end());
    return bytes;
}
constexpr std::int32_t kNzcv = ctx_disp(offsetof(GuestContext, nzcv));
// imul eax, eax, 0x10210000: the EFLAGS -> NZCV conversion.
const std::vector<std::uint8_t> kImul = {0x69, 0xc0, 0x00, 0x00, 0x21, 0x10};
// mov word ptr [r15 + nzcv], ax: an exit's raw flags.
const std::vector<std::uint8_t> kRawStore = with_disp32({0x66, 0x41, 0x89, 0x87}, kNzcv);
// cmp byte ptr [r15 + nzcv + 1], 0: an entry's raw-form check.
const std::vector<std::uint8_t> kRawCheck = with_disp32({0x41, 0x80, 0xbf}, kNzcv + 1, {0x00});

// A flags materialization, as the ARM layout or an exit's raw form.
bool materializes_flags(const TranslatedRegion& region) { return contains(region, kImul) || contains(region, kRawStore); }

TranslatedRegion translate_words(const std::vector<std::uint32_t>& words, bool abi_flags) {
    auto& r = runner();
    auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(r.program_base()));
    std::memcpy(program, words.data(), words.size() * 4);
    const BufferCode code(r.code_base(), r.program_base() + words.size() * 4);
    TranslateOptions options;
    options.abi_flags = abi_flags;
    return translate_region(r.program_base(), code, options);
}
} // namespace

// TranslateOptions::abi_flags: NZCV is dead at BL/BLR and RET, so flags a
// compare left in EFLAGS are not converted into the context there; strict
// flags (and BR, which keeps them live) still convert them.
FX_TEST(core_abi_flags_not_materialized_at_calls_and_returns) {
    using namespace a64;
    const auto cmp = add_shift(true, true, true, 0, 2, 0, 1, 31);   // cmp x1, x2
    struct Case { const char* label; std::vector<std::uint32_t> words; bool live_with_abi; };
    const Case cases[] = {
        {"cmp; bl", {cmp, bl(64)}, false},
        {"cmp; blr", {cmp, blr(9)}, false},
        {"cmp; ret", {cmp, ret()}, false},
        {"cmp; br", {cmp, br(9)}, true},
        {"cmp; b.eq; bl", {cmp, bcond(0, 2), bl(64), ret()}, false},
    };
    core::set_p2_knobs_for_testing(0);   // BR keeps NZCV live by default (brdeadflags: core_br_flags_dead_knob)
    for (const auto& c : cases) {
        if (!materializes_flags(translate_words(c.words, false)))
            testing::report_failure(__FILE__, __LINE__, std::string(c.label) + ": strict flags not materialized");
        if (materializes_flags(translate_words(c.words, true)) != c.live_with_abi)
            testing::report_failure(__FILE__, __LINE__, std::string(c.label) + ": abi flags");
    }
    // TRANSMUTE_FX_CORE=abiflags: the same model without TranslateOptions::abi_flags.
    core::set_p2_knobs_for_testing(core::knob_abi_flags);
    for (const auto& c : cases)
        if (materializes_flags(translate_words(c.words, false)) != c.live_with_abi)
            testing::report_failure(__FILE__, __LINE__, std::string(c.label) + ": abiflags knob");
    core::set_p2_knobs_for_testing(~0u);
}

// ---- raw flags at region exits ------------------------------------------------------

// A direct exit with NZCV live stores the raw lahf/seto form (no conversion;
// TRANSMUTE_FX_CORE=norawflags converts), and an entry whose code reads NZCV
// first converts a raw form (cmp byte [nzcv + 1], 0); flags that only pass
// through the region to an exit stay raw. Behaviour: regions cut
// after one to three instructions pass the flags of a CMP through raw exits
// to the B.cond / CSEL reading them.
FX_TEST(core_raw_flags_at_exits) {
    using namespace a64;
    const auto cmp = add_shift(true, true, true, 0, 2, 0, 1, 31);   // cmp x1, x2
    const std::vector<std::uint32_t> exit_words = {cmp, b(64)};        // b out of the code: a direct exit
    const std::vector<std::uint32_t> reader_words = {bcond(0, 2), nop(), nop()};
    for (const unsigned knob : {0u, static_cast<unsigned>(core::knob_no_raw_flags)}) {
        core::set_p2_knobs_for_testing(knob);
        const auto exit = translate_words(exit_words, false);
        FX_CHECK_EQ(contains(exit, kRawStore), knob == 0);
        FX_CHECK_EQ(contains(exit, kImul), knob != 0);
        FX_CHECK_EQ(contains(translate_words(reader_words, false), kRawCheck), knob == 0);
        FX_CHECK(!contains(translate_words({cmp, bcond(0, 2), nop(), nop()}, false), kRawCheck));   // writes first
        // Flags that only pass through to the exit (a leaf like eor; ret): no check.
        FX_CHECK(!contains(translate_words({logic_shift(true, 2, 0, false, 1, 1, 0, 0), ret()}, false), kRawCheck));
    }
    auto& r = runner();
    const auto saved = r.machine.options;
    const std::vector<std::uint32_t> prog = {cmp,
                                             add_imm(true, false, false, 3, 10, 1),
                                             csel(true, false, 6, 0, 0, 5, 7),   // csel x7, x5, x6, eq
                                             bcond(11, 2),                        // b.lt
                                             add_imm(true, false, false, 8, 10, 2),
                                             adc(true, false, false, 4, 3, 9)};   // adc x9, x3, x4
    for (const unsigned knob : {0u, static_cast<unsigned>(core::knob_no_raw_flags)}) {
        core::set_p2_knobs_for_testing(knob);
        for (unsigned len = 1; len <= 3; ++len) {
            r.machine.options.max_instructions = len;
            for (unsigned k = 0; k < 4; ++k) r.run(prog, r.context(7300 + k), "raw flags " + std::to_string(len));
        }
    }
    r.machine.options = saved;
    core::set_p2_knobs_for_testing(~0u);
}

// ---- dirty cache across branches into single-predecessor blocks ------------------

// x1 and x2 are written (dirty in the register cache) before a CBZ/TBNZ whose
// successors each have it as their only predecessor; both successors fault.
// The values cross the branch unstored (dirty_crosses_branch), so the fault
// map must describe them, and resuming at the reported pc must finish like
// the reference. TRANSMUTE_FX_CORE=nobranchdirty stores them first.
FX_TEST(core_dirty_cache_crosses_branches_exact_at_faults) {
    using namespace a64;
    for (const bool taken : {false, true}) {
        for (const bool test_bit : {false, true}) {
            const std::vector<std::uint32_t> prog = {
                add_imm(true, false, false, 1, 1, 5),             // 0 add x1, x1, #5
                add_imm(true, false, false, 2, 1, 7),             // 1 add x2, x1, #7
                movw(true, 2, 0, taken ? 0 : 1, 3),               // 2 movz x3, #(0|1)
                test_bit ? tbz(false, 0, 3, 3) : cbz(true, false, 3, 3),   // 3 tbz x3, #0 / cbz x3 -> 6
                ldst_uimm(3, 1, 0, 9, 4),                         // 4 ldr x4, [x9] (faults)
                b(3),                                             // 5 b 8
                ldst_uimm(3, 1, 1, 9, 5),                         // 6 ldr x5, [x9, #8] (faults)
                add_imm(true, false, false, 6, 5, 1),             // 7 add x6, x5, #1
                add_imm(true, false, false, 7, 2, 3),             // 8 add x7, x2, #3
            };
            const std::string label = std::string(test_bit ? "tbz" : "cbz") + (taken ? " taken" : " not taken");
            check_fold_fault(prog, taken ? 5 : 3, label.c_str(), false);
            if (core::codegen_on(core::knob_no_branch_dirty))
                FX_CHECK((g_ff.in_host & 6u) == 6u);   // x1, x2 still in registers at the fault
        }
    }
}

// TRANSMUTE_FX_CORE=brdeadflags (opt-in): a BR to a register target (tail
// call, veneer, interpreter dispatch) is flag-dead under abi_flags; a BR
// whose block builds its target from an ADR/ADRP (a switch table, a computed
// goto) keeps NZCV live, and strict flags keep every BR live.
FX_TEST(core_br_flags_dead_knob) {
    using namespace a64;
    const auto cmp = add_shift(true, true, true, 0, 2, 0, 1, 31);   // cmp x1, x2
    const std::vector<std::uint32_t> reg_target = {cmp, br(9)};
    const std::vector<std::uint32_t> computed_goto = {
        adr(false, 16, 10),                     // adr x10, table
        ldst_reg(3, 1, 11, 3, true, 10, 9),     // ldr x9, [x10, x11, lsl #3]
        cmp,
        br(9),
    };
    const std::vector<std::uint32_t> switch_table = {
        add_imm(false, true, true, 31, 14, 3),   // cmp w14, #3
        bcond(8, 6),                            // b.hi default
        adr(false, 20, 9),                      // adr x9, table
        ldst_reg(0, 1, 14, 2, false, 9, 8),     // ldrb w8, [x9, w14, uxtw]
        add_shift(true, false, false, 0, 8, 2, 9, 8),   // add x8, x9, x8, lsl #2
        br(8),
        ret(),                                  // default
        0x00000000u,                            // table
    };
    struct Case { const char* label; const std::vector<std::uint32_t>* words; bool dead_with_knob; };
    const Case cases[] = {
        {"register target", &reg_target, true},
        {"computed goto", &computed_goto, false},
        {"switch table", &switch_table, false},
    };
    for (const auto knob : {0u, static_cast<std::uint32_t>(core::knob_br_flags_dead)}) {
        core::set_p2_knobs_for_testing(knob);
        for (const auto& c : cases) {
            const bool dead = knob != 0 && c.dead_with_knob;
            if (!materializes_flags(translate_words(*c.words, false)))
                testing::report_failure(__FILE__, __LINE__, std::string(c.label) + ": strict flags not materialized");
            if (materializes_flags(translate_words(*c.words, true)) == dead)
                testing::report_failure(__FILE__, __LINE__, std::string(c.label) + (knob ? ": knob on" : ": knob off"));
        }
    }
    core::set_p2_knobs_for_testing(~0u);
}

// ---- pins written before they are observed ---------------------------------------

// x5 and x6 are hot in the loop (pinned) and written by MOVZ before anything
// reads them or may fault: the region entry does not load them
// (pins_dead_at_entry; TRANSMUTE_FX_CORE=nodeadpinloads loads every pin). The
// result matches the reference either way; without the loads the code is smaller.
FX_TEST(core_dead_pin_loads_skipped_at_entries) {
    using namespace a64;
    auto& r = runner();
    const std::vector<std::uint32_t> prog = {
        movw(true, 2, 0, 7, 5),                     // 0 movz x5, #7
        movw(true, 2, 0, 100, 6),                   // 1 movz x6, #100
        add_shift(true, false, false, 0, 5, 1, 5, 5),   // 2 L: add x5, x5, x5, lsl #1
        add_imm(true, false, false, 5, 5, 3),       // 3 add x5, x5, #3
        add_imm(true, true, true, 6, 6, 1),         // 4 subs x6, x6, #1
        bcond(1, -3),                               // 5 b.ne L
        add_imm(true, false, false, 7, 5, 1),       // 6 add x7, x5, #1
    };
    std::size_t sizes[2]{};
    for (const unsigned knob : {0u, static_cast<unsigned>(core::knob_no_dead_pin_loads)}) {
        core::set_p2_knobs_for_testing(knob);
        sizes[knob ? 1 : 0] = translate_words(prog, true).code.size();
        for (unsigned k = 0; k < 4; ++k) r.run(prog, r.context(77000 + k), "dead pin loads " + std::to_string(knob));
    }
    core::set_p2_knobs_for_testing(~0u);
    FX_CHECK(sizes[0] < sizes[1]);
}

// ---- flags kept in EFLAGS around a clobbering instruction --------------------------

// A flags producer, one or two instructions that must clobber EFLAGS but not
// read or write NZCV (EOR/ORR/AND without S, shifts, bitfields, REV, ...),
// then a reader (CSEL family, CCMP, ADC, B.cond): save_flags_around keeps
// the flags in host form across the clobber (lahf/seto ... add al/sahf).
// Differential against the reference, NZCV included, with and without it.
FX_TEST(core_flags_saved_around_clobbers) {
    using namespace a64;
    auto& r = runner();
    Rng rng(0x5a7ef1a9);
    InstGen g(rng);
    for (const unsigned knob : {0u, static_cast<unsigned>(core::knob_no_flag_save)}) {
        core::set_p2_knobs_for_testing(knob);
        int failures = 0;
        for (unsigned i = 0; i < 400 && failures < 6; ++i) {
            std::vector<std::uint32_t> w;
            switch (rng.below(3)) {   // producer
            case 0: w.push_back(add_shift(rng.chance(60), rng.chance(50), true, rng.below(3), g.src(), rng.below(8), g.src(), g.dest())); break;
            case 1: w.push_back(add_imm(rng.chance(60), rng.chance(50), true, g.dest(), g.src_sp(), rng.below(4096))); break;
            default: w.push_back(logic_shift(rng.chance(60), 3, rng.below(4), false, g.src(), rng.below(8), g.src(), g.dest())); break;
            }
            for (unsigned k = 0, n = 1 + rng.below(2); k < n; ++k) {
                switch (rng.below(5)) {   // clobbers without touching NZCV
                case 0: w.push_back(logic_shift(rng.chance(60), rng.below(3), rng.below(4), rng.chance(30), g.src(), rng.below(16), g.src(), g.dest())); break;
                case 1: { auto x = g.logic_imm(); if (((x >> 29) & 3) == 3) x &= ~(1u << 29); w.push_back(x); break; }   // not ANDS
                case 2: w.push_back(g.bitfield()); break;
                case 3: w.push_back(g.dp1()); break;
                default: w.push_back(g.dp2()); break;
                }
            }
            switch (rng.below(4)) {   // reader
            case 0: w.push_back(g.csel()); break;
            case 1: w.push_back(g.ccmp()); break;
            case 2: w.push_back(g.adc()); break;
            default:
                w.push_back(bcond(rng.below(14), 2));
                w.push_back(add_imm(true, false, false, 3, 3, 1));
                break;
            }
            const auto ctx = r.context(0x4242 + i);
            if (!r.run(w, ctx, "flags saved " + std::to_string(knob) + hex_words(w))) ++failures;
        }
    }
    // subs; eor; csel; ret: the flags stay in EFLAGS (no conversion to ctx->nzcv) unless noflagsave.
    const std::vector<std::uint32_t> fixed = {
        add_shift(true, true, true, 1, 10, 1, 15, 15),        // subs x15, x15, x10, lsr #1
        logic_shift(true, 2, 0, false, 11, 0, 16, 17),        // eor x17, x16, x11
        csel(true, false, 16, 6, 0, 17, 17),                  // csel x17, x17, x16, vs
        ret(),
    };
    core::set_p2_knobs_for_testing(0);
    FX_CHECK(!materializes_flags(translate_words(fixed, true)));
    core::set_p2_knobs_for_testing(core::knob_no_flag_save);
    FX_CHECK(materializes_flags(translate_words(fixed, true)));
    core::set_p2_knobs_for_testing(~0u);
}

// ---- CCMP fused with its only reader -----------------------------------------------

// CMP/CMN; CCMP/CCMN (any condition and immediate NZCV); up to two fillers (a
// flag-free ADD, or an EOR that clobbers EFLAGS: the answer is parked); B.cond
// or a CSEL-family reader; then, half of the time, a compare that makes NZCV dead after the
// reader (the fusion's condition). Differential against the reference with
// and without the fusion; the fused form is branchless (smaller code).
FX_TEST(core_ccmp_fused_with_reader) {
    using namespace a64;
    auto& r = runner();
    Rng rng(0xcc3f05e);
    InstGen g(rng);
    for (const unsigned knob : {0u, static_cast<unsigned>(core::knob_no_ccmp_fusion)}) {
        core::set_p2_knobs_for_testing(knob);
        int failures = 0;
        for (unsigned i = 0; i < 600 && failures < 6; ++i) {
            std::vector<std::uint32_t> w;
            const bool sf = rng.chance(60);
            if (rng.chance(70)) w.push_back(add_shift(sf, true, true, 0, g.src(), 0, g.src(), 31));   // cmp
            else w.push_back(add_imm(sf, rng.chance(50), true, 31, g.src_sp(), rng.below(16)));       // cmp/cmn #imm
            w.push_back(g.ccmp());
            for (unsigned k = 0, n = rng.below(3); k < n; ++k) {   // fillers: flag-free ADD, or EOR (parks the answer)
                if (rng.chance(50)) w.push_back(add_imm(true, false, false, g.dest(), g.src_sp(), rng.below(64)));
                else w.push_back(logic_shift(true, 2, 0, false, g.src(), 0, g.src(), g.dest()));
            }
            if (rng.chance(50)) {
                w.push_back(g.csel());
            } else {
                w.push_back(bcond(rng.below(14), 2));
                w.push_back(add_imm(true, false, false, 3, 3, 1));
            }
            if (rng.chance(50)) w.push_back(add_shift(true, true, true, 0, 2, 0, 1, 31));   // cmp x1, x2: NZCV dead before
            const auto ctx = r.context(0x7777 + i);
            if (!r.run(w, ctx, "ccmp fusion " + std::to_string(knob) + hex_words(w))) ++failures;
        }
    }
    const std::vector<std::uint32_t> fixed = {
        add_imm(true, false, true, 31, 16, 1),                // cmn x16, #1
        ccmp(false, true, true, 2, 4, 12, 1),                 // ccmp w1, #2, #4, gt
        add_imm(true, false, false, 16, 17, 11),              // add x16, x17, #11 (between, flag-free)
        csel(true, false, 16, 1, 0, 17, 16),                  // csel x16, x17, x16, ne
        add_shift(true, true, true, 0, 8, 0, 0, 31),          // cmp x0, x8
        ret(),
    };
    core::set_p2_knobs_for_testing(0);
    const auto fused = translate_words(fixed, true).code.size();
    core::set_p2_knobs_for_testing(core::knob_no_ccmp_fusion);
    const auto branchy = translate_words(fixed, true).code.size();
    core::set_p2_knobs_for_testing(~0u);
    FX_CHECK(fused < branchy);
}

// ---- GPR results reuse the destination's cached host ------------------------------

// Random straight-line and diamond code (data processing, loads, compares and
// branches): differential with and without reusable_gpr, and with the
// opt-in mergedirty (dirty values into merge points). A max(x8, 3)
// diamond keeps x8 in one host on both arms, so the merge needs no reload.
FX_TEST(core_gpr_dest_reuse) {
    using namespace a64;
    auto& r = runner();
    Rng rng(0x6e05e);
    InstGen g(rng);
    for (const unsigned knob : {0u, static_cast<unsigned>(core::knob_no_gpr_reuse), static_cast<unsigned>(core::knob_merge_dirty),
                                static_cast<unsigned>(core::knob_merge_dirty | core::knob_no_gpr_reuse)}) {
        core::set_p2_knobs_for_testing(knob);
        int failures = 0;
        for (unsigned i = 0; i < 300 && failures < 6; ++i) {
            std::vector<std::uint32_t> w;
            for (unsigned k = 0, n = 3 + rng.below(6); k < n; ++k) {
                switch (rng.below(7)) {
                case 0: w.push_back(g.add_sub_imm()); break;
                case 1: w.push_back(g.logic_shift()); break;
                case 2: w.push_back(g.move_wide()); break;
                case 3: w.push_back(g.csel()); break;
                case 4: w.push_back(g.bitfield()); break;
                case 5: w.push_back(g.load_store()); break;
                default:
                    w.push_back(add_shift(true, true, true, 0, g.src(), 0, g.src(), 31));   // cmp
                    w.push_back(bcond(rng.below(14), 2));
                    w.push_back(g.add_sub_imm());
                    break;
                }
            }
            const auto ctx = r.context(0x9191 + i);
            if (!r.run(w, ctx, "gpr reuse " + std::to_string(knob) + hex_words(w))) ++failures;
        }
    }
    const std::vector<std::uint32_t> diamond = {
        add_imm(true, true, true, 31, 8, 3),        // cmp x8, #3
        bcond(10, 2),                               // b.ge L
        movw(true, 2, 0, 3, 8),                     // mov x8, #3
        add_imm(true, false, false, 8, 8, 1),       // L: add x8, x8, #1
        add_imm(true, false, false, 9, 8, 2),       // add x9, x8, #2
        ret(),
    };
    core::set_p2_knobs_for_testing(0);
    const auto reuse = translate_words(diamond, true).code.size();
    core::set_p2_knobs_for_testing(core::knob_no_gpr_reuse);
    const auto fresh = translate_words(diamond, true).code.size();
    core::set_p2_knobs_for_testing(~0u);
    FX_CHECK(reuse <= fresh);
}

// ---- BR/BLR targets already in rcx --------------------------------------------------

// The target of BR/BLR computed into rcx by the register cache (add x9, x10,
// #4) is used in place: no reload of x9 from its context slot after the exit's
// stores (take_value). Behaviour: core_calls_returns_rsb.
FX_TEST(core_indirect_target_not_reloaded) {
    using namespace a64;
    // mov rcx, qword ptr [r15 + x_disp(9)] (disp8 -56): 49 8b 4f c8
    static constexpr std::uint8_t kReload[] = {0x49, 0x8b, 0x4f, 0xc8};
    for (const auto branch : {br(9), blr(9)}) {
        // x1-x4 take r8-r11 (dirty), so x9 lands in rcx, the indirect jump's register.
        const std::vector<std::uint32_t> words = {add_imm(true, false, false, 1, 10, 1), add_imm(true, false, false, 2, 10, 2),
                                                  add_imm(true, false, false, 3, 10, 3), add_imm(true, false, false, 4, 10, 5),
                                                  add_imm(true, false, false, 9, 10, 4), branch};
        const auto region = translate_words(words, true);
        FX_CHECK(std::search(region.code.begin(), region.code.end(), std::begin(kReload), std::end(kReload)) == region.code.end());
    }
    // A target that is a tier-2 copy alias (ldr x2, [x21, x4, lsl #3]; mov x17,
    // x2; br x17: V8's dispatch keeps the handler address in two registers)
    // with no register spare: promoted to its host's entry, not stored and
    // reloaded (galias_promote; fx perf lead's jitless profile). x3-x7 fill
    // the other cache registers.
    // mov rcx, qword ptr [r15 + x_disp(17)] (disp8 8): 49 8b 4f 08
    static constexpr std::uint8_t kReload17[] = {0x49, 0x8b, 0x4f, 0x08};
    core::PlanOverrides o = core::default_plan_overrides();
    o.t2_no_alloc = true;   // x17 a cache entry, not a tier-2 pin
    core::set_overrides_for_testing(&o);
    for (const auto branch : {br(17), blr(17), ret(17)}) {
        const std::vector<std::uint32_t> words = {movw(true, 2, 0, 1, 3), movw(true, 2, 0, 2, 4), movw(true, 2, 0, 3, 5),
                                                  movw(true, 2, 0, 4, 6), movw(true, 2, 0, 5, 7), ldst_reg(3, 1, 4, 3, true, 21, 2),
                                                  logic_shift(true, 1, 0, false, 2, 0, 31, 17), branch};
        for (const unsigned knob : {static_cast<unsigned>(core::knob_no_alias_target), 0u}) {
            core::set_p2_knobs_for_testing(knob);
            const auto region = translate_words(words, true);
            const bool reloads = std::search(region.code.begin(), region.code.end(), std::begin(kReload17), std::end(kReload17)) !=
                                 region.code.end();
            FX_CHECK_EQ(reloads, knob != 0);
        }
    }
    // Behaviour: adr x2, T; mov x17, x2; x3-x7; br/blr x17; T: add x8, x17, #1.
    auto& r = runner();
    for (const auto branch : {br(17), blr(17)}) {
        const std::vector<std::uint32_t> prog = {adr(false, 32, 2), logic_shift(true, 1, 0, false, 2, 0, 31, 17), movw(true, 2, 0, 1, 3),
                                                 movw(true, 2, 0, 2, 4), movw(true, 2, 0, 3, 5), movw(true, 2, 0, 4, 6),
                                                 movw(true, 2, 0, 5, 7), branch, add_imm(true, false, false, 8, 17, 1)};
        for (const unsigned knob : {0u, static_cast<unsigned>(core::knob_no_alias_target)}) {
            core::set_p2_knobs_for_testing(knob);
            for (unsigned k = 0; k < 3; ++k) r.run(prog, r.context(7100 + k), "alias target " + std::to_string(knob));
        }
    }
    core::set_p2_knobs_for_testing(~0u);
    core::set_overrides_for_testing(nullptr);
}

// Regression (round-4 deep fuzz, seed 40027669 variant 0): a CCMP fused with a
// later reader across an EXTR and an SMULH. park_bool held the scratch it
// parked the answer with; when that was rax or rdx, SMULH's take() threw
// "register in use". Translation must succeed and match the reference, with
// and without the fusion.
FX_TEST(core_ccmp_fusion_parks_without_holding_a_register) {
    auto& r = runner();
    const std::vector<std::uint32_t> prog = {
        0x139d339bu, 0xba5bab6du, 0x94000007u, 0xf10007ffu, 0x8b8e349au, 0x38c407ebu, 0xd3407ec6u, 0xfa4c9221u, 0x1400000au,
        0x31001c82u, 0xb81f263au, 0xb10472ffu, 0xfa59a0eeu, 0x93c389c0u, 0x9b4e7d34u, 0x9ba25560u, 0xda8305c0u, 0xd65f03c0u,
    };
    for (const unsigned knob : {0u, static_cast<unsigned>(core::knob_no_ccmp_fusion)}) {
        core::set_p2_knobs_for_testing(knob);
        for (const bool abi : {false, true}) (void)translate_words(prog, abi);
        for (unsigned k = 0; k < 4; ++k) r.run(prog, r.context(40027669u + k), "ccmp fusion park " + std::to_string(knob));
    }
    core::set_p2_knobs_for_testing(~0u);
}
