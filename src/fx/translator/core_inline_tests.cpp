// Call inlining tests (owner: translator core). Small leaf callees of BL are
// copied into the caller's region (region_plan.cpp). Checked here:
// differential execution against the reference interpreter (random callers
// and leaf callees: loops, several RETs, tail exits, stack use, flags across
// the call), which callees are inlined, region metadata (guest ranges cover
// the callee, no entries inside the copy), and the exact guest state at a
// fault inside an inlined body (callee pc, x30 = return address, SP), plus
// resumption from that state.
#include "fx/testing/harness.hpp"
#include "fx/testing/test.hpp"
#include "fx/translator/core_decode.hpp"
#include "fx/translator/core_test_encode.hpp"
#include "fx/translator/core_test_gen.hpp"
#include "fx/translator/core_test_machine.hpp"
#include "fx/translator/region_emitter.hpp"
#include "fx/translator/region_plan.hpp"
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

bool inlining_enabled() {
    return (core::codegen_knob_bits() & (1u << 23)) == 0 && core::codegen_inline_max() != 0;
}

std::uint64_t inlined_now() { return core::core_counters().inlined_calls.load(); }

bool covers(const TranslatedRegion& r, std::uint64_t pc) {
    for (const auto& [b, e] : r.guest_ranges)
        if (pc >= b && pc < e) return true;
    return false;
}

bool has_entry_at(const TranslatedRegion& r, std::uint64_t pc) {
    for (const auto& e : r.entries)
        if (e.guest_pc == pc) return true;
    return false;
}

// ---- random callers of random leaf callees ------------------------------------

struct Patch {
    std::size_t at, target;
    int kind;   // 0 b.cond / cbz, 1 b / bl
};

void apply(std::vector<std::uint32_t>& w, const Patch& p) {
    const auto off = static_cast<std::int32_t>(p.target) - static_cast<std::int32_t>(p.at);
    if (p.kind == 0) w[p.at] |= (static_cast<std::uint32_t>(off) & 0x7ffff) << 5;
    else w[p.at] |= static_cast<std::uint32_t>(off) & 0x3ffffff;
}

bool reads_x30(std::uint32_t word) {
    core::Inst d;
    core::decode(word, 0x1000, d);
    const auto u = core::usage(d);
    return ((u.read | u.write) >> 30) & 1u;
}

// main: straight code, flag setters, BLs to 1..3 leaves (also inside a
// counted loop), then B end. Each leaf: optional stack frame, straight code,
// optional loop, optional early RET / tail exit to the end, RET.
std::vector<std::uint32_t> generate(std::uint64_t seed, unsigned& leaves_out) {
    Rng rng(seed);
    InstGen g(rng);
    std::vector<std::uint32_t> w;
    std::vector<Patch> patches;
    const unsigned leaves = 1 + rng.below(3);
    leaves_out = leaves;
    std::vector<std::vector<std::size_t>> calls(leaves);
    std::vector<std::size_t> to_end;
    const auto data = [&] {
        for (;;) {
            const auto word = g.data_or_memory();
            if (!reads_x30(word)) return word;
        }
    };
    const auto call = [&] {
        const auto k = rng.below(leaves);
        w.push_back(a64::bl(0));
        calls[k].push_back(w.size() - 1);
    };
    const unsigned main_len = 4 + rng.below(20);
    bool loop_used = false;
    while (w.size() < main_len) {
        const auto c = rng.below(100);
        if (c < 45) {
            w.push_back(data());
        } else if (c < 60) {
            w.push_back(a64::add_shift(true, true, true, 0, rng.pick(kSrc), 0, rng.pick(kSrc), 31));   // cmp
        } else if (c < 70) {   // flags read after a call (set before it or by the callee)
            call();
            w.push_back(a64::csel(true, rng.chance(50), rng.pick(kSrc), rng.below(14), 0, rng.pick(kSrc), rng.pick(kDest)));
        } else if (c < 80 && !loop_used) {
            loop_used = true;
            w.push_back(a64::movw(false, 2, 0, 1 + rng.below(4), 14));   // mov w14, #n (x14: not a leaf dest)
            const auto start = w.size();
            w.push_back(data());
            call();
            w.push_back(a64::add_imm(true, true, false, 14, 14, 1));   // sub x14, x14, #1
            w.push_back(a64::cbz(true, true, 0, 14));                  // cbnz x14, start
            patches.push_back({w.size() - 1, start, 0});
        } else {
            call();
        }
    }
    w.push_back(a64::b(0));
    to_end.push_back(w.size() - 1);
    for (unsigned k = 0; k < leaves; ++k) {
        w.push_back(a64::nop());
        const auto entry = w.size();
        const bool frame = rng.chance(40);
        const bool odd = rng.chance(8);   // x30 used: never inlined
        if (frame) w.push_back(a64::ldst_pair(2, 3, false, -4, 20, 31, 19));   // stp x19, x20, [sp, #-32]!
        for (unsigned i = 0, n = rng.below(6); i < n; ++i) w.push_back(data());
        if (odd) w.push_back(a64::logic_shift(true, 1, 0, false, 30, 0, 31, 11));   // mov x11, x30
        std::size_t early = 0;
        if (rng.chance(30)) {   // early RET (or a tail exit) on a condition
            early = w.size();
            w.push_back(rng.chance(50) ? a64::bcond(rng.below(14), 0) : a64::cbz(true, rng.chance(50), 0, rng.pick(kSrc)));
        }
        if (rng.chance(35)) {   // counted loop (x12)
            w.push_back(a64::movw(false, 2, 0, 1 + rng.below(4), 12));
            const auto start = w.size();
            for (unsigned i = 0, n = 1 + rng.below(4); i < n; ++i) w.push_back(data());
            w.push_back(a64::add_imm(true, true, true, 12, 12, 1));   // subs x12, x12, #1
            w.push_back(a64::bcond(1, 0));
            patches.push_back({w.size() - 1, start, 0});
        }
        for (unsigned i = 0, n = rng.below(4); i < n; ++i) w.push_back(data());
        if (frame) w.push_back(a64::ldst_pair(2, 1, true, 4, 20, 31, 19));   // ldp x19, x20, [sp], #32
        w.push_back(a64::ret());
        if (early) {
            if (rng.chance(25)) {
                to_end.push_back(early);   // tail exit: leaves the callee for the end of main
            } else {
                w.push_back(a64::ret());   // a second RET
                patches.push_back({early, w.size() - 1, 0});
            }
        }
        for (const auto at : calls[k]) patches.push_back({at, entry, 1});
    }
    for (const auto at : to_end) patches.push_back({at, w.size(), (w[at] & 0xfc000000u) == 0x14000000u ? 1 : 0});
    for (const auto& p : patches) apply(w, p);
    return w;
}

void inline_fuzz(std::uint64_t first, std::uint64_t count, unsigned variant) {
    auto& r = runner();
    const auto saved = r.machine.options;
    int failures = 0;
    std::uint64_t programs_inlined = 0;
    for (std::uint64_t seed = first; seed < first + count && failures < 6; ++seed) {
        unsigned leaves = 0;
        const auto words = generate(seed, leaves);
        auto ctx = r.context(seed * 7 + 3);
        r.machine.options = saved;
        r.machine.link = variant != 1;
        if (variant == 2) r.machine.options.max_instructions = 2 + static_cast<unsigned>(seed % 8);
        const auto before = inlined_now();
        if (!r.run(words, ctx, "inline fuzz seed " + std::to_string(seed))) {
            ++failures;
            std::string s;
            char buf[16];
            for (const auto x : words) {
                std::snprintf(buf, sizeof buf, "%08x ", x);
                s += buf;
            }
            std::fprintf(stderr, "    program: %s\n", s.c_str());
        }
        programs_inlined += inlined_now() != before;
    }
    r.machine.options = saved;
    r.machine.link = true;
    std::printf("    %llu of %llu programs inlined calls\n", static_cast<unsigned long long>(programs_inlined),
                static_cast<unsigned long long>(count));
    if (inlining_enabled() && variant != 2) FX_CHECK(programs_inlined * 2 > count);
}

// ---- faults inside inlined bodies ---------------------------------------------

struct FaultCapture {
    const TranslatedRegion* region{};
    std::uintptr_t host{};
    bool hit{};
    std::uint64_t guest_pc{};
    FlagsLocation flags{};
    std::uint8_t flags_register{};
    std::uint64_t eflags{};
    std::uint64_t values[32]{};
    std::uint64_t flags_value{};
    std::uint32_t in_host{};
    const void* resume{};
} g_fc;

LONG CALLBACK inline_fault_handler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !g_fc.region) return EXCEPTION_CONTINUE_SEARCH;
    const auto rip = static_cast<std::uintptr_t>(info->ContextRecord->Rip);
    if (rip < g_fc.host || rip >= g_fc.host + g_fc.region->code.size()) return EXCEPTION_CONTINUE_SEARCH;
    const auto off = static_cast<std::uint32_t>(rip - g_fc.host);
    const DWORD64* regs = &info->ContextRecord->Rax;   // rax..r15 in x64 order
    for (const auto& f : g_fc.region->faults) {
        if (off < f.host_begin || off >= f.host_end) continue;
        g_fc.hit = true;
        g_fc.guest_pc = f.guest_pc;
        g_fc.flags = f.flags;
        g_fc.flags_register = f.flags_register;
        g_fc.eflags = info->ContextRecord->EFlags;
        g_fc.flags_value = regs[f.flags_register & 15];
        for_each_fault_register(g_fc.region->pins, f, g_fc.region->fault_maps, g_fc.region->fault_map_regs,
                                [&](unsigned guest, unsigned host, bool vector) {
                                    if (vector) return;
                                    g_fc.values[guest & 31] = regs[host & 15];
                                    g_fc.in_host |= 1u << (guest & 31);
                                });
    }
    info->ContextRecord->Rip = reinterpret_cast<DWORD64>(g_fc.resume);
    return EXCEPTION_CONTINUE_EXECUTION;
}

std::uint64_t nzcv_at_fault(const GuestContext& ctx) {
    const auto f = g_fc.eflags;
    const std::uint64_t n = (f >> 7) & 1, z = (f >> 6) & 1, cf = f & 1, v = (f >> 11) & 1;
    switch (g_fc.flags) {
    case FlagsLocation::host_sub: return (n << 31) | (z << 30) | ((cf ^ 1) << 29) | (v << 28);
    case FlagsLocation::host_add:
    case FlagsLocation::host_logic: return (n << 31) | (z << 30) | (cf << 29) | (v << 28);
    case FlagsLocation::host_register: return g_fc.flags_value & 0xf0000000u;
    default: return ctx.nzcv;
    }
}

// Runs `prog` (main, then its leaf callees) with x9 at a page whose upper half
// is inaccessible: the access at word `fault_word` faults on its
// `occurrence`-th execution. The callee at `callee_word` must be inlined into
// the region; the guest state rebuilt at the fault (pins, fault map, flags,
// context) must equal the reference state before that instruction (pc, x30 =
// return address of the call, SP inside the callee frame), and resuming from
// it with the page accessible must finish like the reference run.
void check_inline_fault(const std::vector<std::uint32_t>& prog, unsigned fault_word, unsigned callee_word,
                        unsigned occurrence, const char* label, std::uint64_t seed) {
    auto& r = runner();
    auto* page = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 8192, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    FX_REQUIRE(page);
    for (unsigned k = 0; k < 8192; ++k) page[k] = static_cast<std::uint8_t>(k * 29 + 5);
    DWORD old = 0;
    VirtualProtect(page + 4096, 4096, PAGE_NOACCESS, &old);
    auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(r.program_base()));
    std::memcpy(program, prog.data(), prog.size() * 4);
    program[prog.size()] = a64::brk(0xdead);
    const BufferCode code(r.code_base(), r.program_base() + (prog.size() + 1) * 4);
    auto ctx = r.context(seed);
    ctx.x[9] = reinterpret_cast<std::uint64_t>(page) + (occurrence > 1 ? 0 : 4096);
    ctx.x[1] = 1;
    ctx.x[2] = 2;
    const auto initial = ctx;
    const auto base = r.program_base();

    r.machine.reset();
    r.machine.prepare(ctx);
    const auto inlined_before = inlined_now();
    const auto* host = r.machine.entry_for(base, code);
    FX_REQUIRE(host != nullptr);
    const auto& region = r.machine.regions.back();
    if (inlining_enabled()) {
        FX_CHECK(inlined_now() > inlined_before);
        FX_CHECK(covers(region, base + 4ull * callee_word));
        FX_CHECK(covers(region, base + 4ull * fault_word));
        for (std::uint64_t pc = base + 4ull * callee_word; pc < base + 4ull * prog.size(); pc += 4)
            FX_CHECK(!has_entry_at(region, pc));
    }
    g_fc = {};
    g_fc.region = &region;
    g_fc.host = reinterpret_cast<std::uintptr_t>(host) - region.entries[0].offset;
    g_fc.resume = ctx.stub_dispatch_exit;
    auto* handler = AddVectoredExceptionHandler(1, inline_fault_handler);
    ctx.pc = base;
    r.machine.enter(ctx, host);
    RemoveVectoredExceptionHandler(handler);
    FX_REQUIRE(g_fc.hit);
    FX_CHECK_EQ(g_fc.guest_pc, base + 4ull * fault_word);

    auto state = ctx;
    for (unsigned g = 0; g < 32; ++g)
        if ((g_fc.in_host >> g) & 1u) state.x[g] = g_fc.values[g];
    state.nzcv = nzcv_at_fault(ctx);
    state.pc = g_fc.guest_pc;
    auto before = initial;
    before.pc = base;
    for (unsigned seen = 0, steps = 0; steps < 100000; ++steps) {
        if (before.pc == g_fc.guest_pc && ++seen == occurrence) break;
        const auto word = *reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(before.pc));
        FX_REQUIRE(testing::oracle_step(before, word));
    }
    for (unsigned g = 0; g < 32; ++g) {
        if (state.x[g] != before.x[g])
            std::fprintf(stderr, "    %s: x%u = %llx, reference %llx\n", label, g,
                         static_cast<unsigned long long>(state.x[g]), static_cast<unsigned long long>(before.x[g]));
        FX_CHECK_EQ(state.x[g], before.x[g]);
    }
    FX_CHECK_EQ(state.nzcv, before.nzcv);

    // Resume at the faulting callee instruction (a new region there; its RET
    // finds the continuation by lookup) with the page accessible.
    VirtualProtect(page + 4096, 4096, PAGE_READWRITE, &old);
    auto expect = initial;
    expect.pc = base;
    const auto stop = base + prog.size() * 4;
    for (unsigned steps = 0; expect.pc != stop && steps < 100000; ++steps) {
        const auto word = *reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(expect.pc));
        FX_REQUIRE(testing::oracle_step(expect, word));
    }
    std::vector<std::uint8_t> expect_page(page, page + 8192);
    for (unsigned k = 0; k < 8192; ++k) page[k] = static_cast<std::uint8_t>(k * 29 + 5);
    // Stack memory the program wrote before the fault is already in place.
    r.machine.reset();
    const auto res = r.machine.run(state, code, kReturnToHostPc);
    FX_CHECK(res.ok && res.reason == ExitReason::brk && res.arg == 0xdead);
    expect.pc = state.pc;
    testing::compare_contexts(expect, state, label);
    FX_CHECK(std::memcmp(expect_page.data(), page, 8192) == 0);
    VirtualFree(page, 0, MEM_RELEASE);
}

} // namespace

FX_TEST(inline_callee_selection) {
    auto& r = runner();
    using namespace a64;
    const auto base = r.program_base();
    struct Case {
        const char* label;
        std::vector<std::uint32_t> callee;
        bool inlined;
        bool run = true;   // also run it against the reference
    };
    const unsigned limit = core::codegen_inline_max();
    std::vector<std::uint32_t> too_big(limit + 1, add_imm(true, false, false, 0, 0, 1));
    too_big.back() = ret();
    std::vector<std::uint32_t> at_limit(limit, add_imm(true, false, false, 0, 0, 1));
    at_limit.back() = ret();
    const std::vector<Case> cases = {
        {"straight leaf", {add_imm(true, false, false, 0, 0, 1), ret()}, true},
        {"loop leaf", {movw(false, 2, 0, 3, 12), add_imm(true, false, false, 0, 0, 1), add_imm(true, true, true, 12, 12, 1),
                       bcond(1, -2), ret()}, true},
        {"two rets", {cbz(true, false, 3, 1), add_imm(true, false, false, 0, 0, 1), ret(), ret()}, true},
        {"stack frame", {ldst_pair(2, 3, false, -2, 20, 31, 19), add_imm(true, false, false, 19, 0, 1),
                         ldst_pair(2, 1, true, 2, 20, 31, 19), ret()}, true},
        {"conditional tail exit", {cbz(true, true, 2, 0), ret(), b(-3)}, true},   // cbnz x0 -> b to main's `b end`
        {"at the size limit", at_limit, true},
        {"reads x30", {logic_shift(true, 1, 0, false, 30, 0, 31, 0), ret()}, false},
        {"writes x30", {movw(true, 2, 0, 1, 30), ret()}, false, false},
        {"saves x30", {ldst_pair(2, 3, false, -2, 30, 31, 29), ldst_pair(2, 1, true, 2, 30, 31, 29), ret()}, false},
        {"calls", {ldst_pair(2, 3, false, -2, 30, 31, 29), bl(3), ldst_pair(2, 1, true, 2, 30, 31, 29), ret(), ret()}, false},
        {"br x30", {add_imm(true, false, false, 0, 0, 1), br(30)}, false},
        {"ret x1", {add_imm(true, false, false, 0, 0, 1), ret(1)}, false, false},
        {"svc", {svc(1), ret()}, false, false},
        {"no ret (falls out)", {add_imm(true, false, false, 0, 0, 1)}, false},
        {"too big", too_big, false},
    };
    for (const auto& c : cases) {
        // main: bl f; add x2, x2, #1; b end; f: ...
        std::vector<std::uint32_t> prog = {bl(3), add_imm(true, false, false, 2, 2, 1), b(0)};
        prog.insert(prog.end(), c.callee.begin(), c.callee.end());
        prog[2] = b(static_cast<std::int32_t>(prog.size()) - 2);
        auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(base));
        std::memcpy(program, prog.data(), prog.size() * 4);
        program[prog.size()] = brk(0xdead);
        const BufferCode code(r.code_base(), base + (prog.size() + 1) * 4);
        const auto before = inlined_now();
        const auto region = translate_region(base, code);
        const bool inlined = inlined_now() != before;
        if (inlined != (c.inlined && inlining_enabled())) std::fprintf(stderr, "    %s: inlined %d\n", c.label, inlined ? 1 : 0);
        FX_CHECK_EQ(inlined, c.inlined && inlining_enabled());
        FX_CHECK_EQ(covers(region, base + 12), inlined);
        // An inlined call is straight-line code: one entry, no exit to the callee.
        if (inlined) {
            FX_CHECK_EQ(region.entries.size(), 1u);
            for (const auto& e : region.exits) FX_CHECK(e.target_pc != base + 12);
        }
        // Either way the program runs like the reference.
        if (c.run) r.run(prog, r.context(4100), std::string("selection: ") + c.label);
    }
    // Callees with .pdata bounds: inlined only at a function start, within the size limit.
    std::vector<std::uint32_t> prog = {bl(3), add_imm(true, false, false, 2, 2, 1), b(3), add_imm(true, false, false, 0, 0, 1),
                                       ret()};
    std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(base)), prog.data(), prog.size() * 4);
    BufferCode code(r.code_base(), base + prog.size() * 4 + 4);
    code.fn_begin = base + 12;
    code.fn_end = base + 20;
    auto before = inlined_now();
    (void)translate_region(base, code);
    FX_CHECK_EQ(inlined_now() != before, inlining_enabled());
    code.fn_begin = base + 8;   // the target is not a function start
    before = inlined_now();
    (void)translate_region(base, code);
    FX_CHECK(inlined_now() == before);
}

FX_TEST(inline_calls_differential) { inline_fuzz(1, 2500, 0); }
FX_TEST(inline_calls_differential_unlinked) { inline_fuzz(50000, 500, 1); }
FX_TEST(inline_calls_differential_small_regions) { inline_fuzz(60000, 500, 2); }

// Flags set before an inlined call and read after it, through a callee that
// does not touch them (exact NZCV, also under the ABI flags assumption).
FX_TEST(inline_flags_flow_through_callee) {
    auto& r = runner();
    using namespace a64;
    for (unsigned k = 0; k < 40; ++k) {
        const std::vector<std::uint32_t> prog = {
            add_shift(true, true, true, 0, 2, 0, 1, 31),   // 0 cmp x1, x2
            bl(5),                                          // 1 bl f (6)
            csel(true, false, 4, k % 14, 0, 3, 5),          // 2 csel x5, x3, x4, cond
            bcond((k + 3) % 14, 2),                         // 3 b.cond 5
            add_imm(true, false, false, 6, 6, 1),           // 4
            b(4),                                           // 5 b end (9)
            add_imm(true, false, false, 3, 3, 9),           // 6 f: add x3, x3, #9
            ldst_uimm(3, 1, 0, 16, 7),                      // 7 ldr x7, [x16]
            ret(),                                          // 8 ret
        };
        r.run(prog, r.context(4200 + k), "flags through callee " + std::to_string(k));
    }
}

FX_TEST(inline_fault_in_callee_frame) {
    using namespace a64;
    const std::vector<std::uint32_t> prog = {
        add_imm(true, false, false, 19, 19, 5),          // 0 add x19, x19, #5
        add_imm(true, false, false, 3, 3, 7),            // 1 add x3, x3, #7 (dirty in the cache)
        add_shift(true, true, true, 0, 2, 0, 1, 31),     // 2 cmp x1, x2 (live across the call)
        bl(5),                                           // 3 bl f (8)
        csel(true, false, 19, 0, 0, 3, 4),               // 4 csel x4, x3, x19, eq
        add_imm(true, false, false, 0, 0, 1),            // 5 add x0, x0, #1
        b(8),                                            // 6 b end (14)
        nop(),                                           // 7
        add_imm(true, true, false, 31, 31, 32),          // 8 f: sub sp, sp, #32
        ldst_uimm(3, 0, 1, 31, 3),                       // 9 str x3, [sp, #8]
        add_imm(true, false, false, 5, 5, 9),            // 10 add x5, x5, #9
        ldst_uimm(3, 1, 1, 9, 6),                        // 11 ldr x6, [x9, #8]  -> faults
        add_imm(true, false, false, 31, 31, 32),         // 12 add sp, sp, #32
        ret(),                                           // 13 ret
    };
    check_inline_fault(prog, 11, 8, 1, "fault in callee frame", 7100);
}

FX_TEST(inline_fault_in_second_call_after_loop) {
    using namespace a64;
    const std::vector<std::uint32_t> prog = {
        bl(7),                                           // 0 bl f (7)
        add_imm(true, false, false, 9, 9, 1, true),      // 1 add x9, x9, #4096 (into the guard half)
        add_shift(true, true, true, 0, 1, 0, 2, 31),     // 2 cmp x2, x1
        bl(4),                                           // 3 bl f (7)
        csel(true, false, 19, 1, 1, 3, 4),               // 4 csinc x4, x3, x19, ne
        b(10),                                           // 5 b end (15)
        nop(),                                           // 6
        movw(false, 2, 0, 3, 12),                        // 7 f: mov w12, #3
        add_shift(true, false, false, 0, 12, 0, 3, 3),   // 8 L: add x3, x3, x12
        add_imm(true, true, true, 12, 12, 1),            // 9 subs x12, x12, #1
        bcond(1, -2),                                    // 10 b.ne L
        ldst_uimm(3, 1, 2, 9, 6),                        // 11 ldr x6, [x9, #16]  -> faults (second call)
        add_shift(true, false, false, 0, 3, 0, 6, 6),    // 12 add x6, x6, x3
        ldst_uimm(3, 0, 3, 16, 6),                       // 13 str x6, [x16, #24]
        ret(),                                           // 14 ret
    };
    check_inline_fault(prog, 11, 7, 2, "fault in second call after loop", 7200);
}

FX_TEST(inline_fault_after_early_return_path) {
    using namespace a64;
    const std::vector<std::uint32_t> prog = {
        add_shift(true, true, true, 0, 2, 0, 1, 31),     // 0 cmp x1, x2 (1 != 2: ne)
        bl(5),                                           // 1 bl f (6)
        add_imm(true, false, false, 0, 0, 1),            // 2 add x0, x0, #1
        bl(3),                                           // 3 bl f (6)
        b(8),                                            // 4 b end (12)
        nop(),                                           // 5
        bcond(0, 4),                                     // 6 f: b.eq out (10)
        add_imm(true, false, false, 7, 7, 3),            // 7 add x7, x7, #3
        ldst_uimm(3, 1, 3, 9, 8),                        // 8 ldr x8, [x9, #24]  -> faults
        ret(),                                           // 9 ret
        add_imm(true, false, false, 7, 7, 1),            // 10 out: add x7, x7, #1
        ret(),                                           // 11 ret
    };
    check_inline_fault(prog, 8, 6, 1, "fault after early return path", 7300);
}

// The copy follows the callee's code: changing the callee and translating
// again changes the inlined body (the runtime's SMC invalidation covers it
// through TranslatedRegion::guest_ranges).
FX_TEST(inline_callee_change_retranslates) {
    auto& r = runner();
    using namespace a64;
    std::vector<std::uint32_t> prog = {
        bl(4),                                           // 0 bl f (4)
        bl(3),                                           // 1 bl f (4)
        add_imm(true, false, false, 2, 2, 1),            // 2
        b(3),                                            // 3 b end (6)
        add_imm(true, false, false, 0, 0, 5),            // 4 f: add x0, x0, #5
        ret(),                                           // 5
    };
    const auto ctx = r.context(7400);
    r.run(prog, ctx, "callee v1");
    const auto x0_v1 = r.actual.x[0];
    FX_CHECK_EQ(x0_v1, ctx.x[0] + 10);
    const auto base = r.program_base();
    {
        const BufferCode code(r.code_base(), base + prog.size() * 4 + 4);
        const auto region = translate_region(base, code);
        if (inlining_enabled()) {
            FX_CHECK(covers(region, base + 16));
            FX_CHECK(covers(region, base + 20));
        }
    }
    prog[4] = add_imm(true, true, false, 0, 0, 3);   // f: sub x0, x0, #3
    r.run(prog, ctx, "callee v2");
    FX_CHECK_EQ(r.actual.x[0], ctx.x[0] - 6);
}
