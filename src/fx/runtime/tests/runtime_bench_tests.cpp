// Runtime cost measurements (printed; they only fail on functional errors).

#include "fx/runtime/tests/test_support.hpp"
#include "fx/testing/test.hpp"

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <vector>

using namespace rtt;
namespace rt = transmute::fx::rt;
using x64::Alu;

namespace {

void init() { runtime_initialize(RuntimeOptions{}); }

// Best of `repeats` samples: the machine is shared with parallel builds, so
// the minimum is the least noisy estimate.
template <typename F>
double best_ns(int repeats, F&& f) {
    std::vector<double> samples;
    for (int i = 0; i < repeats; ++i) {
        const double start = now_ns();
        f();
        samples.push_back(now_ns() - start);
    }
    return *std::min_element(samples.begin(), samples.end());
}

// One-shot measurement (state-changing work such as first-time linking).
template <typename F>
double once_ns(F&& f) {
    const double start = now_ns();
    f();
    return now_ns() - start;
}

// Loop region at pc: x0 -= 1; if zero return; else continue at pc through
// the lookup stub (indirect) or a linked direct branch.
TranslatedRegion loop_region(std::uint64_t pc, bool indirect) {
    RegionBuilder b(pc);
    b.entry(pc);
    auto& a = b.as();
    const Label done = a.new_label();
    a.alu(Alu::sub, 64, ptr(Gp::r15, x_disp(0)), 1);
    a.jcc(x64::Cond::e, done);
    if (indirect) b.indirect_exit(pc);
    else b.direct_exit(pc);
    a.bind(done);
    b.return_via_x30();
    return b.finish();
}

DWORD WINAPI empty_thread(void*) { return 0; }

} // namespace

FX_TEST(bench_lookup_stub_hit_and_linked_jump) {
    init();
    GuestCode code({});
    const auto pi = code.at(0), pd = code.at(64);
    set_region_factory(pi, [](std::uint64_t pc) { return loop_region(pc, true); });
    set_region_factory(pd, [](std::uint64_t pc) { return loop_region(pc, false); });
    constexpr std::uint64_t kIterations = 20'000'000;
    std::uint64_t warm[] = {10};
    call_guest(pi, warm);
    call_guest(pd, warm);
    std::uint64_t args[] = {kIterations};
    const auto s0 = runtime_stats();
    const double indirect = best_ns(7, [&] { FX_CHECK_EQ(call_guest(pi, args), 0u); }) / kIterations;
    const double direct = best_ns(7, [&] { FX_CHECK_EQ(call_guest(pd, args), 0u); }) / kIterations;
    const auto s1 = runtime_stats();
    std::uint64_t exits = 0;
    for (unsigned i = 0; i < 16; ++i) exits += s1.exits[i] - s0.exits[i];
    FX_CHECK_EQ(exits, 14u);   // one return_to_host per call (7 + 7 samples)
    std::printf("  bench lookup-stub hit: %.2f ns/iteration; linked direct branch: %.2f ns/iteration;"
                " stub cost ~%.2f ns\n", indirect, direct, indirect - direct);
    set_region_factory(pi, nullptr);
    set_region_factory(pd, nullptr);
}

FX_TEST(bench_link_exit) {
    init();
    constexpr unsigned kRegions = 4000;
    GuestCode code({}, kRegions * 16 + 0x1000);
    const auto pc = [&](unsigned i) { return code.base() + std::uint64_t{i} * 16; };
    for (unsigned i = 0; i < kRegions; ++i) {
        set_region_factory(pc(i), [&, i](std::uint64_t p) {
            RegionBuilder b(p);
            b.entry(p);
            b.as().alu(Alu::add, 64, ptr(Gp::r15, x_disp(0)), 1);
            if (i + 1 < kRegions) b.direct_exit(pc(i + 1));
            else b.return_via_x30();
            return b.finish();
        });
    }
    // Translate front to back: each install links the previous region's
    // pending exit (eager linking), so the first run never leaves the chain.
    const auto before = runtime_stats();
    const double translate = once_ns([&] {
        for (unsigned i = 0; i < kRegions; ++i) rt::translate_and_install(pc(i));
    }) / kRegions;
    const auto s0 = runtime_stats();
    FX_CHECK_EQ(s0.links - before.links, kRegions - 1);
    std::uint64_t args[] = {0};
    const double first = once_ns([&] { FX_CHECK_EQ(call_guest(pc(0), args), kRegions); });
    const auto s1 = runtime_stats();
    constexpr unsigned kLinkExit = static_cast<unsigned>(ExitReason::link);
    FX_CHECK_EQ(s1.links - s0.links, 0u);
    FX_CHECK_EQ(s1.exits[kLinkExit] - s0.exits[kLinkExit], 0u);
    const double linked = best_ns(7, [&] { FX_CHECK_EQ(call_guest(pc(0), args), kRegions); });
    std::printf("  bench pre-linked chain, first run: %.2f ns/hop; linked chain: %.2f ns/hop;"
                " install + eager link (test translator): %.0f ns/region\n",
                first / kRegions, linked / kRegions, translate);
    for (unsigned i = 0; i < kRegions; ++i) set_region_factory(pc(i), nullptr);
}

FX_TEST(bench_call_guest_round_trip) {
    init();
    GuestCode code({});
    const auto p = code.at(0);
    set_region_factory(p, [](std::uint64_t pc) {
        RegionBuilder b(pc);
        b.entry(pc);
        b.return_via_x30();
        return b.finish();
    });
    call_guest(p, {});
    constexpr int kCalls = 1'000'000;
    GuestContext& ctx = ensure_thread_context();
    double ns = 1e30, prepared = 1e30;
    for (int round = 0; round < 4; ++round) {   // interleaved: both see the same machine state
        ns = std::min(ns, best_ns(2, [&] {
            for (int i = 0; i < kCalls; ++i) call_guest(p, {});
        }) / kCalls);
        prepared = std::min(prepared, best_ns(2, [&] {
            for (int i = 0; i < kCalls; ++i) call_guest_prepared(ctx, p);
        }) / kCalls);
    }
    std::printf("  bench call_guest round trip (empty guest function): %.1f ns; call_guest_prepared: %.1f ns\n",
                ns, prepared);
    set_region_factory(p, nullptr);
}

FX_TEST(bench_host_call_into_guest_code) {
    init();
    GuestCode code({0x8b010000 /* add x0, x0, x1 */, 0xd65f03c0});
    const auto p = code.at(0);
    set_region_factory(p, [](std::uint64_t pc) {
        RegionBuilder b(pc);
        b.entry(pc);
        b.as().mov(64, Gp::rax, ptr(Gp::r15, x_disp(1)));
        b.as().alu(Alu::add, 64, ptr(Gp::r15, x_disp(0)), Gp::rax);
        b.return_via_x30();
        b.cover(pc, pc + 8);
        return b.finish();
    });
    using Fn = std::uint64_t (*)(std::uint64_t, std::uint64_t);
    const auto fn = reinterpret_cast<Fn>(p);
    FX_CHECK_EQ(fn(1, 2), 3u);
    constexpr int kCalls = 20000;
    const double ns = best_ns(5, [&] {
        for (int i = 0; i < kCalls; ++i) fn(i, 1);
    }) / kCalls;
    std::printf("  bench host->guest call via execute fault + bridge: %.0f ns\n", ns);
    set_region_factory(p, nullptr);
}

FX_TEST(bench_thread_creation) {
    init();
    GuestCode code({});
    const auto p = code.at(0);
    set_region_factory(p, [](std::uint64_t pc) {
        RegionBuilder b(pc);
        b.entry(pc);
        b.return_via_x30();
        return b.finish();
    });
    constexpr int kThreads = 200;
    const double guest = best_ns(5, [&] {
        for (int i = 0; i < kThreads; ++i) {
            GuestThreadStart start{p, 0, 0, 0};
            const auto handle = reinterpret_cast<HANDLE>(create_guest_thread(start, nullptr));
            WaitForSingleObject(handle, INFINITE);
            CloseHandle(handle);
        }
    }) / kThreads;
    const double native = best_ns(5, [&] {
        for (int i = 0; i < kThreads; ++i) {
            HANDLE handle = CreateThread(nullptr, 0, &empty_thread, nullptr, 0, nullptr);
            WaitForSingleObject(handle, INFINITE);
            CloseHandle(handle);
        }
    }) / kThreads;
    std::printf("  bench guest thread create+run+exit+join: %.1f us (native empty thread: %.1f us)\n",
                guest / 1000, native / 1000);
    set_region_factory(p, nullptr);
}

FX_TEST(bench_interpreter_fallback) {
    init();
    GuestCode code({
        0xd2800000,   // movz x0, #0
        0x8b020000,   // loop: add x0, x0, x2
        0xd1000442,   // sub x2, x2, #1
        0xb5ffffc2,   // cbnz x2, loop
        0xd65f03c0,
    });
    constexpr std::uint64_t kLoops = 20000;
    std::uint64_t args[] = {0, 0, kLoops};
    call_guest(code.at(0), args);
    const double ns = best_ns(5, [&] { FX_CHECK_EQ(call_guest(code.at(0), args), kLoops * (kLoops + 1) / 2); });
    std::printf("  bench interpret exit (exit + interpret_one + lookup + re-entry): %.0f ns/instruction\n",
                ns / (3 * kLoops + 2));
}

// A guest loop calling a native host callback (register_host_callback): BL
// into the callback trampoline (linked), the C++ call, the RSB return. The
// trampoline's FP-state handoff (docs/FX_ENGINE.md, FP) is part of the cost.
FX_TEST(bench_host_callback_round_trip) {
    init();
    GuestCode code({});
    const auto pm = code.at(0), cont = code.at(2), callee = code.at(16);
    register_host_callback(callee, [](GuestContext& ctx, void*) { ++ctx.x[1]; }, nullptr);
    set_region_factory(pm, [&](std::uint64_t) {
        RegionBuilder b(pm);
        b.entry(pm);
        auto& a = b.as();
        a.mov(64, Gp::rax, ptr(Gp::r15, x_disp(30)));
        a.mov(64, ptr(Gp::r15, x_disp(19)), Gp::rax);
        const Label top = a.bound_label(), continuation = a.new_label(), no_fp = a.new_label();
        // x2 != 0: an inexact divss (IXC, which most guest FP code raises) before each call.
        a.cmp(64, ptr(Gp::r15, x_disp(2)), 0);
        a.jcc(x64::Cond::e, no_fp);
        a.mov_imm(Gp::rax, 0x3f800000);
        a.movd(x64::Xmm::xmm0, Gp::rax);
        a.mov_imm(Gp::rax, 0x40400000);
        a.movd(x64::Xmm::xmm1, Gp::rax);
        a.sse(x64::sse::divss, x64::Xmm::xmm0, x64::Xmm::xmm1);
        a.bind(no_fp);
        b.call(cont, continuation, callee);
        a.bind(continuation);
        b.entry(cont);
        a.alu(Alu::sub, 64, ptr(Gp::r15, x_disp(0)), 1);
        a.jcc(x64::Cond::ne, top);
        a.mov(64, Gp::rax, ptr(Gp::r15, x_disp(19)));
        a.mov(64, ptr(Gp::r15, x_disp(30)), Gp::rax);
        b.return_via_x30();
        b.cover(pm, pm + 12);
        return b.finish();
    });
    GuestContext& ctx = ensure_thread_context();
    ctx.x[0] = 2;
    ctx.x[1] = 0;
    call_guest_prepared(ctx, pm);   // translate and link
    FX_CHECK_EQ(ctx.x[1], 2u);
    constexpr int kCalls = 1'000'000;
    // 0: no FP flag; 1: IXC pending from before the loop (in MXCSR until the
    // first call); 2: the loop raises IXC (an inexact divss) before every call.
    double ns[3] = {1e30, 1e30, 1e30};
    for (int round = 0; round < 3; ++round)
        for (int mode = 0; mode < 3; ++mode)
            ns[mode] = std::min(ns[mode], best_ns(3, [&] {
                ctx.fpsr = mode == 1 ? 0x10u : 0u;
                ctx.x[2] = mode == 2;
                ctx.x[0] = kCalls;
                call_guest_prepared(ctx, pm);
            }) / kCalls);
    FX_CHECK_EQ(ctx.fpsr & 0x9fu, 0x10u);
    ctx.fpsr = 0;
    ctx.x[2] = 0;
    std::printf("  bench host callback round trip (BL -> trampoline -> C++ -> RSB return): %.1f ns; "
                "IXC pending: %.1f ns; divss + call: %.1f ns\n", ns[0], ns[1], ns[2]);
    // The same call as a leaf (register_host_leaf): no adapter, no FP handoff.
    register_host_leaf(callee, [](GuestContext& ctx) { ++ctx.x[1]; return true; }, [](GuestContext&, void*) {}, nullptr);
    double leaf = 1e30;
    for (int round = 0; round < 3; ++round)
        leaf = std::min(leaf, best_ns(3, [&] {
            ctx.x[0] = kCalls;
            call_guest_prepared(ctx, pm);
        }) / kCalls);
    std::printf("  bench host leaf round trip (BL -> leaf entry -> C++ -> RSB return): %.1f ns\n", leaf);
    unregister_host_entry(callee);
    set_region_factory(pm, nullptr);
}
