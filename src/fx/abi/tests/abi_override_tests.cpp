// Guest-aware overrides: loader, threads, CRT argv/initializers/exit,
// memory protection, processor identity and SEH routing.

#include "fx/abi/abi_internal.hpp"
#include "fx/abi/overrides_internal.hpp"
#include "fx/abi/tests/abi_test_support.hpp"
#include "fx/testing/test.hpp"
#include "transmute/fx/process.hpp"

#include <windows.h>

#include <fstream>
#include <winternl.h>
#include <xmmintrin.h>

#include <atomic>
#include <cfenv>
#include <float.h>
#include <cstring>
#include <string>
#include <vector>

namespace transmute::fx::abi {
std::vector<std::wstring> parse_guest_command_line(const wchar_t* text);
}

using namespace transmute::fx;
using namespace transmute::fx::abi_test;

namespace {

std::uint64_t api(const char* dll, const char* name) {
    HMODULE module = GetModuleHandleA(dll);
    if (!module) module = LoadLibraryA(dll);
    auto* p = module ? GetProcAddress(module, name) : nullptr;
    return bind_host_function(dll, name, reinterpret_cast<const void*>(p));
}
std::uint64_t u64(const void* p) { return reinterpret_cast<std::uint64_t>(p); }

std::unique_ptr<TestThread> g_thread;
GuestContext& ctx() {
    if (!g_thread) g_thread = make_thread();
    return *g_thread->ctx;
}

// Runs `body` on a fresh host thread with its own guest context (for
// overrides that end the thread or the process).
template <typename Body>
DWORD on_worker(Body body) {
    struct Box { Body* body; };
    Box box{&body};
    HANDLE h = CreateThread(nullptr, 0, [](void* p) -> DWORD {
        auto* b = static_cast<Box*>(p);
        (*b->body)();
        return 0xfeed;
    }, &box, 0, nullptr);
    WaitForSingleObject(h, 30000);
    DWORD code = 0;
    GetExitCodeThread(h, &code);
    CloseHandle(h);
    return code;
}

} // namespace

FX_TEST(override_threadpool_environment_retains_guest_module_and_bridges_callbacks) {
    constexpr std::uint64_t guest_base = 0x700010000ull;
    GuestModule module;
    module.base = guest_base;
    module.size = 4096;
    module.name = L"threadpool_guest.dll";
    set_test_modules({module});
    std::atomic<unsigned> calls{0};
    const auto callback = make_guest_function([&](GuestContext& g) {
        FX_CHECK(g.x[0] != 0 && g.x[2] != 0);
        FX_CHECK_EQ(g.x[1], u64(&calls));
        calls.fetch_add(1);
    });
    const auto finalizer = make_guest_function([&](GuestContext& g) {
        FX_CHECK(g.x[0] != 0);
        FX_CHECK_EQ(g.x[1], u64(&calls));
        calls.fetch_add(10);
    });
    TP_CALLBACK_ENVIRON environment;
    InitializeThreadpoolEnvironment(&environment);
    SetThreadpoolCallbackLibrary(&environment, reinterpret_cast<HMODULE>(guest_base));
    environment.FinalizationCallback = reinterpret_cast<PTP_SIMPLE_CALLBACK>(finalizer);
    auto& c = ctx();
    c.x[0] = callback;
    c.x[1] = u64(&calls);
    c.x[2] = u64(&environment);
    run_guest_call(c, api("kernel32.dll", "CreateThreadpoolWork"));
    const auto work = reinterpret_cast<PTP_WORK>(c.x[0]);
    FX_CHECK(work != nullptr);
    if (work) {
        SubmitThreadpoolWork(work);
        WaitForThreadpoolWorkCallbacks(work, FALSE);
        CloseThreadpoolWork(work);
        FX_CHECK_EQ(calls.load(), 11u);
    }
    FX_CHECK_EQ(u64(environment.RaceDll), guest_base);
    FX_CHECK_EQ(u64(reinterpret_cast<const void*>(environment.FinalizationCallback)), finalizer);
    DestroyThreadpoolEnvironment(&environment);
    set_test_modules({});
}

FX_TEST(override_command_line_and_argv) {
    auto& c = ctx();
    run_guest_call(c, api("kernel32.dll", "GetCommandLineW"));
    FX_CHECK(std::wstring(reinterpret_cast<const wchar_t*>(c.x[0])) == guest_command_line());
    run_guest_call(c, api("kernel32.dll", "GetCommandLineA"));
    FX_CHECK_EQ(std::string(reinterpret_cast<const char*>(c.x[0])).size(), guest_command_line().size());

    c.x[0] = 1;
    run_guest_call(c, api("ucrtbase.dll", "_configure_narrow_argv"));
    FX_CHECK_EQ(c.x[0], 0u);
    run_guest_call(c, api("ucrtbase.dll", "__p___argc"));
    const int argc = *reinterpret_cast<const int*>(c.x[0]);
    run_guest_call(c, api("ucrtbase.dll", "__p___argv"));
    char** argv = *reinterpret_cast<char***>(c.x[0]);
    run_guest_call(c, api("ucrtbase.dll", "__p___wargv"));
    wchar_t** wargv = *reinterpret_cast<wchar_t***>(c.x[0]);
    // "C:\guest dir\app.exe" one "two words" a\\\"b "c""d" e\f
    FX_REQUIRE(argc == 6);
    FX_CHECK_EQ(std::string(argv[0]), std::string("C:\\guest dir\\app.exe"));
    FX_CHECK_EQ(std::string(argv[1]), std::string("one"));
    FX_CHECK_EQ(std::string(argv[2]), std::string("two words"));
    FX_CHECK_EQ(std::string(argv[3]), std::string("a\\\"b"));
    FX_CHECK_EQ(std::string(argv[4]), std::string("c\"d"));
    FX_CHECK_EQ(std::string(argv[5]), std::string("e\\f"));
    FX_CHECK(argv[6] == nullptr);
    FX_CHECK(std::wstring(wargv[2]) == L"two words");

    run_guest_call(c, api("ucrtbase.dll", "_get_wide_winmain_command_line"));
    FX_CHECK(std::wstring(reinterpret_cast<const wchar_t*>(c.x[0])) ==
             L"one \"two words\" a\\\\\\\"b \"c\"\"d\" e\\f");
    char* pgm = nullptr;
    c.x[0] = u64(&pgm);
    run_guest_call(c, api("ucrtbase.dll", "_get_pgmptr"));
    FX_CHECK_EQ(std::string(pgm), std::string("C:\\guest dir\\app.exe"));
    // The parser agrees with CommandLineToArgvW on simple input.
    const auto parsed = abi::parse_guest_command_line(L"prog  \"a b\"\tc");
    FX_CHECK(parsed.size() == 3 && parsed[1] == L"a b" && parsed[2] == L"c");
}

FX_TEST(override_module_queries) {
    auto& c = ctx();
    // With a guest main module registered, GetModuleHandle(NULL) and
    // GetModuleFileName(NULL) report the guest image.
    static std::vector<std::uint8_t> image(0x4000, 0);
    GuestModule exe;
    exe.base = u64(image.data());
    exe.size = image.size();
    exe.path = L"C:\\guest dir\\app.exe";
    exe.name = L"app.exe";
    exe.is_dll = false;
    set_test_modules({exe});
    c.x[0] = 0;
    run_guest_call(c, api("kernel32.dll", "GetModuleHandleW"));
    FX_CHECK_EQ(c.x[0], u64(image.data()));

    wchar_t path[MAX_PATH] = {};
    c.x[0] = 0;
    c.x[1] = u64(path);
    c.x[2] = MAX_PATH;
    run_guest_call(c, api("kernel32.dll", "GetModuleFileNameW"));
    FX_CHECK(std::wstring(path) == L"C:\\guest dir\\app.exe");
    FX_CHECK_EQ(c.x[0], 20u);
    // Truncation semantics.
    wchar_t small[8] = {};
    c.x[0] = u64(image.data());
    c.x[1] = u64(small);
    c.x[2] = 8;
    run_guest_call(c, api("kernel32.dll", "GetModuleFileNameW"));
    FX_CHECK_EQ(c.x[0], 8u);
    FX_CHECK(std::wstring(small) == L"C:\\gues");
    FX_CHECK_EQ(*reinterpret_cast<std::uint32_t*>(static_cast<std::uint8_t*>(c.shadow_teb) + 0x68),
                static_cast<std::uint32_t>(ERROR_INSUFFICIENT_BUFFER));
    char narrow[MAX_PATH] = {};
    c.x[0] = 0;
    c.x[1] = u64(narrow);
    c.x[2] = MAX_PATH;
    run_guest_call(c, api("kernel32.dll", "GetModuleFileNameA"));
    FX_CHECK_EQ(std::string(narrow), std::string("C:\\guest dir\\app.exe"));
    // Host modules still go to the host.
    c.x[0] = u64(GetModuleHandleW(L"kernel32.dll"));
    c.x[1] = u64(path);
    c.x[2] = MAX_PATH;
    run_guest_call(c, api("kernel32.dll", "GetModuleFileNameW"));
    FX_CHECK(std::wstring(path).find(L"KERNEL32") != std::wstring::npos ||
             std::wstring(path).find(L"kernel32") != std::wstring::npos);
    // GetModuleHandleExW(FROM_ADDRESS) inside the guest image.
    HMODULE out = nullptr;
    c.x[0] = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
    c.x[1] = u64(image.data() + 0x123);
    c.x[2] = u64(&out);
    run_guest_call(c, api("kernel32.dll", "GetModuleHandleExW"));
    FX_CHECK_EQ(c.x[0], 1u);
    FX_CHECK_EQ(u64(out), u64(image.data()));
    // GetProcAddress goes through the loader, which binds host exports to thunks.
    c.x[0] = u64(GetModuleHandleW(L"kernel32.dll"));
    c.x[1] = u64("GetTickCount");
    run_guest_call(c, api("kernel32.dll", "GetProcAddress"));
    FX_CHECK(is_abi_thunk(c.x[0]));
    set_test_modules({});
}

FX_TEST(override_loader_search_path_and_delay_load) {
    auto& c = ctx();
    reset_records();
    const wchar_t dir[] = L"C:\\guest dir\\plugins";
    c.x[0] = u64(dir);
    run_guest_call(c, api("kernel32.dll", "AddDllDirectory"));
    const auto cookie = c.x[0];
    FX_CHECK_EQ(cookie, 0xc00c1eu);
    c.x[0] = cookie;
    run_guest_call(c, api("kernel32.dll", "RemoveDllDirectory"));
    FX_CHECK_EQ(c.x[0], 1u);
    c.x[0] = u64("C:\\ansi dir");
    run_guest_call(c, api("kernel32.dll", "SetDllDirectoryA"));
    FX_CHECK_EQ(c.x[0], 1u);
    c.x[0] = 0;   // NULL restores the default search order
    run_guest_call(c, api("kernel32.dll", "SetDllDirectoryW"));
    // ResolveDelayLoadedAPI(parent, descriptor, dll_hook, system_hook, thunk, flags)
    for (unsigned n = 0; n < 6; ++n) c.x[n] = 0x100 + n;
    run_guest_call(c, api("kernelbase.dll", "ResolveDelayLoadedAPI"));
    FX_CHECK_EQ(c.x[0], 0xde1a7u);
    FX_CHECK((records().delay_load_args == std::vector<std::uint64_t>{0x100, 0x101, 0x102, 0x103, 0x104, 0x105}));
    // DisableThreadLibraryCalls on a guest module goes to the loader.
    static std::vector<std::uint8_t> image(0x2000, 0);
    GuestModule dll;
    dll.base = u64(image.data());
    dll.size = image.size();
    dll.path = L"C:\\guest dir\\lib.dll";
    dll.name = L"lib.dll";
    dll.is_dll = true;
    set_test_modules({dll});
    c.x[0] = dll.base;
    run_guest_call(c, api("kernel32.dll", "DisableThreadLibraryCalls"));
    FX_CHECK_EQ(c.x[0], 1u);
    set_test_modules({});
    const std::vector<std::string> expected = {"AddDllDirectory C:\\guest dir\\plugins", "RemoveDllDirectory",
                                               "SetDllDirectory C:\\ansi dir", "SetDllDirectory ",
                                               "ResolveDelayLoadedAPI", "DisableThreadLibraryCalls"};
    FX_CHECK(records().loader_calls == expected);
}

FX_TEST(override_create_thread_stack_sizes) {
    auto& c = ctx();
    static std::atomic<int> ran{0};
    ran = 0;
    const auto entry = make_guest_function([](GuestContext& g) {
        ran += static_cast<int>(g.x[0]);
        g.x[0] = 7;
    });
    reset_records();
    set_test_stack_reserve(2ull << 20);
    struct Case { std::uint64_t size; std::uint32_t flags; std::uint64_t reserve; };
    const Case cases[] = {
        {0, 0, 0},                                                      // image default
        {64 * 1024, 0, 0},                                              // commit below the default reserve
        {16ull << 20, 0, 16ull << 20},                                  // CPython: 16 MiB commit
        {(5ull << 20) + 1, 0, 6ull << 20},                              // rounded up to 1 MiB
        {256ull << 20, STACK_SIZE_PARAM_IS_A_RESERVATION, 256ull << 20}, // explicit reservation, never rejected
    };
    for (const auto& k : cases) {
        DWORD tid = 0;
        c.x[0] = 0;
        c.x[1] = k.size;
        c.x[2] = entry;
        c.x[3] = 1;
        c.x[4] = k.flags;
        c.x[5] = u64(&tid);
        run_guest_call(c, api("kernel32.dll", "CreateThread"));
        FX_REQUIRE(c.x[0] != 0);
        WaitForSingleObject(reinterpret_cast<HANDLE>(c.x[0]), 10000);
        DWORD code = 0;
        GetExitCodeThread(reinterpret_cast<HANDLE>(c.x[0]), &code);
        FX_CHECK_EQ(code, 7u);
        CloseHandle(reinterpret_cast<HANDLE>(c.x[0]));
        FX_CHECK(tid != 0);
    }
    FX_REQUIRE(records().threads.size() == std::size(cases));
    for (std::size_t i = 0; i < std::size(cases); ++i) {
        FX_CHECK_EQ(records().threads[i].stack_reserve, cases[i].reserve);
        FX_CHECK_EQ(records().threads[i].entry, entry);
        FX_CHECK_EQ(records().threads[i].creation_flags & STACK_SIZE_PARAM_IS_A_RESERVATION, 0u);
    }
    FX_CHECK_EQ(ran.load(), 5);
    // _beginthreadex(security, stack, start, arg, flags, tid*) uses the same policy.
    unsigned tid = 0;
    c.x[0] = 0;
    c.x[1] = 16ull << 20;
    c.x[2] = entry;
    c.x[3] = 2;
    c.x[4] = 0;
    c.x[5] = u64(&tid);
    run_guest_call(c, api("ucrtbase.dll", "_beginthreadex"));
    FX_REQUIRE(c.x[0] != 0);
    WaitForSingleObject(reinterpret_cast<HANDLE>(c.x[0]), 10000);
    CloseHandle(reinterpret_cast<HANDLE>(c.x[0]));
    FX_CHECK_EQ(records().threads.back().stack_reserve, 16ull << 20);
    // _beginthread(start, stack, arg) returns a handle (-1 on failure).
    c.x[0] = entry;
    c.x[1] = 1ull << 20;
    c.x[2] = 3;
    run_guest_call(c, api("ucrtbase.dll", "_beginthread"));
    FX_CHECK(c.x[0] != 0 && c.x[0] != ~0ull);
    WaitForSingleObject(reinterpret_cast<HANDLE>(c.x[0]), 10000);
    set_test_stack_reserve(0);
}

FX_TEST(override_guest_stack_limits) {
    auto& c = ctx();
    ULONG_PTR low = 0, high = 0;
    c.x[0] = u64(&low);
    c.x[1] = u64(&high);
    run_guest_call(c, api("kernel32.dll", "GetCurrentThreadStackLimits"));
    FX_CHECK_EQ(static_cast<std::uint64_t>(low), c.guest_stack_limit);
    FX_CHECK_EQ(static_cast<std::uint64_t>(high), c.guest_stack_base);
    ULONG guarantee = 64 * 1024;
    c.x[0] = u64(&guarantee);
    run_guest_call(c, api("kernel32.dll", "SetThreadStackGuarantee"));
    FX_CHECK_EQ(c.x[0], 1u);
    FX_CHECK_EQ(guarantee, 0u);                    // previous value
    guarantee = 0;
    c.x[0] = u64(&guarantee);
    run_guest_call(c, api("kernel32.dll", "SetThreadStackGuarantee"));
    FX_CHECK_EQ(guarantee, 64u * 1024);            // query
    guarantee = 64u << 20;                         // larger than the 1 MiB test stack
    c.x[0] = u64(&guarantee);
    run_guest_call(c, api("kernel32.dll", "SetThreadStackGuarantee"));
    FX_CHECK_EQ(c.x[0], 0u);
}

FX_TEST(override_exit_thread_and_initterm) {
    static std::vector<int> order;
    order.clear();
    const auto f1 = make_guest_function([](GuestContext&) { order.push_back(1); });
    const auto f2 = make_guest_function([](GuestContext&) { order.push_back(2); });
    const auto e1 = make_guest_function([](GuestContext& g) { order.push_back(3); g.x[0] = 0; });
    const auto e2 = make_guest_function([](GuestContext& g) { order.push_back(4); g.x[0] = 99; });
    const auto e3 = make_guest_function([](GuestContext& g) { order.push_back(5); g.x[0] = 0; });
    auto& c = ctx();
    std::uint64_t table[] = {f1, 0, f2};
    c.x[0] = u64(table);
    c.x[1] = u64(table + 3);
    run_guest_call(c, api("ucrtbase.dll", "_initterm"));
    std::uint64_t table_e[] = {e1, e2, e3};
    c.x[0] = u64(table_e);
    c.x[1] = u64(table_e + 3);
    run_guest_call(c, api("ucrtbase.dll", "_initterm_e"));
    FX_CHECK_EQ(c.x[0], 99u);
    FX_CHECK((order == std::vector<int>{1, 2, 3, 4}));
    FX_CHECK_EQ(c.pc, kTestReturnPc);

    // ExitThread ends the calling thread through exit_guest_thread.
    const auto code = on_worker([] {
        auto t = make_thread();
        t->ctx->x[0] = 31;
        run_guest_call(*t->ctx, api("kernel32.dll", "ExitThread"));
    });
    FX_CHECK_EQ(code, 31u);
}

FX_TEST(override_onexit_tables_and_exit) {
    static std::vector<int> order;
    order.clear();
    const auto a = make_guest_function([](GuestContext&) { order.push_back(1); });
    const auto b = make_guest_function([](GuestContext&) { order.push_back(2); });
    const auto d = make_guest_function([](GuestContext&) { order.push_back(3); });
    auto& c = ctx();
    // A module-local onexit table runs in reverse registration order.
    struct { void* first; void* last; void* end; } table{};
    c.x[0] = u64(&table);
    run_guest_call(c, api("ucrtbase.dll", "_initialize_onexit_table"));
    for (auto fn : {a, b}) {
        c.x[0] = u64(&table);
        c.x[1] = fn;
        run_guest_call(c, api("ucrtbase.dll", "_register_onexit_function"));
        FX_CHECK_EQ(c.x[0], 0u);
    }
    c.x[0] = u64(&table);
    run_guest_call(c, api("ucrtbase.dll", "_execute_onexit_table"));
    FX_CHECK((order == std::vector<int>{2, 1}));
    FX_CHECK(table.first == nullptr);

    // exit(): guest atexit functions (LIFO), then loader_process_exit.
    order.clear();
    reset_records();
    const auto code = on_worker([&] {
        auto t = make_thread();
        t->ctx->x[0] = a;
        run_guest_call(*t->ctx, api("ucrtbase.dll", "atexit"));
        t->ctx->x[0] = d;
        run_guest_call(*t->ctx, api("ucrtbase.dll", "_crt_atexit"));
        t->ctx->x[0] = 42;
        run_guest_call(*t->ctx, api("ucrtbase.dll", "exit"));
    });
    FX_CHECK_EQ(code, 42u);
    FX_CHECK_EQ(records().process_exits, 1);
    FX_CHECK_EQ(records().last_exit_code, 42u);
    FX_CHECK((order == std::vector<int>{3, 1}));
}

FX_TEST(override_window_procedures_use_trampolines) {
    auto& c = ctx();
    static int messages = 0;
    messages = 0;
    // A guest WNDPROC: counts WM_USER+7 and defers everything else to DefWindowProcW.
    const auto wndproc = make_guest_function([](GuestContext& g) {
        if (static_cast<UINT>(g.x[1]) == WM_USER + 7) {
            ++messages;
            g.x[0] = 0x77 + g.x[2];
            return;
        }
        g.x[0] = static_cast<std::uint64_t>(DefWindowProcW(reinterpret_cast<HWND>(g.x[0]), static_cast<UINT>(g.x[1]),
                                                           static_cast<WPARAM>(g.x[2]), static_cast<LPARAM>(g.x[3])));
    });
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = reinterpret_cast<WNDPROC>(wndproc);    // raw guest address
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"FxAbiTestClass";
    c.x[0] = u64(&wc);
    run_guest_call(c, api("user32.dll", "RegisterClassExW"));
    FX_REQUIRE(c.x[0] != 0);
    FX_CHECK_EQ(u64(reinterpret_cast<void*>(wc.lpfnWndProc)), wndproc);   // the guest's struct is untouched
    // CreateWindowExW: 12 arguments, four on the guest stack.
    c.x[0] = 0;
    c.x[1] = u64(L"FxAbiTestClass");
    c.x[2] = u64(L"fx");
    c.x[3] = WS_OVERLAPPED;
    c.x[4] = 0;
    c.x[5] = 0;
    c.x[6] = 10;
    c.x[7] = 10;
    const auto sp = c.x[31];
    c.x[31] = (sp - 64) & ~std::uint64_t{15};
    auto* stack = reinterpret_cast<std::uint64_t*>(c.x[31]);
    stack[0] = u64(HWND_MESSAGE);
    stack[1] = 0;
    stack[2] = u64(GetModuleHandleW(nullptr));
    stack[3] = 0;
    run_guest_call(c, api("user32.dll", "CreateWindowExW"));
    c.x[31] = sp;
    const auto hwnd = c.x[0];
    FX_REQUIRE(hwnd != 0);
    FX_CHECK(guest_call_count(wndproc) > 0);    // WM_NCCREATE/WM_CREATE reached the guest
    c.x[0] = hwnd;
    c.x[1] = WM_USER + 7;
    c.x[2] = 5;
    c.x[3] = 0;
    run_guest_call(c, api("user32.dll", "SendMessageW"));
    FX_CHECK_EQ(c.x[0], 0x7cu);
    FX_CHECK_EQ(messages, 1);
    // The guest reads back its own procedure, not the trampoline.
    c.x[0] = hwnd;
    c.x[1] = static_cast<std::uint64_t>(GWLP_WNDPROC);
    run_guest_call(c, api("user32.dll", "GetWindowLongPtrW"));
    FX_CHECK_EQ(c.x[0], wndproc);
    // Subclass with a second guest procedure: the previous one comes back unwrapped.
    const auto subclass = make_guest_function([](GuestContext& g) { g.x[0] = 0x5b; });
    c.x[0] = hwnd;
    c.x[1] = static_cast<std::uint64_t>(GWLP_WNDPROC);
    c.x[2] = subclass;
    run_guest_call(c, api("user32.dll", "SetWindowLongPtrW"));
    FX_CHECK_EQ(c.x[0], wndproc);
    c.x[0] = hwnd;
    c.x[1] = WM_USER + 7;
    c.x[2] = 0;
    c.x[3] = 0;
    run_guest_call(c, api("user32.dll", "SendMessageW"));
    FX_CHECK_EQ(c.x[0], 0x5bu);
    c.x[0] = hwnd;
    run_guest_call(c, api("user32.dll", "DestroyWindow"));
    FX_CHECK_EQ(c.x[0], 1u);
    c.x[0] = u64(L"FxAbiTestClass");
    c.x[1] = u64(GetModuleHandleW(nullptr));
    run_guest_call(c, api("user32.dll", "UnregisterClassW"));
}

FX_TEST(override_terminate_process_and_thread_local_exit_callback) {
    // TerminateProcess on our own process goes through loader_process_terminate
    // (no DLL_PROCESS_DETACH, unlike loader_process_exit).
    reset_records();
    auto code = on_worker([] {
        auto t = make_thread();
        t->ctx->x[0] = static_cast<std::uint64_t>(-1);   // GetCurrentProcess()
        t->ctx->x[1] = 9;
        run_guest_call(*t->ctx, api("kernel32.dll", "TerminateProcess"));
    });
    FX_CHECK_EQ(code, 9u);
    FX_CHECK_EQ(records().process_terminations, 1);
    FX_CHECK_EQ(records().process_exits, 0);
    FX_CHECK_EQ(records().last_exit_code, 9u);
    // Another process is terminated by the host.
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    wchar_t command[] = L"cmd.exe /c exit 0";
    FX_REQUIRE(CreateProcessW(nullptr, command, nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
                              nullptr, &si, &pi));
    auto& c = ctx();
    c.x[0] = u64(pi.hProcess);
    c.x[1] = 5;
    run_guest_call(c, api("kernel32.dll", "TerminateProcess"));
    FX_CHECK_EQ(c.x[0], 1u);
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD child_code = 0;
    GetExitCodeProcess(pi.hProcess, &child_code);
    FX_CHECK_EQ(child_code, 5u);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    FX_CHECK_EQ(records().process_terminations, 1);

    // _register_thread_local_exe_atexit_callback: kept by the ABI (the host
    // UCRT accepts one registration per process and transmute-fx owns it);
    // exit() runs it with DLL_PROCESS_DETACH before the atexit table.
    static std::vector<std::string> order;
    order.clear();
    const auto tls_dtor = make_guest_function([](GuestContext& g) {
        order.push_back("tls:" + std::to_string(g.x[1]));
    });
    const auto at_exit = make_guest_function([](GuestContext&) { order.push_back("atexit"); });
    reset_records();
    code = on_worker([&] {
        auto t = make_thread();
        t->ctx->x[0] = tls_dtor;
        run_guest_call(*t->ctx, api("ucrtbase.dll", "_register_thread_local_exe_atexit_callback"));
        t->ctx->x[0] = at_exit;
        run_guest_call(*t->ctx, api("ucrtbase.dll", "atexit"));
        t->ctx->x[0] = 4;
        run_guest_call(*t->ctx, api("ucrtbase.dll", "exit"));
    });
    FX_CHECK_EQ(code, 4u);
    FX_CHECK((order == std::vector<std::string>{"tls:0", "atexit"}));
}

FX_TEST(override_memory_protection) {
    auto& c = ctx();
    reset_records();
    c.x[0] = 0;
    c.x[1] = 0x3000;
    c.x[2] = MEM_COMMIT | MEM_RESERVE;
    c.x[3] = PAGE_EXECUTE_READWRITE;
    run_guest_call(c, api("kernel32.dll", "VirtualAlloc"));
    const auto block = c.x[0];
    FX_REQUIRE(block != 0);
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(reinterpret_cast<void*>(block), &mbi, sizeof(mbi));
    FX_CHECK_EQ(mbi.Protect, static_cast<DWORD>(PAGE_READWRITE));      // never host-executable
    FX_REQUIRE(records().registered_code.size() == 1);
    FX_CHECK_EQ(records().registered_code[0].first, block);
    FX_CHECK_EQ(records().registered_code[0].second, 0x3000u);
    // The guest sees its executable protection.
    MEMORY_BASIC_INFORMATION guest_view{};
    c.x[0] = block;
    c.x[1] = u64(&guest_view);
    c.x[2] = sizeof(guest_view);
    run_guest_call(c, api("kernel32.dll", "VirtualQuery"));
    FX_CHECK_EQ(guest_view.Protect, static_cast<DWORD>(PAGE_EXECUTE_READWRITE));
    // VirtualProtect to RX: host RO, old protection reported as RWX, code invalidated.
    DWORD old = 0;
    c.x[0] = block;
    c.x[1] = 0x1000;
    c.x[2] = PAGE_EXECUTE_READ;
    c.x[3] = u64(&old);
    run_guest_call(c, api("kernel32.dll", "VirtualProtect"));
    FX_CHECK_EQ(c.x[0], 1u);
    FX_CHECK_EQ(old, static_cast<DWORD>(PAGE_EXECUTE_READWRITE));
    VirtualQuery(reinterpret_cast<void*>(block), &mbi, sizeof(mbi));
    FX_CHECK_EQ(mbi.Protect, static_cast<DWORD>(PAGE_READONLY));
    FX_CHECK(!records().invalidated.empty());
    // FlushInstructionCache -> invalidate_guest_code.
    const auto before = records().invalidated.size();
    c.x[0] = static_cast<std::uint64_t>(-1);
    c.x[1] = block + 0x10;
    c.x[2] = 0x40;
    run_guest_call(c, api("kernel32.dll", "FlushInstructionCache"));
    FX_CHECK_EQ(c.x[0], 1u);
    FX_REQUIRE(records().invalidated.size() == before + 1);
    FX_CHECK_EQ(records().invalidated.back().first, block + 0x10);
    // VirtualFree(MEM_RELEASE) invalidates and unregisters the whole allocation.
    c.x[0] = block;
    c.x[1] = 0;
    c.x[2] = MEM_RELEASE;
    run_guest_call(c, api("kernel32.dll", "VirtualFree"));
    FX_CHECK_EQ(c.x[0], 1u);
    FX_CHECK_EQ(records().invalidated.back().first, block);
    FX_CHECK_EQ(records().invalidated.back().second, 0x3000u);
    FX_REQUIRE(!records().unregistered_code.empty());
    FX_CHECK_EQ(records().unregistered_code.back().first, block);
    // Ordinary allocations are untouched.
    const auto invalidations = records().invalidated.size();
    c.x[0] = 0;
    c.x[1] = 0x1000;
    c.x[2] = MEM_COMMIT | MEM_RESERVE;
    c.x[3] = PAGE_READWRITE;
    run_guest_call(c, api("kernel32.dll", "VirtualAlloc"));
    const auto plain = c.x[0];
    c.x[0] = plain;
    c.x[1] = 0;
    c.x[2] = MEM_RELEASE;
    run_guest_call(c, api("kernel32.dll", "VirtualFree"));
    FX_CHECK_EQ(records().invalidated.size(), invalidations);
    FX_CHECK_EQ(records().registered_code.size(), 1u);
}

FX_TEST(override_mapped_executable_views) {
    auto& c = ctx();
    reset_records();
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_EXECUTE_READWRITE, 0, 0x2000, nullptr);
    FX_REQUIRE(mapping != nullptr);
    c.x[0] = u64(mapping);
    c.x[1] = FILE_MAP_READ | FILE_MAP_WRITE | FILE_MAP_EXECUTE;
    c.x[2] = 0;
    c.x[3] = 0;
    c.x[4] = 0x2000;
    run_guest_call(c, api("kernel32.dll", "MapViewOfFile"));
    const auto view = c.x[0];
    FX_REQUIRE(view != 0);
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(reinterpret_cast<void*>(view), &mbi, sizeof(mbi));
    FX_CHECK_EQ(mbi.Protect, static_cast<DWORD>(PAGE_READWRITE));
    FX_CHECK_EQ(records().registered_code.size(), 1u);
    c.x[0] = view;
    run_guest_call(c, api("kernel32.dll", "UnmapViewOfFile"));
    FX_CHECK_EQ(c.x[0], 1u);
    FX_CHECK_EQ(records().unregistered_code.size(), 1u);
    CloseHandle(mapping);
}

FX_TEST(override_processor_identity) {
    auto& c = ctx();
    const auto present = api("kernel32.dll", "IsProcessorFeaturePresent");
    const struct { DWORD feature; bool expected; } features[] = {
        {PF_ARM_NEON_INSTRUCTIONS_AVAILABLE, true},
        {PF_ARM_V8_INSTRUCTIONS_AVAILABLE, true},
        {PF_ARM_V81_ATOMIC_INSTRUCTIONS_AVAILABLE, true},
        {PF_ARM_V8_CRC32_INSTRUCTIONS_AVAILABLE, true},
        {PF_ARM_V83_LRCPC_INSTRUCTIONS_AVAILABLE, true},
        {PF_FASTFAIL_AVAILABLE, true},
        {PF_XMMI64_INSTRUCTIONS_AVAILABLE, false},
        {PF_AVX2_INSTRUCTIONS_AVAILABLE, false},
        {PF_RDTSC_INSTRUCTION_AVAILABLE, false},
        {PF_ARM_SVE_INSTRUCTIONS_AVAILABLE, false},
    };
    for (const auto& f : features) {
        c.x[0] = f.feature;
        run_guest_call(c, present);
        FX_CHECK_EQ(c.x[0] != 0, f.expected);
    }
    SYSTEM_INFO info{};
    c.x[0] = u64(&info);
    run_guest_call(c, api("kernel32.dll", "GetNativeSystemInfo"));
    FX_CHECK_EQ(info.wProcessorArchitecture, static_cast<WORD>(PROCESSOR_ARCHITECTURE_ARM64));
    FX_CHECK(info.dwNumberOfProcessors >= 1);
    FX_CHECK_EQ(info.dwPageSize, 4096u);
    c.x[0] = u64(&info);
    run_guest_call(c, api("kernel32.dll", "GetSystemInfo"));
    FX_CHECK_EQ(info.wProcessorArchitecture, static_cast<WORD>(PROCESSOR_ARCHITECTURE_ARM64));
    USHORT process_machine = 0xffff, native_machine = 0;
    c.x[0] = static_cast<std::uint64_t>(-1);
    c.x[1] = u64(&process_machine);
    c.x[2] = u64(&native_machine);
    run_guest_call(c, api("kernel32.dll", "IsWow64Process2"));
    FX_CHECK_EQ(c.x[0], 1u);
    FX_CHECK_EQ(process_machine, static_cast<USHORT>(IMAGE_FILE_MACHINE_UNKNOWN));
    FX_CHECK_EQ(native_machine, static_cast<USHORT>(IMAGE_FILE_MACHINE_ARM64));
}

FX_TEST(override_seh_routing) {
    auto& c = ctx();
    reset_records();
    // RtlCaptureContext sees ctx.pc == the guest return address.
    std::uint64_t record[64] = {};
    c.x[0] = u64(record);
    run_guest_call(c, api("kernel32.dll", "RtlCaptureContext"));
    FX_CHECK_EQ(record[0], kTestReturnPc);
    FX_CHECK_EQ(c.pc, kTestReturnPc);
    // RaiseException continues at ctx.pc (the caller here).
    c.x[0] = 0xe0000001;
    c.x[1] = 0;
    c.x[2] = 0;
    c.x[3] = 0;
    run_guest_call(c, api("kernel32.dll", "RaiseException"));
    FX_CHECK_EQ(c.pc, kTestReturnPc);
    // longjmp resumes wherever the seh component says.
    std::uint64_t jump_buffer[32] = {0x0000'5555'0000'1000ULL};
    c.x[0] = u64(jump_buffer);
    c.x[1] = 0;
    run_guest_call(c, api("ucrtbase.dll", "longjmp"));
    FX_CHECK_EQ(c.pc, 0x0000'5555'0000'1000ULL);
    FX_CHECK_EQ(c.x[0], 1u);
    // Table/handler APIs return to the caller with the seh result.
    run_guest_call(c, api("kernel32.dll", "SetUnhandledExceptionFilter"));
    FX_CHECK_EQ(c.x[0], 0x5e40u);
    run_guest_call(c, api("kernel32.dll", "AddVectoredExceptionHandler"));
    run_guest_call(c, api("kernel32.dll", "RemoveVectoredContinueHandler"));
    run_guest_call(c, api("ntdll.dll", "RtlAddFunctionTable"));
    run_guest_call(c, api("ntdll.dll", "RtlVirtualUnwind"));
    const std::vector<std::string> expected = {
        "RtlCaptureContext", "RaiseException", "longjmp", "SetUnhandledExceptionFilter",
        "AddVectoredExceptionHandler", "RemoveVectoredExceptionHandler", "RtlAddFunctionTable", "RtlVirtualUnwind"};
    FX_CHECK(records().seh_calls == expected);
}

FX_TEST(override_fenv_syncs_fpcr) {
    auto& c = ctx();
    const unsigned saved = _mm_getcsr();
    c.fpcr = 0;
    // _controlfp(_RC_UP, _MCW_RC) -> FPCR.RMode = RP (01), FZ unchanged.
    c.x[0] = _RC_UP;
    c.x[1] = _MCW_RC;
    run_guest_call(c, api("ucrtbase.dll", "_controlfp"));
    FX_CHECK_EQ((c.fpcr >> 22) & 3, 1u);
    // fesetround(FE_TOWARDZERO) -> RZ (11).
    c.x[0] = FE_TOWARDZERO;
    run_guest_call(c, api("ucrtbase.dll", "fesetround"));
    FX_CHECK_EQ(c.x[0], 0u);
    FX_CHECK_EQ((c.fpcr >> 22) & 3, 3u);
    // _controlfp(_DN_FLUSH, _MCW_DN) -> FZ.
    c.x[0] = _DN_FLUSH;
    c.x[1] = _MCW_DN;
    run_guest_call(c, api("ucrtbase.dll", "_controlfp"));
    FX_CHECK((c.fpcr >> 24) & 1);
    _mm_setcsr(saved);
    c.fpcr = 0;
}

// Flags folded into ctx.fpsr at a dispatcher exit are part of the
// architectural FPSR (ctx.fpsr | MXCSR flags): the host CRT must see them,
// and a host clear must not come back when the next region entry reloads
// MXCSR from ctx.fpsr.
// The override thunk's FP handoff (src/fx/runtime/fp_boundary.hpp): the
// handler sees the architectural FPSR in ctx.fpsr (the flags guest code left
// in MXCSR folded in); afterwards MXCSR = state(ctx.fpcr, ctx.fpsr), flags
// the host raised in MXCSR dropped, and the mirrored bits are in MXCSR only.
// RtlCaptureContext, RtlRestoreContext, longjmp and RaiseException use this
// thunk.
namespace {
struct FpProbe {
    std::uint32_t seen_fpsr = 0;
    std::uint32_t clear = 0, set = 0, raise_in_mxcsr = 0;
    bool change_fpcr = false;
    std::uint32_t fpcr = 0;
};
FpProbe g_fp_probe;
abi::OverrideResult o_fp_probe(GuestContext& c, const abi::OverrideBinding&) {
    auto& p = g_fp_probe;
    p.seen_fpsr = c.fpsr;
    c.fpsr = (c.fpsr & ~p.clear) | p.set;
    if (p.change_fpcr) c.fpcr = p.fpcr;
    if (p.raise_in_mxcsr) _mm_setcsr(_mm_getcsr() | p.raise_in_mxcsr);   // the host CRT's view
    return abi::OverrideResult::return_to_caller;
}
} // namespace

FX_TEST(override_thunk_fp_handoff) {
    auto& c = ctx();
    const unsigned saved = _mm_getcsr();
    static abi::OverrideBinding binding{};
    const auto bytes = abi::generate_override_thunk(&o_fp_probe, &binding);
    auto* thunk = allocate_code(bytes.size(), 16);
    std::memcpy(thunk, bytes.data(), bytes.size());
    const auto code = u64(thunk);

    // Guest code left ZE|IE in MXCSR; IDC lives in ctx.fpsr. The handler
    // clears everything (feclearexcept + IDC).
    g_fp_probe = {};
    g_fp_probe.clear = 0x9f;
    c.fpcr = 0;
    c.fpsr = 0x80;
    _mm_setcsr(0x1f80u | 0x04u | 0x01u);
    run_guest_call(c, code);
    FX_CHECK_EQ(c.pc, kTestReturnPc);
    FX_CHECK_EQ(g_fp_probe.seen_fpsr, 0x83u);   // IDC DZC IOC
    FX_CHECK_EQ(c.fpsr, 0u);
    FX_CHECK_EQ(_mm_getcsr(), 0x1f80u);

    // The handler raises OFC in ctx.fpsr and sets FPCR = RZ | FZ.
    g_fp_probe = {};
    g_fp_probe.set = 0x4;
    g_fp_probe.change_fpcr = true;
    g_fp_probe.fpcr = (3u << 22) | (1u << 24);
    c.fpsr = 0;
    _mm_setcsr(0x1f80u | 0x20u);   // PE from guest code
    run_guest_call(c, code);
    FX_CHECK_EQ(g_fp_probe.seen_fpsr, 0x10u);   // IXC
    FX_CHECK_EQ(c.fpsr, 0u);
    FX_CHECK_EQ(_mm_getcsr(), 0x1f80u | 0x6000u | 0x8040u | 0x08u | 0x20u);   // RZ, FTZ|DAZ, OE, PE

    // Flags the host's own arithmetic raised in MXCSR are not guest state
    // (DE, which has no FPSR bit, is left alone).
    g_fp_probe = {};
    g_fp_probe.raise_in_mxcsr = 0x02u | 0x10u;   // DE (no FPSR bit), UE
    c.fpcr = 0;
    c.fpsr = 0;
    _mm_setcsr(0x1f80u);
    run_guest_call(c, code);
    FX_CHECK_EQ(_mm_getcsr(), 0x1f80u | 0x02u);
    FX_CHECK_EQ(c.fpsr, 0u);
    _mm_setcsr(saved);
    c.fpcr = 0;
    c.fpsr = 0;
}

FX_TEST(override_fenv_flags_include_context_fpsr) {
    auto& c = ctx();
    const unsigned saved = _mm_getcsr();
    _mm_setcsr(0x1f80);
    c.fpcr = 0;
    c.fpsr = 0x2;   // DZC, held only in ctx.fpsr
    c.x[0] = FE_DIVBYZERO | FE_INVALID;
    run_guest_call(c, api("ucrtbase.dll", "fetestexcept"));
    FX_CHECK_EQ(c.x[0], static_cast<std::uint64_t>(FE_DIVBYZERO));
    c.x[0] = FE_ALL_EXCEPT;
    run_guest_call(c, api("ucrtbase.dll", "feclearexcept"));
    FX_CHECK_EQ(c.fpsr & 0x1fu, 0u);
    FX_CHECK_EQ(_mm_getcsr() & 0x3fu, 0u);
    // A dispatcher round trip: region entry loads MXCSR = control | flags(ctx.fpsr).
    _mm_setcsr(0x1f80u | (c.fpsr & 1u) | ((c.fpsr & 0x1eu) << 1));
    c.x[0] = FE_DIVBYZERO | FE_INVALID;
    run_guest_call(c, api("ucrtbase.dll", "fetestexcept"));
    FX_CHECK_EQ(c.x[0], 0u);
    // fegetenv captures them too.
    c.fpsr = 0x1;   // IOC
    fenv_t env{};
    c.x[0] = u64(&env);
    run_guest_call(c, api("ucrtbase.dll", "fegetenv"));
    FX_CHECK((env._Fe_stat & FE_INVALID) != 0);
    // Control-word changes keep the flags: an FPCR write leaves FPSR alone.
    c.x[0] = _DN_FLUSH;
    c.x[1] = _MCW_DN;
    run_guest_call(c, api("ucrtbase.dll", "_control87"));
    FX_CHECK((c.fpcr >> 24) & 1);
    FX_CHECK_EQ(_mm_getcsr() & 0x3fu, 0x1u);   // IE
    _mm_setcsr(saved);
    c.fpcr = 0;
    c.fpsr = 0;
}

namespace {
struct GlCall { unsigned a = 0, b = 0; int c = 0, d = 0; float f = 0, g = 0, h = 0, k = 0; };
GlCall g_gl;
void WINAPI fake_sampler_parameterf(unsigned sampler, unsigned pname, float value) { g_gl = {sampler, pname, 0, 0, value}; }
void WINAPI fake_clear_bufferfi(unsigned buffer, int drawbuffer, float depth, int stencil) {
    g_gl = {buffer, 0, drawbuffer, stencil, depth};
}
void WINAPI fake_vertex_attrib4f(unsigned index, float x, float y, float z, float w) { g_gl = {index, 0, 0, 0, x, y, z, w}; }
void WINAPI fake_unlisted() {}
void* WINAPI fake_wgl_get_proc_address(const char* name) {
    if (!std::strcmp(name, "glSamplerParameterf")) return reinterpret_cast<void*>(&fake_sampler_parameterf);
    if (!std::strcmp(name, "glClearBufferfi")) return reinterpret_cast<void*>(&fake_clear_bufferfi);
    if (!std::strcmp(name, "glVertexAttrib4f")) return reinterpret_cast<void*>(&fake_vertex_attrib4f);
    if (!std::strcmp(name, "glFakeVendorThingEXT")) return reinterpret_cast<void*>(&fake_unlisted);
    if (!std::strcmp(name, "glSentinel")) return reinterpret_cast<void*>(std::uintptr_t{2});
    return nullptr;
}
void set_float(GuestContext& c, unsigned v, float f) {
    std::uint32_t bits;
    std::memcpy(&bits, &f, 4);
    c.v[v][0] = bits;
    c.v[v][1] = 0;
}
} // namespace

// GL entry points come back from the driver without export names. AAPCS64
// numbers integer and FP arguments separately, Win64 positionally, so a
// positional binding would hand glSamplerParameterf garbage from v2.
FX_TEST(override_wgl_get_proc_address_types_gl_entry_points) {
    auto& c = ctx();
    const auto get = bind_host_function("opengl32.dll", "wglGetProcAddress",
                                        reinterpret_cast<const void*>(&fake_wgl_get_proc_address));
    const auto resolve = [&](const char* name) {
        c.x[0] = u64(name);
        run_guest_call(c, get);
        return c.x[0];
    };
    const auto sampler = resolve("glSamplerParameterf");
    FX_REQUIRE(sampler != 0);
    FX_CHECK(sampler != u64(reinterpret_cast<void*>(&fake_sampler_parameterf)));
    FX_CHECK_EQ(resolve("glSamplerParameterf"), sampler);   // cached
    c.x[0] = 7;
    c.x[1] = 0x84fe;   // GL_TEXTURE_MAX_ANISOTROPY
    set_float(c, 0, 16.0f);
    set_float(c, 2, -1.0f);
    run_guest_call(c, sampler);
    FX_CHECK_EQ(g_gl.a, 7u);
    FX_CHECK_EQ(g_gl.b, 0x84feu);
    FX_CHECK_EQ(g_gl.f, 16.0f);

    const auto clear = resolve("glClearBufferfi");
    c.x[0] = 0x84f9;   // GL_DEPTH_STENCIL
    c.x[1] = 0;
    c.x[2] = 0xff;
    set_float(c, 0, 1.0f);
    set_float(c, 2, -3.0f);
    run_guest_call(c, clear);
    FX_CHECK_EQ(g_gl.a, 0x84f9u);
    FX_CHECK_EQ(g_gl.c, 0);
    FX_CHECK_EQ(g_gl.d, 0xff);
    FX_CHECK_EQ(g_gl.f, 1.0f);

    // Fifth Win64 position is on the host stack.
    const auto attrib = resolve("glVertexAttrib4f");
    c.x[0] = 3;
    for (unsigned i = 0; i < 4; ++i) set_float(c, i, 0.25f * static_cast<float>(i + 1));
    run_guest_call(c, attrib);
    FX_CHECK_EQ(g_gl.a, 3u);
    FX_CHECK_EQ(g_gl.f, 0.25f);
    FX_CHECK_EQ(g_gl.g, 0.5f);
    FX_CHECK_EQ(g_gl.h, 0.75f);
    FX_CHECK_EQ(g_gl.k, 1.0f);

    // Unknown names, failures and ICD sentinels pass through unchanged.
    FX_CHECK_EQ(resolve("glFakeVendorThingEXT"), u64(reinterpret_cast<void*>(&fake_unlisted)));
    FX_CHECK_EQ(resolve("glNotThere"), 0u);
    FX_CHECK_EQ(resolve("glSentinel"), 2u);
}

namespace {
std::wstring process_command_line(HANDLE process) {
    using QueryFn = LONG(NTAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);
    const auto query = reinterpret_cast<QueryFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationProcess"));
    std::vector<std::uint8_t> buffer(8192);
    ULONG size = 0;
    if (!query || query(process, 60 /* ProcessCommandLineInformation */, buffer.data(), static_cast<ULONG>(buffer.size()), &size) < 0)
        return {};
    const auto* text = reinterpret_cast<const UNICODE_STRING*>(buffer.data());
    return std::wstring(text->Buffer, text->Length / sizeof(wchar_t));
}
} // namespace

// An x64 host cannot start an ARM64 image; the override starts this runtime
// with the image as its guest instead. Other targets are left alone.
FX_TEST(override_create_process_runs_arm64_images_under_the_runtime) {
    auto& c = ctx();
    const auto create = api("kernel32.dll", "CreateProcessW");
    const auto run = [&](const wchar_t* app, wchar_t* cmd, DWORD flags, STARTUPINFOW& si, PROCESS_INFORMATION& pi) {
        c.x[0] = u64(app);
        c.x[1] = u64(cmd);
        c.x[2] = c.x[3] = 0;
        c.x[4] = FALSE;
        c.x[5] = flags;
        c.x[6] = c.x[7] = 0;
        const auto sp = c.x[31];
        c.x[31] = (sp - 16) & ~std::uint64_t{15};
        auto* stack = reinterpret_cast<std::uint64_t*>(c.x[31]);
        stack[0] = u64(&si);
        stack[1] = u64(&pi);
        run_guest_call(c, create);
        c.x[31] = sp;
        return c.x[0];
    };

    // Native target: unchanged, including the exit code.
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    std::wstring native_cmd = L"cmd.exe /c exit 7";
    FX_REQUIRE(run(nullptr, native_cmd.data(), CREATE_NO_WINDOW, si, pi) == TRUE);
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    FX_CHECK_EQ(code, 7u);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    // A file with ARM64 PE headers, started suspended and terminated unrun.
    wchar_t dir[MAX_PATH], image[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    GetTempFileNameW(dir, L"fxa", 0, image);
    std::uint8_t headers[512] = {};
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(headers);
    dos->e_magic = IMAGE_DOS_SIGNATURE;
    dos->e_lfanew = 0x80;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(headers + 0x80);
    nt->Signature = IMAGE_NT_SIGNATURE;
    nt->FileHeader.Machine = IMAGE_FILE_MACHINE_ARM64;
    HANDLE file = CreateFileW(image, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    FX_REQUIRE(file != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    WriteFile(file, headers, sizeof(headers), &written, nullptr);
    CloseHandle(file);
    wchar_t self[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH * 2);

    for (const bool with_app : {false, true}) {
        std::wstring cmd = L"\"" + std::wstring(image) + L"\" a \"b c\"";
        pi = {};
        FX_REQUIRE(run(with_app ? image : nullptr, cmd.data(), CREATE_SUSPENDED | CREATE_NO_WINDOW, si, pi) == TRUE);
        wchar_t started[MAX_PATH * 2] = {};
        DWORD length = MAX_PATH * 2;
        QueryFullProcessImageNameW(pi.hProcess, 0, started, &length);
        const std::wstring line = process_command_line(pi.hProcess);
        TerminateProcess(pi.hProcess, 0);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        FX_CHECK(_wcsicmp(started, self) == 0);
        FX_CHECK(line == L"\"" + std::wstring(self) + L"\" -- \"" + image + L"\" a \"b c\"");
    }
    DeleteFileW(image);
}

// The guest environment reports ARM64 (set by the loader); x64 children get
// the host's processor variables back, inherited or in an explicit block.
FX_TEST(override_create_process_gives_host_children_the_host_processor) {
    auto& c = ctx();
    const auto create = api("kernel32.dll", "CreateProcessW");
    wchar_t saved[64] = {};
    const DWORD had = GetEnvironmentVariableW(L"PROCESSOR_ARCHITECTURE", saved, 64);
    SetEnvironmentVariableW(L"PROCESSOR_ARCHITECTURE", L"ARM64");
    wchar_t dir[MAX_PATH], out[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    GetTempFileNameW(dir, L"fxe", 0, out);
    const auto child_sees = [&](const void* environment, DWORD flags) {
        std::wstring cmd = L"cmd.exe /c echo %PROCESSOR_ARCHITECTURE%> \"" + std::wstring(out) + L"\"";
        STARTUPINFOW si{sizeof(si)};
        PROCESS_INFORMATION pi{};
        c.x[0] = 0;
        c.x[1] = u64(cmd.data());
        c.x[2] = c.x[3] = 0;
        c.x[4] = FALSE;
        c.x[5] = CREATE_NO_WINDOW | flags;
        c.x[6] = u64(environment);
        c.x[7] = 0;
        const auto sp = c.x[31];
        c.x[31] = (sp - 16) & ~std::uint64_t{15};
        auto* stack = reinterpret_cast<std::uint64_t*>(c.x[31]);
        stack[0] = u64(&si);
        stack[1] = u64(&pi);
        run_guest_call(c, create);
        c.x[31] = sp;
        if (c.x[0] != TRUE) return std::string("<failed>");
        WaitForSingleObject(pi.hProcess, 30000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        std::ifstream in(out);
        std::string line;
        std::getline(in, line);
        while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) line.pop_back();
        return line;
    };
    FX_CHECK_EQ(child_sees(nullptr, 0), std::string("AMD64"));
    static const wchar_t wide[] = L"PROCESSOR_ARCHITECTURE=ARM64\0SystemRoot=C:\\Windows\0FXTEST=1\0";
    FX_CHECK_EQ(child_sees(wide, CREATE_UNICODE_ENVIRONMENT), std::string("AMD64"));
    static const char narrow[] = "processor_architecture=ARM64\0SystemRoot=C:\\Windows\0";
    FX_CHECK_EQ(child_sees(narrow, 0), std::string("AMD64"));
    static const char missing[] = "SystemRoot=C:\\Windows\0";   // added when absent
    FX_CHECK_EQ(child_sees(missing, 0), std::string("AMD64"));
    DeleteFileW(out);
    SetEnvironmentVariableW(L"PROCESSOR_ARCHITECTURE", had ? saved : nullptr);
}
