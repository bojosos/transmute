// Host->guest callbacks: host_callable trampolines and automatic wrapping of
// function-pointer arguments by thunks.

#include "fx/abi/abi_internal.hpp"
#include "fx/abi/tests/abi_test_support.hpp"
#include "fx/testing/test.hpp"
#include "transmute/fx/process.hpp"

#include <windows.h>

#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace transmute::fx;
using namespace transmute::fx::abi_test;

namespace {

std::uint64_t api(const char* dll, const char* name) {
    HMODULE module = GetModuleHandleA(dll);
    if (!module) module = LoadLibraryA(dll);
    auto* p = module ? GetProcAddress(module, name) : nullptr;
    return p ? bind_host_function(dll, name, reinterpret_cast<const void*>(p)) : 0;
}
std::uint64_t typed(const void* native, const char* sig) {
    return abi::bind_host_pointer(native, abi::intern_signature(sig));
}
std::uint64_t u64(const void* p) { return reinterpret_cast<std::uint64_t>(p); }
double d_of(const GuestContext& ctx, unsigned v) { double d; std::memcpy(&d, &ctx.v[v][0], 8); return d; }
float f_of(const GuestContext& ctx, unsigned v) { float f; std::memcpy(&f, &ctx.v[v][0], 4); return f; }
void set_d(GuestContext& ctx, unsigned v, double d) { std::memcpy(&ctx.v[v][0], &d, 8); ctx.v[v][1] = 0; }
void set_f(GuestContext& ctx, unsigned v, float f) { ctx.v[v][0] = 0; std::memcpy(&ctx.v[v][0], &f, 4); ctx.v[v][1] = 0; }

struct S12 { int a, b, c; };
struct V3f { float x, y, z; };

extern "C" {
__declspec(noinline) double native_apply_d(double (*fn)(double, int), double x, int n) { return fn(x, n) + 1.0; }
__declspec(noinline) long long native_apply10(long long (*fn)(long long, long long, long long, long long, long long,
                                                              long long, long long, long long, long long, long long)) {
    return fn(1, 2, 3, 4, 5, 6, 7, 8, 9, 10);
}
__declspec(noinline) V3f native_call_v3(V3f (*fn)(float, int), float x) { return fn(x, 3); }
__declspec(noinline) int native_call_s12(int (*fn)(S12, float), int base) { return fn(S12{base, base + 1, base + 2}, 0.5f); }
__declspec(noinline) int native_compare_ints(const void* a, const void* b) {
    return *static_cast<const int*>(a) - *static_cast<const int*>(b);
}
}

std::unique_ptr<TestThread> g_thread;
GuestContext& ctx() {
    if (!g_thread) g_thread = make_thread();
    return *g_thread->ctx;
}

} // namespace

FX_TEST(callback_qsort_with_guest_comparator) {
    auto& c = ctx();
    // The "guest function": reads two pointers from x0/x1, returns in w0.
    const auto comparator = make_guest_function([](GuestContext& g) {
        const int a = *reinterpret_cast<const int*>(g.x[0]);
        const int b = *reinterpret_cast<const int*>(g.x[1]);
        g.x[0] = static_cast<std::uint64_t>(static_cast<std::int64_t>(a < b ? -1 : a > b ? 1 : 0));
    });
    std::vector<int> values = {42, -7, 19, 0, 3, 3, 1000, -500, 8, 77, 5, 2};
    c.x[0] = u64(values.data());
    c.x[1] = values.size();
    c.x[2] = sizeof(int);
    c.x[3] = comparator;
    run_guest_call(c, api("ucrtbase.dll", "qsort"));
    FX_CHECK(std::is_sorted(values.begin(), values.end()));
    FX_CHECK(guest_call_count(comparator) > 0);
    FX_CHECK_EQ(c.pc, kTestReturnPc);     // x30 survived nested callbacks that clobber ctx.x[30]
    FX_CHECK_EQ(c.x[30], kTestReturnPc);  // and is written back for scrubbed RSB entries

    // bsearch over the sorted array with the same comparator.
    const int key = 77;
    c.x[0] = u64(&key);
    c.x[1] = u64(values.data());
    c.x[2] = values.size();
    c.x[3] = sizeof(int);
    c.x[4] = comparator;
    run_guest_call(c, api("ucrtbase.dll", "bsearch"));
    FX_REQUIRE(c.x[0] != 0);
    FX_CHECK_EQ(*reinterpret_cast<const int*>(c.x[0]), 77);
}

FX_TEST(callback_host_callable_is_cached_per_signature) {
    const auto fn = make_guest_function([](GuestContext& g) { g.x[0] = g.x[0] * 2; });
    const void* a = host_callable(fn, "l(l)");
    const void* b = host_callable(fn, "l(l)");
    const void* d = host_callable(fn, "i(i)");
    FX_CHECK(a != nullptr && a == b);
    FX_CHECK(d != a);
    FX_CHECK(host_callable(0, "l(l)") == nullptr);
    FX_CHECK_EQ(abi::trampoline_guest_function(reinterpret_cast<std::uint64_t>(a)), fn);
    // Called directly from host code: without a current context the
    // trampoline adopts the thread through ensure_thread_context.
    auto* host = reinterpret_cast<long long (*)(long long)>(const_cast<void*>(a));
    const int created = records().contexts_created;
    FX_CHECK_EQ(host(21), 42);
    FX_CHECK(records().contexts_created >= created);
    // Narrow results are converted per signature.
    auto* narrow = reinterpret_cast<int (*)(int)>(const_cast<void*>(host_callable(
        make_guest_function([](GuestContext& g) { g.x[0] = 0xffffffff00000005ULL; }), "i(i)")));
    FX_CHECK_EQ(narrow(1), 5);
}

FX_TEST(callback_floating_point_and_stack_arguments) {
    auto& c = ctx();
    // double fn(double, int): Win64 xmm0/edx -> ARM64 d0/w0; result d0 -> xmm0.
    const auto fn = make_guest_function([](GuestContext& g) {
        double x;
        std::memcpy(&x, &g.v[0][0], 8);
        const auto n = static_cast<std::int32_t>(g.x[0]);
        const double r = x * n;
        std::memcpy(&g.v[0][0], &r, 8);
    });
    set_d(c, 0, 2.5);
    c.x[0] = fn;
    c.x[1] = 4;
    run_guest_call(c, typed(reinterpret_cast<const void*>(&native_apply_d), "d(@d(di)di)"));
    FX_CHECK_EQ(d_of(c, 0), 11.0);

    // Ten integer arguments: x0-x7 plus two slots on the guest stack.
    const auto ten = make_guest_function([](GuestContext& g) {
        const auto* stack = reinterpret_cast<const std::uint64_t*>(g.x[31]);
        std::uint64_t sum = 0;
        for (unsigned n = 0; n < 8; ++n) sum += g.x[n] * (n + 1);
        sum += stack[0] * 9 + stack[1] * 10;
        g.x[0] = sum;
    });
    const auto sp = c.x[31];
    c.x[0] = ten;
    run_guest_call(c, typed(reinterpret_cast<const void*>(&native_apply10), "l(@l(llllllllll))"));
    FX_CHECK_EQ(c.x[0], 385u);
    FX_CHECK_EQ(c.x[31], sp);
}

FX_TEST(callback_aggregates) {
    auto& c = ctx();
    // V3f fn(float, int): Win64 passes a hidden result pointer in rcx, the
    // guest returns s0-s2.
    const auto v3 = make_guest_function([](GuestContext& g) {
        float x;
        std::memcpy(&x, &g.v[0][0], 4);
        const auto n = static_cast<float>(static_cast<std::int32_t>(g.x[0]));
        const float out[3] = {x, x * n, x + n};
        for (unsigned k = 0; k < 3; ++k) { g.v[k][0] = 0; std::memcpy(&g.v[k][0], &out[k], 4); }
    });
    set_f(c, 0, 2.0f);
    c.x[0] = v3;
    run_guest_call(c, typed(reinterpret_cast<const void*>(&native_call_v3), "{f3}(@{f3}(fi)f)"));
    FX_CHECK(f_of(c, 0) == 2.0f && f_of(c, 1) == 6.0f && f_of(c, 2) == 5.0f);

    // int fn(S12, float): Win64 passes S12 by reference, the guest gets x0/x1 + s0.
    const auto s12 = make_guest_function([](GuestContext& g) {
        S12 s;
        std::memcpy(&s, &g.x[0], 8);
        s.c = static_cast<int>(g.x[1]);
        float f;
        std::memcpy(&f, &g.v[0][0], 4);
        g.x[0] = static_cast<std::uint64_t>(s.a * 100 + s.b * 10 + s.c + static_cast<int>(f * 1000));
    });
    c.x[0] = s12;
    c.x[1] = 1;
    run_guest_call(c, typed(reinterpret_cast<const void*>(&native_call_s12), "i(@i({12}f)i)"));
    FX_CHECK_EQ(c.x[0] & 0xffffffff, 100u + 20u + 3u + 500u);
}

FX_TEST(callback_plain_thunks_are_unwrapped) {
    auto& c = ctx();
    // A guest that passes a thunk (e.g. an imported comparator) gives the
    // host the native function itself: no trampoline, no guest calls.
    const auto native_cmp = typed(reinterpret_cast<const void*>(&native_compare_ints), "i(pp)");
    FX_CHECK_EQ(abi::wrap_callback_argument(native_cmp, abi::intern_signature("i(pp)")),
                u64(reinterpret_cast<const void*>(&native_compare_ints)));
    std::vector<int> values = {5, 4, 3, 2, 1};
    c.x[0] = u64(values.data());
    c.x[1] = values.size();
    c.x[2] = sizeof(int);
    c.x[3] = native_cmp;
    run_guest_call(c, api("ucrtbase.dll", "qsort"));
    FX_CHECK(std::is_sorted(values.begin(), values.end()));
    // Sentinels and host pointers pass through unchanged.
    FX_CHECK_EQ(abi::wrap_callback_argument(1, nullptr), 1u);
    FX_CHECK_EQ(abi::wrap_callback_argument(0, nullptr), 0u);
}

FX_TEST(callback_function_pointer_results_are_unwrapped) {
    auto& c = ctx();
    const auto handler = make_guest_function([](GuestContext&) {});
    const auto signal_thunk = api("ucrtbase.dll", "signal");
    c.x[0] = SIGTERM;
    c.x[1] = handler;
    run_guest_call(c, signal_thunk);
    const auto previous = c.x[0];
    // Restore and get our guest handler back (not the host trampoline).
    c.x[0] = SIGTERM;
    c.x[1] = previous;
    run_guest_call(c, signal_thunk);
    FX_CHECK_EQ(c.x[0], handler);
}

FX_TEST(callback_frames_are_described_for_the_unwinder) {
    const auto fn = make_guest_function([](GuestContext&) {});
    const auto* tramp = static_cast<const std::uint8_t*>(host_callable(fn, "v(pp)"));
    AbiCodeFrame frame;
    FX_REQUIRE(abi_code_frame(reinterpret_cast<std::uint64_t>(tramp) + 3, frame));
    FX_CHECK(frame.entered_by_call);
    FX_CHECK_EQ(frame.begin, reinterpret_cast<std::uint64_t>(tramp));
    FX_CHECK_EQ(frame.frame_bytes, 0x58u);
    // The prolog ends right after "sub rsp, 0x58" (48 83 ec 58).
    static constexpr std::uint8_t kSubRsp58[] = {0x48, 0x83, 0xec, 0x58};
    FX_CHECK(std::memcmp(tramp + frame.prolog_bytes - 4, kSubRsp58, 4) == 0);
    FX_CHECK_EQ(tramp[frame.epilog_offset], 0xc3);   // ret
    // A plain thunk: entered by jmp, frame allocated by its prolog.
    const auto thunk = api("kernel32.dll", "CreateFileW");
    FX_REQUIRE(abi_code_frame(thunk, frame));
    FX_CHECK(!frame.entered_by_call);
    FX_CHECK(frame.frame_bytes >= 64 && frame.frame_bytes % 16 == 0);
    const auto* code = reinterpret_cast<const std::uint8_t*>(thunk);
    const auto* end = code + frame.prolog_bytes;
    const bool sub_imm8 = end[-4] == 0x48 && end[-3] == 0x83 && end[-2] == 0xec &&
                          end[-1] == static_cast<std::uint8_t>(frame.frame_bytes);
    std::uint32_t imm32 = 0;
    std::memcpy(&imm32, end - 4, 4);
    const bool sub_imm32 = end[-7] == 0x48 && end[-6] == 0x81 && end[-5] == 0xec && imm32 == frame.frame_bytes;
    FX_CHECK(sub_imm8 || sub_imm32);                   // the prolog ends with sub rsp, frame_bytes
    FX_CHECK(!abi_code_frame(reinterpret_cast<std::uint64_t>(&native_compare_ints), frame));
    // Up to four integer arguments: no frame at all (the entry shadow space is used).
    FX_REQUIRE(abi_code_frame(api("kernel32.dll", "GetCurrentThreadId"), frame));
    FX_CHECK_EQ(frame.frame_bytes, 0u);
    FX_REQUIRE(abi_code_frame(api("kernel32.dll", "lstrlenW"), frame));
    FX_CHECK_EQ(frame.frame_bytes, 0u);
}

FX_TEST(callback_queue_user_work_item_and_apc) {
    auto& c = ctx();
    static HANDLE s_event;
    static volatile LONG s_calls;
    static volatile LONG s_apc;
    s_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    s_calls = 0;
    s_apc = 0;
    // DWORD WINAPI work_item(PVOID)
    const auto work = make_guest_function([](GuestContext& g) {
        InterlockedIncrement(&s_calls);
        FX_CHECK_EQ(g.x[0], 0x4321u);
        SetEvent(s_event);
        g.x[0] = 0;
    });
    // The legacy pool never runs callbacks outside loaded modules (it reports
    // success and drops them); the override routes through a function of
    // this image, so the trampoline still works.
    c.x[0] = work;
    c.x[1] = 0x4321;
    c.x[2] = WT_EXECUTEDEFAULT;
    run_guest_call(c, api("kernel32.dll", "QueueUserWorkItem"));
    FX_CHECK_EQ(c.x[0] & 0xffffffff, 1u);
    FX_CHECK(WaitForSingleObject(s_event, 10000) == WAIT_OBJECT_0);
    FX_CHECK_EQ(s_calls, 1);
    // void CALLBACK apc(ULONG_PTR)
    const auto apc = make_guest_function([](GuestContext& g) {
        InterlockedExchangeAdd(&s_apc, static_cast<LONG>(g.x[0]));
    });
    for (std::uint64_t value : {40u, 2u}) {
        c.x[0] = apc;
        c.x[1] = reinterpret_cast<std::uint64_t>(GetCurrentThread());
        c.x[2] = value;
        run_guest_call(c, api("kernel32.dll", "QueueUserAPC"));
        FX_CHECK(c.x[0] != 0);
    }
    c.x[0] = 0;
    c.x[1] = TRUE;
    run_guest_call(c, api("kernel32.dll", "SleepEx"));
    FX_CHECK_EQ(c.x[0], static_cast<std::uint64_t>(WAIT_IO_COMPLETION));
    FX_CHECK_EQ(s_apc, 42);
    CloseHandle(s_event);
}

FX_TEST(callback_on_host_thread_pool_thread) {
    auto& c = ctx();
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    static HANDLE s_done;
    static DWORD s_thread;
    static std::uint64_t s_context_argument;
    static const GuestContext* s_guest_context;
    s_done = done;
    s_thread = 0;
    s_context_argument = 0;
    s_guest_context = nullptr;
    // PTP_SIMPLE_CALLBACK(PTP_CALLBACK_INSTANCE, PVOID)
    const auto work = make_guest_function([](GuestContext& g) {
        s_thread = GetCurrentThreadId();
        s_context_argument = g.x[1];
        s_guest_context = &g;
        SetEvent(s_done);
    });
    c.x[0] = work;
    c.x[1] = 0x1234;
    c.x[2] = 0;
    run_guest_call(c, api("kernel32.dll", "TrySubmitThreadpoolCallback"));
    FX_CHECK_EQ(c.x[0], 1u);
    FX_CHECK(WaitForSingleObject(done, 10000) == WAIT_OBJECT_0);
    FX_CHECK(s_thread != 0 && s_thread != GetCurrentThreadId());
    FX_CHECK_EQ(s_context_argument, 0x1234u);
    // The pool thread runs on its own (adopted) guest context.
    FX_CHECK(s_guest_context != nullptr && s_guest_context != &c);
    FX_CHECK(records().contexts_created >= 1);
    CloseHandle(done);
}
