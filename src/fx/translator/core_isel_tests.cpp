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
