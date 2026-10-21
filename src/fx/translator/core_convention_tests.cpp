// Cross-region register convention tests (owner: translator core). The
// convention registers (translator_register_convention: SP, x19-x22, FP, LR
// by default) stay in host registers across region boundaries; a region with
// hotter registers evicts some of them (stored at entries, reloaded at
// exits). Checked here: pin selection (convention pins, eviction), and
// differential execution of programs that cross regions through calls,
// returns, links, unlinked exits and lookups while evicting convention
// registers, plus the exact guest state at a fault in a region that evicted
// them.
#include "fx/testing/harness.hpp"
#include "fx/testing/test.hpp"
#include "fx/translator/core_decode.hpp"
#include "fx/translator/core_test_encode.hpp"
#include "fx/translator/core_test_gen.hpp"
#include "fx/translator/core_test_machine.hpp"
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

const RegisterConvention& conv() { return translator_register_convention(); }

// main: a counted loop (x12) whose body uses x0..x11 heavily (hot enough to
// evict convention registers), BL f; then B end.
// f: a non-leaf frame (stp x29, x30; stp x19, x20; stp x21, x22), changes
// x19..x22 and x29, calls g (BL), restores, RET. g: random data, RET.
std::vector<std::uint32_t> program(std::uint64_t seed, std::size_t& f_at) {
    using namespace a64;
    Rng rng(seed);
    InstGen gen(rng);
    // Random data that leaves SP, FP, LR and x19-x22 alone (the frames below
    // must unwind like the reference).
    const auto data = [&] {
        for (;;) {
            const auto word = gen.data_or_memory();
            core::Inst d;
            core::decode(word, 0x1000, d);
            const auto u = core::usage(d);
            if (((u.write | u.read) & ((1u << 31) | (1u << 30) | (1u << 29) | (0xfu << 19))) == 0) return word;
        }
    };
    std::vector<std::uint32_t> w;
    w.push_back(movw(false, 2, 0, 5 + rng.below(20), 12));       // mov w12, #n
    const auto loop = w.size();
    for (unsigned k = 0; k < 12; ++k) {
        const unsigned d = k, s1 = (k + 1) % 12, s2 = (k + 5) % 12;
        w.push_back(add_shift(true, false, false, 0, s2, rng.below(4), s1, d));   // add xd, xs1, xs2, lsl #n
        if (rng.chance(30)) w.push_back(data());
    }
    w.push_back(add_imm(true, true, true, 12, 12, 1));            // subs x12, x12, #1
    w.push_back(bcond(1, static_cast<std::int32_t>(loop) - static_cast<std::int32_t>(w.size())));
    const auto call_f = w.size();
    w.push_back(bl(0));
    w.push_back(add_shift(true, false, false, 0, 19, 0, 0, 0));   // add x0, x0, x19 (callee-saved after the call)
    w.push_back(add_shift(true, false, false, 0, 22, 0, 0, 0));   // add x0, x0, x22
    const auto to_end = w.size();
    w.push_back(b(0));
    f_at = w.size();
    w.push_back(ldst_pair(2, 3, false, -2, 30, 31, 29));          // stp x29, x30, [sp, #-16]!
    w.push_back(add_imm(true, false, false, 29, 31, 0));          // mov x29, sp
    w.push_back(ldst_pair(2, 3, false, -2, 20, 31, 19));          // stp x19, x20, [sp, #-16]!
    w.push_back(ldst_pair(2, 3, false, -2, 22, 31, 21));          // stp x21, x22, [sp, #-16]!
    w.push_back(add_imm(true, false, false, 19, 0, 3));           // add x19, x0, #3
    w.push_back(add_imm(true, false, false, 22, 1, 7));           // add x22, x1, #7
    const auto call_g = w.size();
    w.push_back(bl(0));
    w.push_back(add_shift(true, false, false, 0, 19, 0, 22, 2));  // add x2, x22, x19
    w.push_back(ldst_pair(2, 1, true, 2, 22, 31, 21));            // ldp x21, x22, [sp], #16
    w.push_back(ldst_pair(2, 1, true, 2, 20, 31, 19));            // ldp x19, x20, [sp], #16
    w.push_back(ldst_pair(2, 1, true, 2, 30, 31, 29));            // ldp x29, x30, [sp], #16
    w.push_back(ret());
    const auto g_at = w.size();
    for (unsigned k = 0, n = 2 + rng.below(6); k < n; ++k) w.push_back(data());
    w.push_back(ret());
    const auto end = w.size();
    w[call_f] = bl(static_cast<std::int32_t>(f_at - call_f));
    w[call_g] = bl(static_cast<std::int32_t>(g_at - call_g));
    w[to_end] = b(static_cast<std::int32_t>(end - to_end));
    return w;
}

} // namespace

// Convention registers are pinned in their hosts unless evicted; a hot loop
// over twelve other registers evicts some.
FX_TEST(convention_pin_selection) {
    if (conv().guest_mask == 0) return;   // TRANSMUTE_FX_CORE=noglobals
    auto& r = runner();
    std::size_t f_at = 0;
    const auto words = program(1, f_at);
    auto* p = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(r.program_base()));
    std::memcpy(p, words.data(), words.size() * 4);
    const BufferCode code(r.code_base(), r.program_base() + words.size() * 4);
    core::Plan plan;
    core::build_plan(r.program_base(), code, TranslateOptions{}, core::PlanOverrides{}, plan);
    FX_CHECK_EQ(plan.globals | plan.evicted, conv().guest_mask);
    FX_CHECK_EQ(plan.globals & plan.evicted, 0u);
    for (unsigned g = 0; g < 32; ++g)
        if ((plan.globals >> g) & 1u) FX_CHECK_EQ(plan.pin_host[g], conv().host[g]);
    FX_CHECK(plan.evicted != 0);   // the loop's registers took convention hosts
    // Every host holds at most one pin.
    std::uint32_t hosts = 0;
    for (unsigned g = 0; g < 32; ++g) {
        if (plan.pin_host[g] < 0) continue;
        FX_CHECK(((hosts >> plan.pin_host[g]) & 1u) == 0);
        hosts |= 1u << plan.pin_host[g];
    }
    // The callee region (frame of x19..x22, FP, LR) keeps them all.
    core::build_plan(r.program_base() + 4 * f_at, code, TranslateOptions{}, core::PlanOverrides{}, plan);
    FX_CHECK_EQ(plan.evicted & ((1u << 19) | (1u << 20) | (1u << 29) | (1u << 30) | (1u << 31)), 0u);
}

// Calls and returns between regions that evict different convention
// registers: linked and unlinked exits, inline lookups on and off, inlining
// on and off (the default inlines nothing here: f calls g).
FX_TEST(convention_cross_region_differential) {
    auto& r = runner();
    for (unsigned variant = 0; variant < 4; ++variant) {
        r.machine.link = (variant & 1) == 0;
        r.machine.inline_lookup = (variant & 2) == 0;
        for (unsigned k = 0; k < 150; ++k) {
            std::size_t f_at = 0;
            const auto words = program(1000 + k, f_at);
            FX_CHECK(r.run(words, r.context(2000 + k), "convention " + std::to_string(variant) + "/" + std::to_string(k)));
        }
    }
    r.machine.link = true;
    r.machine.inline_lookup = true;
}

namespace {
struct {
    const TranslatedRegion* region{};
    std::uintptr_t host{};
    std::uint64_t guest_pc{};
    std::uint64_t values[32]{};
    std::uint32_t in_host{};
    bool hit{};
    const void* resume{};
} g_cf;

LONG CALLBACK convention_fault_handler(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || !g_cf.region) return EXCEPTION_CONTINUE_SEARCH;
    const auto rip = static_cast<std::uintptr_t>(info->ContextRecord->Rip);
    if (rip < g_cf.host || rip >= g_cf.host + g_cf.region->code.size()) return EXCEPTION_CONTINUE_SEARCH;
    const auto off = static_cast<std::uint32_t>(rip - g_cf.host);
    const DWORD64* regs = &info->ContextRecord->Rax;
    for (const auto& f : g_cf.region->faults) {
        if (off < f.host_begin || off >= f.host_end) continue;
        g_cf.hit = true;
        g_cf.guest_pc = f.guest_pc;
        for_each_fault_register(g_cf.region->pins, f, g_cf.region->fault_maps, g_cf.region->fault_map_regs,
                                [&](unsigned guest, unsigned host, bool vector) {
                                    if (vector) return;
                                    g_cf.values[guest & 31] = regs[host & 15];
                                    g_cf.in_host |= 1u << (guest & 31);
                                });
    }
    info->ContextRecord->Rip = reinterpret_cast<DWORD64>(g_cf.resume);
    return EXCEPTION_CONTINUE_EXECUTION;
}
} // namespace

// A fault inside the hot loop of a region that evicted convention registers
// (and writes x19 / x29 there): the rebuilt state matches the reference.
FX_TEST(convention_fault_with_evictions) {
    using namespace a64;
    auto& r = runner();
    auto* guard = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS));
    FX_REQUIRE(guard);
    std::vector<std::uint32_t> w;
    w.push_back(movw(false, 2, 0, 9, 12));                        // mov w12, #9
    const auto loop = w.size();
    for (unsigned k = 0; k < 12; ++k) w.push_back(add_shift(true, false, false, 0, (k + 3) % 12, 1, (k + 1) % 12, k));
    w.push_back(add_imm(true, false, false, 19, 19, 1));          // add x19, x19, #1
    w.push_back(add_imm(true, false, false, 29, 29, 2));          // add x29, x29, #2
    w.push_back(add_imm(true, true, true, 12, 12, 1));            // subs x12, x12, #1
    w.push_back(bcond(1, static_cast<std::int32_t>(loop) - static_cast<std::int32_t>(w.size())));
    const auto fault_at = w.size();
    w.push_back(ldst_uimm(3, 1, 0, 13, 15));                      // ldr x15, [x13]  -> faults
    w.push_back(add_imm(true, false, false, 0, 0, 1));
    auto* program = reinterpret_cast<std::uint32_t*>(static_cast<std::uintptr_t>(r.program_base()));
    std::memcpy(program, w.data(), w.size() * 4);
    program[w.size()] = brk(0xdead);
    const BufferCode code(r.code_base(), r.program_base() + (w.size() + 1) * 4);
    r.machine.reset();
    auto ctx = r.context(3100);
    ctx.x[13] = reinterpret_cast<std::uint64_t>(guard);
    const auto initial = ctx;
    r.machine.prepare(ctx);
    const auto* host = r.machine.entry_for(r.program_base(), code);
    FX_REQUIRE(host != nullptr);
    g_cf = {};
    g_cf.region = &r.machine.regions.back();
    g_cf.host = reinterpret_cast<std::uintptr_t>(host) - g_cf.region->entries[0].offset;
    g_cf.resume = ctx.stub_dispatch_exit;   // exit_reason 0: stores nothing (see TestMachine)
    auto* handler = AddVectoredExceptionHandler(1, convention_fault_handler);
    ctx.pc = r.program_base();
    ctx.exit_reason = 0;
    r.machine.enter(ctx, host);
    RemoveVectoredExceptionHandler(handler);
    VirtualFree(guard, 0, MEM_RELEASE);
    FX_REQUIRE(g_cf.hit);
    FX_CHECK_EQ(g_cf.guest_pc, r.program_base() + 4 * fault_at);
    auto expect = initial;
    expect.pc = r.program_base();
    for (unsigned steps = 0; expect.pc != g_cf.guest_pc && steps < 10000; ++steps) {
        const auto word = *reinterpret_cast<const std::uint32_t*>(static_cast<std::uintptr_t>(expect.pc));
        FX_REQUIRE(testing::oracle_step(expect, word));
    }
    for (unsigned g = 0; g < 32; ++g) {
        if (g == 18) continue;
        const auto value = ((g_cf.in_host >> g) & 1u) ? g_cf.values[g] : ctx.x[g];
        if (value != expect.x[g]) std::fprintf(stderr, "    x%u = %llx, reference %llx\n", g,
                                               static_cast<unsigned long long>(value), static_cast<unsigned long long>(expect.x[g]));
        FX_CHECK_EQ(value, expect.x[g]);
    }
}
