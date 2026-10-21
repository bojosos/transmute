// Guest threads: creation, shadow TEB, guest stacks (size, guard, overflow),
// exit, foreign host threads and concurrent execution/invalidation.

#include "fx/runtime/tests/test_support.hpp"
#include "fx/testing/test.hpp"

#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

using namespace rtt;
namespace rt = transmute::fx::rt;

namespace {

void init() { runtime_initialize(RuntimeOptions{}); }

constexpr std::uint32_t kRet = 0xd65f03c0;

DWORD run_thread(const GuestThreadStart& start, DWORD timeout = 10000) {
    std::uint32_t id = 0;
    const auto handle = reinterpret_cast<HANDLE>(create_guest_thread(start, &id));
    if (!handle) return 0xFFFFFFFFu;
    if (WaitForSingleObject(handle, timeout) != WAIT_OBJECT_0) {
        CloseHandle(handle);
        return 0xFFFFFFFEu;
    }
    DWORD code = 0;
    GetExitCodeThread(handle, &code);
    CloseHandle(handle);
    return code;
}

void host_exit_thread(std::uint64_t code) { exit_guest_thread(static_cast<std::uint32_t>(code)); }

} // namespace

FX_TEST(guest_thread_shadow_teb_and_stack) {
    init();
    GuestCode code({
        0xaa1203e1,   // mov x1, x18
        0xf9000001,   // str x1, [x0]
        0xf9400642,   // ldr x2, [x18, #8]     StackBase
        0xf9000402,   // str x2, [x0, #8]
        0xf9401a42,   // ldr x2, [x18, #0x30]  Self
        0xf9000802,   // str x2, [x0, #16]
        0x910003e2,   // mov x2, sp
        0xf9000c02,   // str x2, [x0, #24]
        0xf9403242,   // ldr x2, [x18, #0x60]  PEB
        0xf9001002,   // str x2, [x0, #32]
        0xf9400a42,   // ldr x2, [x18, #16]    StackLimit
        0xf9001402,   // str x2, [x0, #40]
        0xd2800540,   // movz x0, #42
        kRet,
    });
    std::uint64_t results[6] = {};
    const int attach = g_attach_calls.load(), detach = g_detach_calls.load();
    GuestThreadStart start{code.at(0), reinterpret_cast<std::uint64_t>(results), 0, 0};
    FX_CHECK_EQ(run_thread(start), 42u);
    FX_CHECK(results[0] != 0);
    FX_CHECK_EQ(results[0], results[2]);                         // x18 == TEB.Self
    FX_CHECK(results[3] <= results[1] && results[3] > results[5]);   // limit < sp <= base
    FX_CHECK_EQ(results[1] - results[5], std::uint64_t{1} << 20);    // default reserve
    FX_CHECK_EQ(results[1] % 16, 0u);
    const auto peb = *reinterpret_cast<const std::uint64_t*>(reinterpret_cast<const std::uint8_t*>(NtCurrentTeb()) + 0x60);
    FX_CHECK_EQ(results[4], peb);
    FX_CHECK(results[0] != reinterpret_cast<std::uint64_t>(NtCurrentTeb()));
    FX_CHECK_EQ(g_attach_calls.load(), attach + 1);
    FX_CHECK_EQ(g_detach_calls.load(), detach + 1);
}

FX_TEST(guest_thread_stack_reserve_is_configurable) {
    init();
    GuestCode code({
        0xf9400641,   // ldr x1, [x18, #8]    StackBase
        0xf9400a42,   // ldr x2, [x18, #16]   StackLimit
        0xf900005f,   // str xzr, [x2]        lowest usable byte is committed
        0xcb020020,   // sub x0, x1, x2
        kRet,
    });
    constexpr std::uint64_t k16M = std::uint64_t{16} << 20;
    GuestThreadStart big{code.at(0), 0, k16M, STACK_SIZE_PARAM_IS_A_RESERVATION};
    FX_CHECK_EQ(run_thread(big), k16M);
    GuestThreadStart commit_size{code.at(0), 0, 3u << 20, 0};        // commit size: rounded up to 1 MiB
    FX_CHECK_EQ(run_thread(commit_size), 3u << 20);
    GuestThreadStart small{code.at(0), 0, 0x30000, STACK_SIZE_PARAM_IS_A_RESERVATION};
    FX_CHECK_EQ(run_thread(small), 0x30000u);
}

FX_TEST(guest_stack_overflow_raises_stack_overflow) {
    init();
    clear_exception_log();
    GuestCode code({
        0xd14007ff,   // loop: sub sp, sp, #0x1000
        0xf90003ff,   //       str xzr, [sp]
        0x17fffffe,   //       b loop
    });
    std::uint64_t handler_limit = 0, handler_sp = 0;
    set_exception_handler([&](GuestContext& ctx, const GuestExceptionRecord& record) {
        if (record.code != 0xC00000FDu) return;
        handler_limit = ctx.guest_stack_limit;
        handler_sp = ctx.x[31];
        // The handler has emergency stack below the old limit.
        *reinterpret_cast<volatile std::uint64_t*>(ctx.guest_stack_limit) = 1;
        ctx.x[0] = 0xfd;
        ctx.pc = kReturnToHostPc;
    });
    GuestThreadStart start{code.at(0), 0, 0x40000, STACK_SIZE_PARAM_IS_A_RESERVATION};
    FX_CHECK_EQ(run_thread(start), 0xfdu);
    set_exception_handler(nullptr);
    const auto log = exception_log();
    FX_REQUIRE(log.size() == 1);
    FX_CHECK_EQ(log[0].code, 0xC00000FDu);
    FX_CHECK_EQ(log[0].address, code.at(1));
    FX_CHECK(handler_limit != 0 && handler_limit < handler_sp);
}

FX_TEST(guest_thread_create_suspended) {
    init();
    GuestCode code({0xd2800ee0 /* movz x0, #0x77 */, kRet});
    GuestThreadStart start{code.at(0), 0, 0, CREATE_SUSPENDED};
    std::uint32_t id = 0;
    const auto handle = reinterpret_cast<HANDLE>(create_guest_thread(start, &id));
    FX_REQUIRE(handle != nullptr);
    FX_CHECK(id != 0);
    FX_CHECK_EQ(WaitForSingleObject(handle, 50), static_cast<DWORD>(WAIT_TIMEOUT));
    ResumeThread(handle);
    FX_CHECK_EQ(WaitForSingleObject(handle, 10000), static_cast<DWORD>(WAIT_OBJECT_0));
    DWORD exit_code = 0;
    GetExitCodeThread(handle, &exit_code);
    FX_CHECK_EQ(exit_code, 0x77u);
    CloseHandle(handle);
}

FX_TEST(exit_guest_thread_from_host_call) {
    init();
    GuestCode code({
        0xf9400008,   // ldr x8, [x0]
        0xf9400400,   // ldr x0, [x0, #8]
        0xd63f0100,   // blr x8  -> host_exit_thread(x0)
        0xd2800000,   // movz x0, #0 (not reached)
        kRet,
    });
    const std::uint64_t block[2] = {reinterpret_cast<std::uint64_t>(&host_exit_thread), 1234};
    const int detach = g_detach_calls.load();
    GuestThreadStart start{code.at(0), reinterpret_cast<std::uint64_t>(block), 0, 0};
    FX_CHECK_EQ(run_thread(start), 1234u);
    FX_CHECK_EQ(g_detach_calls.load(), detach + 1);
}

FX_TEST(foreign_thread_context_attaches_and_detaches) {
    init();
    GuestCode code({0x8b010000 /* add x0, x0, x1 */, kRet});
    const int attach = g_attach_calls.load(), detach = g_detach_calls.load();
    std::uint64_t result = 0;
    GuestContext* seen = nullptr;
    std::thread([&] {
        const std::uint64_t args[] = {20, 22};
        result = call_guest(code.at(0), args);
        seen = current_context();
    }).join();
    FX_CHECK_EQ(result, 42u);
    FX_CHECK(seen != nullptr);
    FX_CHECK_EQ(g_attach_calls.load(), attach + 1);
    FX_CHECK_EQ(g_detach_calls.load(), detach + 1);   // FLS callback at thread exit
}

FX_TEST(parallel_guest_threads) {
    init();
    GuestCode code({
        0xaa0003e2,   // mov x2, x0
        0xd2800000,   // movz x0, #0
        0x8b020000,   // loop: add x0, x0, x2
        0xd1000442,   // sub x2, x2, #1
        0xb5ffffc2,   // cbnz x2, loop
        kRet,
    });
    constexpr int kThreads = 8;
    std::vector<HANDLE> handles;
    for (int i = 0; i < kThreads; ++i) {
        GuestThreadStart start{code.at(0), static_cast<std::uint64_t>(100 + i), 0, 0};
        handles.push_back(reinterpret_cast<HANDLE>(create_guest_thread(start, nullptr)));
        FX_REQUIRE(handles.back() != nullptr);
    }
    WaitForMultipleObjects(kThreads, handles.data(), TRUE, 20000);
    for (int i = 0; i < kThreads; ++i) {
        DWORD exit_code = 0;
        GetExitCodeThread(handles[i], &exit_code);
        const std::uint64_t n = 100 + i;
        FX_CHECK_EQ(exit_code, n * (n + 1) / 2);
        CloseHandle(handles[i]);
    }
}

FX_TEST(concurrent_execution_and_invalidation) {
    init();
    GuestCode code({});
    const auto pa = code.at(0), pb = code.at(16);
    std::atomic<int> builds{0};
    set_region_factory(pa, [&](std::uint64_t) {
        ++builds;
        RegionBuilder b(pa);
        b.entry(pa);
        b.as().alu(x64::Alu::add, 64, ptr(Gp::r15, x_disp(0)), 1);
        b.direct_exit(pb);
        return b.finish();
    });
    set_region_factory(pb, [&](std::uint64_t) {
        ++builds;
        RegionBuilder b(pb);
        b.entry(pb);
        b.as().alu(x64::Alu::add, 64, ptr(Gp::r15, x_disp(0)), 10);
        b.return_via_x30();
        return b.finish();
    });
    const auto s0 = runtime_stats();
    std::atomic<bool> stop{false};
    std::atomic<int> errors{0};
    std::atomic<std::uint64_t> calls{0};
    std::atomic<int> running{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < 6; ++t) {
        workers.emplace_back([&] {
            const std::uint64_t args[] = {0};
            if (call_guest(pa, args) != 11) ++errors;
            ++running;
            while (!stop.load()) {
                if (call_guest(pa, args) != 11) ++errors;
                ++calls;
            }
        });
    }
    // Start invalidating only once every worker is executing guest code.
    const auto deadline = GetTickCount64() + 10000;
    while (running.load() < 6 && GetTickCount64() < deadline) SwitchToThread();
    const auto calls_before = calls.load();
    std::uint64_t invalidations = 0;
    for (int i = 0; (i < 200 || calls.load() - calls_before < 5000) && GetTickCount64() < deadline; ++i) {
        invalidate_guest_code(i % 2 ? pa : pb, 4);
        ++invalidations;
    }
    stop = true;
    for (auto& w : workers) w.join();
    FX_CHECK_EQ(errors.load(), 0);
    FX_CHECK(calls.load() >= 5000);
    FX_CHECK(invalidations >= 200);
    const std::uint64_t args[] = {0};
    FX_CHECK_EQ(call_guest(pa, args), 11u);
    FX_CHECK(builds.load() > 2);
    FX_CHECK(runtime_stats().invalidated_regions - s0.invalidated_regions > 0);
    set_region_factory(pa, nullptr);
    set_region_factory(pb, nullptr);
}

namespace {
std::atomic<HANDLE> g_seen_actctx{nullptr};
std::uint64_t record_activation_context() {
    HANDLE current = nullptr;
    GetCurrentActCtx(&current);
    g_seen_actctx.store(current);
    if (current) ReleaseActCtx(current);
    return 7;
}
} // namespace

FX_TEST(guest_threads_activate_the_guest_activation_context) {
    init();
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    GetTempFileNameW(dir, L"fxm", 0, path);
    const char manifest[] =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<assembly xmlns=\"urn:schemas-microsoft-com:asm.v1\" manifestVersion=\"1.0\">"
        "<dependency><dependentAssembly><assemblyIdentity type=\"win32\" name=\"Microsoft.Windows.Common-Controls\""
        " version=\"6.0.0.0\" processorArchitecture=\"*\" publicKeyToken=\"6595b64144ccf1df\" language=\"*\"/>"
        "</dependentAssembly></dependency></assembly>";
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    FX_REQUIRE(file != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    WriteFile(file, manifest, sizeof(manifest) - 1, &written, nullptr);
    CloseHandle(file);
    ACTCTXW desc{};
    desc.cbSize = sizeof(desc);
    desc.lpSource = path;
    HANDLE actctx = CreateActCtxW(&desc);
    DeleteFileW(path);
    FX_REQUIRE(actctx != INVALID_HANDLE_VALUE);
    set_activation_context(actctx);

    // The guest tail-calls a host function that records the active context.
    GuestCode code({0xf9400008 /* ldr x8, [x0] */, 0xd61f0100 /* br x8 */});
    const std::uint64_t block[1] = {reinterpret_cast<std::uint64_t>(&record_activation_context)};
    const std::uint64_t args[1] = {reinterpret_cast<std::uint64_t>(block)};

    // Borrowed host thread: active while guest code runs, popped on return, so
    // the thread goes back to its owner with a balanced activation stack.
    HANDLE after = nullptr;
    std::uint64_t result = 0;
    g_seen_actctx = nullptr;
    std::thread([&] {
        result = call_guest(code.at(0), args);
        GetCurrentActCtx(&after);
        if (after) ReleaseActCtx(after);
    }).join();
    FX_CHECK_EQ(result, 7u);
    FX_CHECK(g_seen_actctx.load() == actctx);
    FX_CHECK(after != actctx);

    // A thread-pool worker fail-fasts with STATUS_SXS_INVALID_DEACTIVATION if a
    // callback returns with an extra activation frame (msvcp140's
    // SubmitThreadpoolWork path, used by OpenAL's WASAPI backend).
    struct PoolCall { std::uint64_t entry; const std::uint64_t* args; std::uint64_t result; };
    PoolCall pool{code.at(0), args, 0};
    g_seen_actctx = nullptr;
    PTP_WORK work = CreateThreadpoolWork([](PTP_CALLBACK_INSTANCE, void* p, PTP_WORK) {
        auto* call = static_cast<PoolCall*>(p);
        call->result = call_guest(call->entry, std::span<const std::uint64_t>(call->args, 1));
    }, &pool, nullptr);
    FX_REQUIRE(work != nullptr);
    for (int i = 0; i < 3; ++i) {
        SubmitThreadpoolWork(work);
        WaitForThreadpoolWorkCallbacks(work, FALSE);
    }
    CloseThreadpoolWork(work);
    FX_CHECK_EQ(pool.result, 7u);
    FX_CHECK(g_seen_actctx.load() == actctx);

    // Guest thread: active for the thread's lifetime.
    GuestThreadStart start{code.at(0), reinterpret_cast<std::uint64_t>(block), 0, 0};
    g_seen_actctx = nullptr;
    FX_CHECK_EQ(run_thread(start), 7u);
    FX_CHECK(g_seen_actctx.load() == actctx);
    set_activation_context(nullptr);
    ReleaseActCtx(actctx);
}

namespace {
// What a guest-aware _resetstkoflw does: a PAGE_GUARD region right below the
// guest SP's page.
std::uint64_t arm_guard_below(std::uint64_t sp) {
    const auto page = sp & ~std::uint64_t{0xfff};
    DWORD old = 0;
    VirtualProtect(reinterpret_cast<void*>(page - 0x2000), 0x2000, PAGE_READWRITE | PAGE_GUARD, &old);
    return 0;
}
} // namespace

FX_TEST(guest_stack_guard_pages_grow_then_overflow) {
    init();
    clear_exception_log();
    GuestCode code({
        0xf9400008,   // ldr x8, [x0]
        0xa9bf7bfd,   // stp x29, x30, [sp, #-16]!
        0x910003e0,   // mov x0, sp
        0xd63f0100,   // blr x8 -> arm_guard_below(sp)
        0xd14007ff,   // loop: sub sp, sp, #0x1000
        0xf90003ff,   //       str xzr, [sp]
        0x17fffffe,   //       b loop
    });
    std::uint64_t overflow_sp = 0, limit = 0, base = 0;
    set_exception_handler([&](GuestContext& ctx, const GuestExceptionRecord& record) {
        if (record.code != 0xC00000FDu) return;
        overflow_sp = ctx.x[31];
        limit = ctx.guest_stack_limit;
        base = ctx.guest_stack_base;
        ctx.x[0] = 0xfd;
        ctx.pc = kReturnToHostPc;
    });
    const std::uint64_t block[1] = {reinterpret_cast<std::uint64_t>(&arm_guard_below)};
    GuestThreadStart start{code.at(0), reinterpret_cast<std::uint64_t>(block), 0x40000, 0};
    FX_CHECK_EQ(run_thread(start), 0xfdu);
    set_exception_handler(nullptr);
    const auto log = exception_log();
    FX_REQUIRE(log.size() == 1);                     // guard hits were silent growth
    FX_CHECK_EQ(log[0].code, 0xC00000FDu);
    FX_CHECK_EQ(log[0].address, code.at(5));
    // The guard walked down the whole stack: the overflow is at the guard
    // region, with emergency space committed below for the handler.
    FX_CHECK(base - overflow_sp >= 0x40000 - 0x2000);
    FX_CHECK(limit < overflow_sp);
}

// _resetstkoflw on a guest stack (seh's guest__resetstkoflw): after a handled
// overflow, rearm_guest_stack_guard restores the guard region so a second
// overflow again raises STATUS_STACK_OVERFLOW with emergency space.
FX_TEST(guest_stack_guard_rearmed_after_overflow) {
    init();
    clear_exception_log();
    GuestCode code({
        0xd14007ff,   // loop: sub sp, sp, #0x1000
        0xf90003ff,   //       str xzr, [sp]
        0x17fffffe,   //       b loop
    });
    int overflows = 0;
    bool rearm_inside_region = true, rearm_after_reset = false;
    std::uint64_t first_sp = 0, second_sp = 0, second_limit = 0, base = 0;
    set_exception_handler([&](GuestContext& ctx, const GuestExceptionRecord& record) {
        if (record.code != 0xC00000FDu) return;
        if (++overflows == 1) {
            first_sp = ctx.x[31];
            base = ctx.guest_stack_base;
            rearm_inside_region = rearm_guest_stack_guard(ctx);   // SP in the guard region: fails
            ctx.x[31] = base - 0x100;                              // the handler unwound to the top
            rearm_after_reset = rearm_guest_stack_guard(ctx);
            ctx.pc = code.at(0);
            return;
        }
        second_sp = ctx.x[31];
        second_limit = ctx.guest_stack_limit;
        ctx.x[0] = 0xfd;
        ctx.pc = kReturnToHostPc;
    });
    GuestThreadStart start{code.at(0), 0, 0x40000, 0};
    FX_CHECK_EQ(run_thread(start), 0xfdu);
    set_exception_handler(nullptr);
    FX_CHECK_EQ(overflows, 2);
    FX_CHECK(!rearm_inside_region);
    FX_CHECK(rearm_after_reset);
    // Both overflows hit the guard region (not the hard bottom page), and the
    // second one again has emergency space committed below it.
    FX_CHECK(base - first_sp >= 0x40000 - 0x1000);
    FX_CHECK(base - second_sp >= 0x40000 - 0x1000);
    FX_CHECK(second_sp + 0x1000 > first_sp && first_sp + 0x1000 > second_sp);   // same guard page
    FX_CHECK(second_limit < second_sp);
}

FX_TEST(switch_guest_stack_for_fibers) {
    init();
    clear_exception_log();
    GuestContext& ctx = ensure_thread_context();
    GuestCode bounds({
        0xf9400641,   // ldr x1, [x18, #8]    StackBase
        0xf9400a42,   // ldr x2, [x18, #16]   StackLimit
        0xcb020020,   // sub x0, x1, x2
        kRet,
    });
    GuestCode overflow({0xd14007ff /* sub sp, sp, #0x1000 */, 0xf90003ff /* str xzr, [sp] */, 0x17fffffe /* b */});
    const auto own_base = ctx.guest_stack_base;
    const auto saved_sp = ctx.x[31];
    GuestStack fiber = allocate_guest_stack(0x20000);
    const GuestStack previous = switch_guest_stack(fiber);
    FX_CHECK_EQ(previous.base, own_base);
    ctx.x[31] = fiber.base;
    FX_CHECK_EQ(call_guest(bounds.at(0), {}), 0x20000u);
    FX_CHECK_EQ(ctx.guest_stack_base, fiber.base);
    set_exception_handler([](GuestContext& c, const GuestExceptionRecord& record) {
        if (record.code == 0xC00000FDu) c.pc = kReturnToHostPc;
    });
    call_guest(overflow.at(0), {});
    set_exception_handler(nullptr);
    const auto log = exception_log();
    FX_REQUIRE(log.size() == 1);
    FX_CHECK_EQ(log[0].code, 0xC00000FDu);
    const GuestStack after = switch_guest_stack(previous);   // back to the thread's stack
    FX_CHECK(after.overflowed);
    FX_CHECK(after.limit < fiber.guard_end);
    FX_CHECK_EQ(ctx.guest_stack_base, own_base);
    ctx.x[31] = saved_sp;
    FX_CHECK_EQ(call_guest(bounds.at(0), {}), own_base - previous.limit);
    free_guest_stack(after);
}

FX_TEST(guest_fls_callbacks_at_thread_exit_reuse_the_thread_state) {
    init();
    std::uint64_t flag = 0;
    GuestCode code({
        0xa9bf7bfd,   //  0 stp x29, x30, [sp, #-16]!
        0xa9bf53f3,   //  1 stp x19, x20, [sp, #-16]!
        0xaa0003f3,   //  2 mov x19, x0            block
        0xf9400268,   //  3 ldr x8, [x19]          FlsAlloc
        0xf9400a60,   //  4 ldr x0, [x19, #16]     guest FLS callback
        0xd63f0100,   //  5 blr x8                 index = FlsAlloc(callback)
        0xaa0003f4,   //  6 mov x20, x0
        0xf9400668,   //  7 ldr x8, [x19, #8]      FlsSetValue
        0xaa1403e0,   //  8 mov x0, x20
        0xd2800ee1,   //  9 movz x1, #0x77
        0xd63f0100,   // 10 blr x8                 FlsSetValue(index, 0x77)
        0xaa1403e0,   // 11 mov x0, x20
        0xa8c153f3,   // 12 ldp x19, x20, [sp], #16
        0xa8c17bfd,   // 13 ldp x29, x30, [sp], #16
        kRet,         // 14
        0x58000061,   // 15 callback: ldr x1, =&flag (literal at 18)
        0xf9000020,   // 16 str x0, [x1]
        kRet,         // 17
        0, 0,         // 18-19 &flag
    });
    const auto flag_address = reinterpret_cast<std::uint64_t>(&flag);
    std::memcpy(code.words() + 18, &flag_address, 8);
    const std::uint64_t block[3] = {reinterpret_cast<std::uint64_t>(&FlsAlloc),
                                    reinterpret_cast<std::uint64_t>(&FlsSetValue), code.at(15)};
    const auto states = rt::live_thread_states();
    const int attach = g_attach_calls.load(), detach = g_detach_calls.load();
    GuestThreadStart start{code.at(0), reinterpret_cast<std::uint64_t>(block), 0, 0};
    const DWORD index = run_thread(start);
    // ntdll ran the guest FLS callback at thread exit (via an execute fault),
    // on the thread's existing state, before the state was torn down.
    FX_CHECK_EQ(flag, 0x77u);
    FX_CHECK_EQ(rt::live_thread_states(), states);
    FX_CHECK_EQ(g_attach_calls.load(), attach + 1);
    FX_CHECK_EQ(g_detach_calls.load(), detach + 1);
    FlsFree(index);
}
