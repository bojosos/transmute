// Static TLS, thread attach/detach, process exit, launch and stack sizes.

#include "test_support.hpp"

#include "fx/loader/command_line.hpp"
#include "fx/loader/loader_internal.hpp"
#include "fx/testing/test.hpp"

#include <windows.h>

#include <condition_variable>
#include <cstdio>
#include <string>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>

using namespace transmute::fx;
using namespace transmute::fx::loader;
using namespace fxl_test;

namespace {

#define FXL_SKIP_UNLESS(path)                                                      \
    do {                                                                           \
        if (!exists(path)) {                                                       \
            std::printf("  skipped: %ls not built\n", std::wstring(path).c_str()); \
            return;                                                                \
        }                                                                          \
    } while (0)

std::uint64_t proc(std::uint64_t module, const char* name) { return guest_get_proc_address(module, name); }

std::vector<unsigned long> tls_events(std::uint64_t module) {
    const auto events = reinterpret_cast<const volatile unsigned long*>(proc(module, "g_events"));
    const auto count = reinterpret_cast<const volatile long*>(proc(module, "g_event_count"));
    std::vector<unsigned long> out;
    if (!events || !count) return out;
    for (long i = 0; i < *count; ++i) {
        const unsigned long value = events[i];
        out.push_back(value);
    }
    return out;
}

void** tls_array(GuestContext& ctx) {
    return *reinterpret_cast<void***>(static_cast<std::uint8_t*>(ctx.shadow_teb) + 0x58);
}

IMAGE_TLS_DIRECTORY64 tls_directory(std::uint64_t base) {
    PeHeaders h;
    std::string error;
    parse_pe_headers({reinterpret_cast<const std::uint8_t*>(base), 4096}, h, error);
    IMAGE_TLS_DIRECTORY64 dir{};
    std::memcpy(&dir, reinterpret_cast<const void*>(base + h.dir(dir_tls).rva), sizeof(dir));
    return dir;
}

std::uint64_t load_tls_dll() {
    LoaderConfig config;
    config.application_path = fixture(L"fxl_importer.exe");
    loader_configure(config);
    return guest_load_library(L"fxl_tls.dll", 0);
}

// Runs this test binary again with --filter <test> and FXL_CHILD=<mode>.
// Returns the exit code; `output` receives the child's stdout.
DWORD run_child(const char* filter, const wchar_t* mode, std::string& output,
                const wchar_t* extra_env_name = nullptr, const wchar_t* extra_env_value = nullptr) {
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring command = L"\"" + std::wstring(self) + L"\" --filter " +
                           std::wstring(filter, filter + std::strlen(filter));
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    CreatePipe(&read, &write, &sa, 0);
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    SetEnvironmentVariableW(L"FXL_CHILD", mode);
    SetEnvironmentVariableW(L"FXL_CHILD_REPORT", L"1");
    if (extra_env_name) SetEnvironmentVariableW(extra_env_name, extra_env_value);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    SetEnvironmentVariableW(L"FXL_CHILD", nullptr);
    SetEnvironmentVariableW(L"FXL_CHILD_REPORT", nullptr);
    if (extra_env_name) SetEnvironmentVariableW(extra_env_name, nullptr);
    CloseHandle(write);
    if (!ok) {
        CloseHandle(read);
        return 0xffffffffu;
    }
    // Children print little (well under the pipe buffer): wait first, so a
    // hung child (the deadlock this guards against) is killed, not waited on.
    DWORD code = 0;
    if (WaitForSingleObject(pi.hProcess, 30000) != WAIT_OBJECT_0) {
        TerminateProcess(pi.hProcess, 0xdead);
        WaitForSingleObject(pi.hProcess, 5000);
        output += "[child timed out]";
    }
    GetExitCodeProcess(pi.hProcess, &code);
    char buffer[4096];
    DWORD got = 0;
    DWORD available = 0;
    while (PeekNamedPipe(read, nullptr, 0, nullptr, &available, nullptr) && available &&
           ReadFile(read, buffer, sizeof(buffer), &got, nullptr) && got) {
        output.append(buffer, got);
    }
    CloseHandle(read);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return code;
}

std::wstring child_mode() {
    wchar_t mode[64] = {};
    const DWORD n = GetEnvironmentVariableW(L"FXL_CHILD", mode, 64);
    return n ? std::wstring(mode, n) : std::wstring{};
}

} // namespace

FX_TEST(tls_index_template_and_zero_fill) {
    FXL_SKIP_UNLESS(fixture(L"fxl_tls.dll"));
    fresh_loader();
    const std::uint64_t h = load_tls_dll();
    FX_REQUIRE(h);
    const GuestModule* m = find_guest_module(h);
    FX_REQUIRE(m);
    FX_CHECK_EQ(m->tls_index, 0u);
    const auto dir = tls_directory(h);
    FX_CHECK_EQ(*reinterpret_cast<const std::uint32_t*>(dir.AddressOfIndex), 0u);
    FX_CHECK_EQ(dir.SizeOfZeroFill, 32u);
    GuestContext& ctx = ensure_thread_context();
    void** array = tls_array(ctx);
    FX_REQUIRE(array);
    auto* block = static_cast<const std::uint8_t*>(array[m->tls_index]);
    FX_REQUIRE(block);
    const auto* templ = reinterpret_cast<const std::uint8_t*>(dir.StartAddressOfRawData);
    const std::size_t size = dir.EndAddressOfRawData - dir.StartAddressOfRawData;
    // The template holds tls_value = 1234; the block is a copy plus zero fill.
    std::size_t value_at = SIZE_MAX;
    for (std::size_t i = 0; i + 4 <= size; ++i) {
        if (*reinterpret_cast<const std::int32_t*>(templ + i) == 1234) value_at = i;
    }
    FX_REQUIRE(value_at != SIZE_MAX);
    FX_CHECK_EQ(*reinterpret_cast<const std::int32_t*>(block + value_at), 1234);
    bool zero = true;
    for (std::size_t i = 0; i < 32; ++i) zero &= block[size + i] == 0;
    FX_CHECK(zero);
    FX_CHECK_EQ(run_guest(proc(h, "tls_get_value")), 1234u);
    FX_CHECK_EQ(run_guest(proc(h, "tls_get_counter")), 1u);   // the PROCESS_ATTACH callback ran here
    const auto events = tls_events(h);
    FX_REQUIRE(events.size() == 2);
    FX_CHECK_EQ(events[0], 0x101ul);
    FX_CHECK_EQ(events[1], 0x201ul);
}

FX_TEST(thread_attach_and_detach_build_private_tls) {
    FXL_SKIP_UNLESS(fixture(L"fxl_tls.dll"));
    fresh_loader();
    const std::uint64_t h = load_tls_dll();
    FX_REQUIRE(h);
    const auto get = proc(h, "tls_get_value");
    const auto set = proc(h, "tls_set_value");
    const auto counter = proc(h, "tls_get_counter");
    std::uint64_t seen_value = 0, seen_after_set = 0, seen_counter = 0;
    bool had_array = false;
    std::thread worker([&] {
        GuestContext& ctx = ensure_thread_context();   // DLL_THREAD_ATTACH happens here
        had_array = tls_array(ctx) != nullptr && tls_array(ctx)[0] != nullptr;
        seen_value = run_guest(get);
        run_guest(set, {5});
        seen_after_set = run_guest(get);
        seen_counter = run_guest(counter);
        end_thread_context();   // DLL_THREAD_DETACH
    });
    worker.join();
    FX_CHECK(had_array);
    FX_CHECK_EQ(seen_value, 1234u);
    FX_CHECK_EQ(seen_after_set, 5u);
    FX_CHECK_EQ(seen_counter, 1u);
    FX_CHECK_EQ(run_guest(get), 1234u);   // the main thread's copy is untouched
    const auto events = tls_events(h);
    const std::vector<unsigned long> expected = {0x101, 0x201, 0x102, 0x202, 0x103, 0x203};
    FX_CHECK(events == expected);
    // DisableThreadLibraryCalls suppresses DllMain but not TLS callbacks.
    FX_CHECK(guest_disable_thread_library_calls(h));
    std::thread second([&] {
        ensure_thread_context();
        end_thread_context();
    });
    second.join();
    const auto after = tls_events(h);
    FX_REQUIRE(after.size() == 8);
    FX_CHECK_EQ(after[6], 0x102ul);
    FX_CHECK_EQ(after[7], 0x103ul);
}

// A thread that already got DLL_THREAD_DETACH can still run guest code while
// it exits (ntdll calls guest FLS callbacks after the runtime tore the guest
// thread down, and the callback gets a fresh context). It must get static TLS
// but no second DLL_THREAD_ATTACH / DLL_THREAD_DETACH (corpus "tls": thread_local
// constructors ran twice per std::thread worker).
FX_TEST(no_thread_notifications_after_detach_on_the_same_thread) {
    FXL_SKIP_UNLESS(fixture(L"fxl_tls.dll"));
    fresh_loader();
    const std::uint64_t h = load_tls_dll();
    FX_REQUIRE(h);
    std::uint64_t value_after_detach = 0, counter_after_detach = 99;
    bool tls_present = false;
    std::thread worker([&] {
        ensure_thread_context();
        run_guest(proc(h, "tls_set_value"), {5});
        end_thread_context();                       // DLL_THREAD_DETACH
        GuestContext& again = ensure_thread_context();   // re-entry while "exiting"
        tls_present = tls_array(again) && tls_array(again)[0];
        value_after_detach = run_guest(proc(h, "tls_get_value"));
        counter_after_detach = run_guest(proc(h, "tls_get_counter"));
        end_thread_context();
    });
    worker.join();
    FX_CHECK(tls_present);
    FX_CHECK_EQ(value_after_detach, 1234u);   // fresh template copy
    FX_CHECK_EQ(counter_after_detach, 0u);    // no TLS callback ran for the re-entry
    const std::vector<unsigned long> expected = {0x101, 0x201, 0x102, 0x202, 0x103, 0x203};
    FX_CHECK(tls_events(h) == expected);
    // A different thread still gets its notifications.
    std::thread other([&] {
        ensure_thread_context();
        end_thread_context();
    });
    other.join();
    FX_CHECK_EQ(tls_events(h).size(), 10u);
}

FX_TEST(dynamic_load_extends_existing_threads) {
    FXL_SKIP_UNLESS(fixture(L"fxl_tls.dll"));
    fresh_loader();
    std::mutex lock;
    std::condition_variable cv;
    int phase = 0;
    std::uint64_t h = 0, seen_value = 0, seen_counter = 0;
    bool block_present = false;
    std::thread worker([&] {
        GuestContext& ctx = ensure_thread_context();   // exists before the DLL
        {
            std::unique_lock guard(lock);
            phase = 1;
            cv.notify_all();
            cv.wait(guard, [&] { return phase == 2; });
        }
        void** array = tls_array(ctx);
        block_present = array && array[find_guest_module(h)->tls_index] != nullptr;
        loader_refresh_thread_tls(ctx);
        loader_refresh_thread_tls(ctx);   // fast path
        seen_value = run_guest(proc(h, "tls_get_value"));
        seen_counter = run_guest(proc(h, "tls_get_counter"));
        end_thread_context();
    });
    {
        std::unique_lock guard(lock);
        cv.wait(guard, [&] { return phase == 1; });
    }
    h = load_tls_dll();
    {
        std::lock_guard guard(lock);
        phase = 2;
        cv.notify_all();
    }
    worker.join();
    FX_REQUIRE(h);
    FX_CHECK(block_present);
    FX_CHECK_EQ(seen_value, 1234u);
    FX_CHECK_EQ(seen_counter, 0u);   // no DLL_THREAD_ATTACH for a thread that already existed
    const auto events = tls_events(h);
    const std::vector<unsigned long> expected = {0x101, 0x201, 0x103, 0x203};
    FX_CHECK(events == expected);
}

FX_TEST(tls_slots_for_exe_and_dll) {
    FXL_SKIP_UNLESS(fixture(L"fxl_importer.exe"));
    fresh_loader();
    LoaderConfig config;
    config.application_path = fixture(L"fxl_importer.exe");
    loader_configure(config);
    std::string error;
    bool init_failed = false;
    const std::uint64_t base = load_exe(fixture(L"fxl_importer.exe"), error, init_failed);
    FX_REQUIRE(base);
    const GuestModule* exe = find_guest_module(base);
    const GuestModule* dll = find_guest_module(guest_get_module_handle(L"fxl_tls.dll"));
    FX_REQUIRE(exe && dll);
    FX_CHECK_EQ(exe->tls_index, 0u);
    FX_CHECK_EQ(dll->tls_index, 1u);
    FX_CHECK_EQ(*reinterpret_cast<const std::uint32_t*>(tls_directory(base).AddressOfIndex), 0u);
    FX_CHECK_EQ(*reinterpret_cast<const std::uint32_t*>(tls_directory(dll->base).AddressOfIndex), 1u);
    FX_CHECK_EQ(run_guest(proc(base, "fxl_sum")), 307u);   // reads the EXE's own TLS (exe_tls = 77)
    FX_CHECK_EQ(run_guest(proc(base, "fxl_tls_value")), 1234u);
    FX_CHECK_EQ(exe->stack_reserve, 1024u * 1024u);   // lld default /stack
}

// ---- process exit and launch (child processes) ---------------------------------------

FX_TEST(child_entry) {
    const std::wstring mode = child_mode();
    if (mode.empty()) return;   // only meaningful in a child started by the tests below
    if (mode == L"exit") {
        fresh_loader();
        if (!load_tls_dll()) ExitProcess(100);
        loader_process_exit(7);
    }
    if (mode == L"launch" || mode == L"launch-importer") {
        LaunchOptions options;
        options.image_path = mode == L"launch" ? legacy_fixture(L"guest_app.exe") : fixture(L"fxl_importer.exe");
        options.command_line = build_guest_command_line(options.image_path, {L"arg one", L"two"});
        const int code = launch_guest_process(options);
        ExitProcess(static_cast<UINT>(1000 + code));   // only reached on failure
    }
    if (mode == L"stack") {
        // The stub runtime records guest_image_stack_reserve/commit when the
        // main thread context is created; cache_flush prints them at exit.
        LaunchOptions options;
        options.image_path = legacy_fixture(L"guest_app.exe");
        wchar_t value[32] = {};
        const DWORD n = GetEnvironmentVariableW(L"FXL_STACK_OPTION", value, 32);
        if (n) parse_stack_size(std::wstring_view(value, n), options.stack_reserve);
        launch_guest_process(options);
    }
    if (mode == L"exitwait") {
        // Workers alive (blocked) or exiting at process exit, and a guest
        // DllMain(DLL_PROCESS_DETACH) that waits for all of them.
        static std::vector<HANDLE> workers;
        fresh_loader();
        stubs().replacements["FxlWaitForWorkers"] = fake_host([](GuestContext& ctx) -> std::uint64_t {
            const DWORD r = WaitForMultipleObjects(static_cast<DWORD>(workers.size()), workers.data(), TRUE, INFINITE);
            char line[96];
            std::snprintf(line, sizeof(line), "detach waited=%lu reserved=%llu\n", r,
                          static_cast<unsigned long long>(ctx.x[0]));
            DWORD written = 0;
            WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
            return 0;
        });
        LoaderConfig config;
        config.application_path = fixture(L"fxl_importer.exe");
        loader_configure(config);
        if (!guest_load_library(L"fxl_exitwait.dll", 0)) ExitProcess(101);
        static std::atomic<int> started{0};
        auto spawn = [](DWORD linger) {
            std::thread t([linger] {
                ensure_thread_context();   // a guest thread (attached)
                started.fetch_add(1);
                Sleep(linger);             // INFINITE: still blocked at exit; 30: exits during exit
            });
            HANDLE h = nullptr;
            DuplicateHandle(GetCurrentProcess(), t.native_handle(), GetCurrentProcess(), &h, SYNCHRONIZE, FALSE, 0);
            workers.push_back(h);
            t.detach();
        };
        spawn(30);
        spawn(INFINITE);
        spawn(INFINITE);
        spawn(0);
        while (started.load() < 4) Sleep(1);
        Sleep(10);
        loader_process_exit(9);
    }
    if (mode == L"missing") {
        ExitProcess(static_cast<UINT>(launch_guest_process(LaunchOptions{L"C:\\definitely\\missing.exe"})));
    }
}

FX_TEST(process_exit_runs_detach_and_flushes_cache) {
    FXL_SKIP_UNLESS(fixture(L"fxl_tls.dll"));
    std::string output;
    const DWORD code = run_child("child_entry", L"exit", output);
    FX_CHECK_EQ(code, 7ul);
    // At exit: TLS callback then DllMain, both DLL_PROCESS_DETACH, before cache_flush.
    FX_CHECK(output.find("events: 101 201 100 200") != std::string::npos);
    FX_CHECK(output.find("cache_flush") != std::string::npos);
    if (code != 7) std::printf("  child output: %s\n", output.c_str());
}

// Regression (Blender hung after "Blender quit"): guest DLL_PROCESS_DETACH ran
// on the exiting thread under the loader lock while worker threads were alive;
// a DllMain waiting for them deadlocked with workers whose LdrShutdownThread
// (host loader lock) needed the loader lock. Now other threads are terminated
// first and detach runs from the host process-detach hook with lpReserved = 1.
FX_TEST(process_exit_terminates_threads_before_guest_detach) {
    FXL_SKIP_UNLESS(fixture(L"fxl_exitwait.dll"));
    for (int run = 0; run < 3; ++run) {   // the exiting worker races the exit
        std::string output;
        const DWORD code = run_child("child_entry", L"exitwait", output);
        FX_CHECK_EQ(code, 9ul);
        FX_CHECK(output.find("detach waited=0 reserved=1") != std::string::npos);
        FX_CHECK(output.find("process_detach_done") != std::string::npos);
        if (code != 9) std::printf("  child output: %s\n", output.c_str());
    }
}

// Lock inversion between a guest thread exiting and a thread loading: the
// exiting thread runs its teardown (DLL_THREAD_DETACH) inside LdrShutdownThread,
// holding the host loader lock, and needs the loader lock; the other thread
// holds the loader lock and calls LoadLibraryExW, which needs the host loader
// lock. The teardown's bounded wait must break the cycle.
FX_TEST(thread_teardown_inside_host_loader_lock_cannot_deadlock_a_load) {
    FXL_SKIP_UNLESS(fixture(L"fxl_tls.dll"));
    fresh_loader();
    const std::uint64_t h = load_tls_dll();
    FX_REQUIRE(h);
    // A system DLL this process has not loaded yet.
    const wchar_t* candidates[] = {L"dciman32.dll", L"msimg32.dll", L"xmllite.dll", L"wsock32.dll", L"mscms.dll"};
    const wchar_t* fresh = nullptr;
    for (const wchar_t* name : candidates) {
        if (!GetModuleHandleW(name)) {
            fresh = name;
            break;
        }
    }
    FX_REQUIRE(fresh);
    std::atomic<int> phase{0};
    std::thread worker([&] {
        ensure_thread_context();   // attached guest thread
        phase = 1;
        while (phase.load() != 2) Sleep(1);
        // Returning ends the thread: the stub's DLL_THREAD_DETACH TLS callback
        // (like the runtime's) runs loader_thread_detach under the host loader lock.
    });
    while (phase.load() != 1) Sleep(1);
    HMODULE loaded = nullptr;
    const auto start = GetTickCount64();
    {
        std::lock_guard lock(state().lock);   // as guest_load_library holds it
        phase = 2;
        Sleep(300);   // the worker is now in LdrShutdownThread waiting for the loader lock
        loaded = LoadLibraryExW(fresh, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    }
    worker.join();
    const auto elapsed = GetTickCount64() - start;
    FX_CHECK(loaded != nullptr);
    FX_CHECK(elapsed < 15000);
    if (loaded) FreeLibrary(loaded);
    // The worker's DLL_THREAD_DETACH was skipped (it would have deadlocked);
    // its TLS record is gone and later threads work normally.
    const auto events = tls_events(h);
    FX_CHECK(std::find(events.begin(), events.end(), 0x203ul) == events.end());
    std::thread later([&] {
        ensure_thread_context();
        end_thread_context();
    });
    later.join();
    const auto after = tls_events(h);
    FX_CHECK(std::find(after.begin(), after.end(), 0x203ul) != after.end());
}

FX_TEST(launch_guest_process_runs_the_program) {
    FXL_SKIP_UNLESS(legacy_fixture(L"guest_app.exe"));
    std::string output;
    FX_CHECK_EQ(run_child("child_entry", L"launch", output), 42ul);
    FX_CHECK(output.find("cache_flush") != std::string::npos);
    if (exists(fixture(L"fxl_importer.exe"))) {
        output.clear();
        FX_CHECK_EQ(run_child("child_entry", L"launch-importer", output), 307ul);
    }
    output.clear();
    FX_CHECK_EQ(run_child("child_entry", L"missing", output), 2ul);
}

FX_TEST(launch_stack_size_precedence) {
    FXL_SKIP_UNLESS(legacy_fixture(L"guest_app.exe"));
    auto launch = [](const wchar_t* env, const wchar_t* option) {
        std::string output;
        if (option) SetEnvironmentVariableW(L"FXL_STACK_OPTION", option);
        const DWORD code = run_child("child_entry", L"stack", output,
                                     env ? L"TRANSMUTE_FX_STACK_SIZE" : nullptr, env);
        SetEnvironmentVariableW(L"FXL_STACK_OPTION", nullptr);
        FX_CHECK_EQ(code, 42ul);
        return output;
    };
    // guest_app.exe: lld defaults, SizeOfStackReserve 1 MiB, commit 4 KiB.
    FX_CHECK(launch(nullptr, nullptr).find("stack=1048576,4096") != std::string::npos);
    FX_CHECK(launch(L"16m", nullptr).find("stack=16777216,4096") != std::string::npos);
    FX_CHECK(launch(L"16m", L"64m").find("stack=67108864,4096") != std::string::npos);
    FX_CHECK(launch(L"bogus", nullptr).find("stack=1048576,4096") != std::string::npos);

    // The precedence logic directly.
    PeHeaders exe;
    exe.stack_reserve = 1 << 20;
    exe.stack_commit = 0x2000;
    std::uint64_t reserve = 0, commit = 0;
    const char* source = nullptr;
    SetEnvironmentVariableW(L"TRANSMUTE_FX_STACK_SIZE", nullptr);
    resolve_stack_sizes(0, exe, reserve, commit, &source);
    FX_CHECK_EQ(reserve, 1u << 20);
    FX_CHECK_EQ(commit, 0x2000u);
    FX_CHECK_EQ(std::string(source), std::string("image"));
    SetEnvironmentVariableW(L"TRANSMUTE_FX_STACK_SIZE", L"16m");
    resolve_stack_sizes(0, exe, reserve, commit, &source);
    FX_CHECK_EQ(reserve, 16u << 20);
    FX_CHECK_EQ(std::string(source), std::string("environment"));
    resolve_stack_sizes(64u << 20, exe, reserve, commit, &source);
    FX_CHECK_EQ(reserve, 64u << 20);
    FX_CHECK_EQ(std::string(source), std::string("option"));
    SetEnvironmentVariableW(L"TRANSMUTE_FX_STACK_SIZE", L"0x10000");
    exe.stack_commit = 1 << 20;
    resolve_stack_sizes(0, exe, reserve, commit, &source);
    FX_CHECK_EQ(reserve, 0x10000u);
    FX_CHECK_EQ(commit, 0x10000u);   // commit capped at reserve
    SetEnvironmentVariableW(L"TRANSMUTE_FX_STACK_SIZE", nullptr);
}

FX_TEST(stack_size_parsing) {
    std::uint64_t v = 0;
    FX_CHECK(parse_stack_size(L"16m", v) && v == 16ull << 20);
    FX_CHECK(parse_stack_size(L"16M", v) && v == 16ull << 20);
    FX_CHECK(parse_stack_size(L"16MiB", v) && v == 16ull << 20);
    FX_CHECK(parse_stack_size(L"512k", v) && v == 512ull << 10);
    FX_CHECK(parse_stack_size(L"1g", v) && v == 1ull << 30);
    FX_CHECK(parse_stack_size(L"1048576", v) && v == 1ull << 20);
    FX_CHECK(parse_stack_size(L"0x100000", v) && v == 1ull << 20);
    FX_CHECK(parse_stack_size(L" 100000 ", v) && v == 0x20000);   // rounded up to 64 KiB
    FX_CHECK(parse_stack_size(L"1", v) && v == 0x10000);
    FX_CHECK(!parse_stack_size(L"", v));
    FX_CHECK(!parse_stack_size(L"0", v));
    FX_CHECK(!parse_stack_size(L"m", v));
    FX_CHECK(!parse_stack_size(L"12q", v));
    FX_CHECK(!parse_stack_size(L"-5", v));
    FX_CHECK(!parse_stack_size(L"99999999999999999999", v));
    FX_CHECK(!parse_stack_size(L"2048g", v));
    CliOptions cli;
    std::wstring error;
    const wchar_t* a[] = {L"transmute-fx", L"--stack-size", L"16m", L"python.exe"};
    FX_REQUIRE(parse_cli(4, a, cli, error));
    FX_CHECK_EQ(cli.stack_reserve, 16ull << 20);
    const wchar_t* b[] = {L"transmute-fx", L"--stack-size", L"huge", L"python.exe"};
    FX_CHECK(!parse_cli(4, b, cli, error));
}
