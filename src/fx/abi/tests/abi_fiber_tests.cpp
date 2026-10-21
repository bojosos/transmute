// Guest fibers: ConvertThreadToFiber / CreateFiber / SwitchToFiber /
// DeleteFiber / ConvertFiberToThread, with the guest's inline
// GetCurrentFiber()/GetFiberData() reading the shadow TEB, per-fiber guest
// stacks and per-fiber (host) FLS.

#include "fx/abi/abi_internal.hpp"
#include "fx/abi/tests/abi_test_support.hpp"
#include "fx/testing/test.hpp"
#include "transmute/fx/process.hpp"

#include <windows.h>

#include <cstring>
#include <string>

using namespace transmute::fx;
using namespace transmute::fx::abi_test;

namespace {

std::uint64_t api(const char* name) {
    HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
    return bind_host_function("kernel32.dll", name, reinterpret_cast<const void*>(GetProcAddress(k32, name)));
}

// What the guest's inline GetCurrentFiber() / GetFiberData() compute.
std::uint64_t guest_current_fiber(const GuestContext& ctx) {
    std::uint64_t fiber;
    std::memcpy(&fiber, static_cast<const std::uint8_t*>(ctx.shadow_teb) + 0x20, 8);
    return fiber;
}
std::uint64_t guest_fiber_data(const GuestContext& ctx) {
    return *reinterpret_cast<const std::uint64_t*>(guest_current_fiber(ctx));
}

std::uint64_t call(GuestContext& ctx, const char* name, std::uint64_t a = 0, std::uint64_t b = 0, std::uint64_t c = 0) {
    ctx.x[0] = a;
    ctx.x[1] = b;
    ctx.x[2] = c;
    run_guest_call(ctx, api(name));
    return ctx.x[0];
}

std::string g_trace;
std::uint64_t g_main_fiber;
std::uint64_t g_workers[2];
DWORD g_fls;
volatile LONG g_fls_callbacks;

} // namespace

FX_TEST(fiber_ping_pong_with_fls_and_stacks) {
    auto thread = make_thread();
    auto& c = *thread->ctx;
    g_trace.clear();
    g_fls_callbacks = 0;
    reset_records();

    const auto cleanup = make_guest_function([](GuestContext& g) {
        if (g.x[0] != 0) InterlockedIncrement(&g_fls_callbacks);
    });
    g_fls = static_cast<DWORD>(call(c, "FlsAlloc", cleanup));
    FX_REQUIRE(g_fls != FLS_OUT_OF_INDEXES);

    FX_CHECK_EQ(call(c, "IsThreadAFiber"), 0u);
    g_main_fiber = call(c, "ConvertThreadToFiber", 99);
    FX_REQUIRE(g_main_fiber != 0);
    FX_CHECK_EQ(call(c, "IsThreadAFiber"), 1u);
    FX_CHECK_EQ(guest_current_fiber(c), g_main_fiber);
    FX_CHECK_EQ(guest_fiber_data(c), 99u);
    call(c, "FlsSetValue", g_fls, 50);

    // void WINAPI fiber_proc(void* id): three rounds of ping-pong, keeping a
    // counter on its own guest stack.
    const auto proc = make_guest_function([](GuestContext& g) {
        const auto id = g.x[0];
        auto* local = reinterpret_cast<std::uint64_t*>(g.x[31] - 64);   // "stack-resident" state
        *local = id * 100;
        const auto stack_base = g.guest_stack_base;
        call(g, "FlsSetValue", g_fls, id + 1);
        for (int round = 0; round < 3; ++round) {
            g_trace += "f" + std::to_string(id) + ":" + std::to_string(*local) + ":" +
                       std::to_string(guest_fiber_data(g)) + ";";
            *local += 1;
            FX_CHECK_EQ(g.guest_stack_base, stack_base);   // our own stack after every resume
            call(g, "SwitchToFiber", id == 0 ? g_workers[1] : g_main_fiber);
        }
        g_trace += "f" + std::to_string(id) + "-done:" + std::to_string(call(g, "FlsGetValue", g_fls) - 1) + ";";
        call(g, "SwitchToFiber", g_main_fiber);
        FX_CHECK(false);   // never resumed again
    });
    g_workers[0] = call(c, "CreateFiber", 64 * 1024, proc, 0);
    g_workers[1] = call(c, "CreateFiber", 0, proc, 1);
    FX_REQUIRE(g_workers[0] != 0 && g_workers[1] != 0);
    FX_CHECK_EQ(records().guest_stacks_allocated, 2);
    const auto main_stack = c.guest_stack_base;
    const auto main_sp = c.x[31];
    c.x[19] = 0x1919;   // a callee-saved register must survive the switches
    for (int round = 0; round < 3; ++round) {
        g_trace += "m;";
        call(c, "SwitchToFiber", g_workers[0]);
        FX_CHECK_EQ(guest_current_fiber(c), g_main_fiber);
        FX_CHECK_EQ(c.guest_stack_base, main_stack);
        FX_CHECK_EQ(c.x[31], main_sp);
        FX_CHECK_EQ(c.x[19], 0x1919u);
    }
    g_trace += "m-final;";
    call(c, "SwitchToFiber", g_workers[0]);
    call(c, "SwitchToFiber", g_workers[1]);
    FX_CHECK_EQ(g_trace, std::string("m;f0:0:0;f1:100:1;m;f0:1:0;f1:101:1;m;f0:2:0;f1:102:1;m-final;f0-done:0;f1-done:1;"));
    FX_CHECK_EQ(call(c, "FlsGetValue", g_fls), 50u);          // the main fiber's own FLS value
    FX_CHECK_EQ(guest_fiber_data(c), 99u);

    call(c, "DeleteFiber", g_workers[0]);
    call(c, "DeleteFiber", g_workers[1]);
    FX_CHECK_EQ(g_fls_callbacks, 2);                           // each fiber's FLS value was cleaned up
    FX_CHECK_EQ(records().guest_stacks_freed, 2);
    FX_CHECK_EQ(call(c, "ConvertFiberToThread"), 1u);
    FX_CHECK_EQ(call(c, "IsThreadAFiber"), 0u);
    call(c, "FlsFree", g_fls);
}
