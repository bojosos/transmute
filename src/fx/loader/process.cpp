// Guest process startup and shutdown.

#include "fx/loader/command_line.hpp"
#include "fx/loader/loader_internal.hpp"

#include "fx/cache/cache.hpp"
#include "fx/runtime/runtime.hpp"

#include <intrin.h>
#include <psapi.h>

#include <cstdio>
#include <cstdlib>

namespace transmute::fx {
namespace loader {
namespace {

std::atomic<std::uint64_t> g_stack_reserve{0};
std::atomic<std::uint64_t> g_stack_commit{0};

constexpr int kExitNotFound = 2;                          // ERROR_FILE_NOT_FOUND
constexpr int kExitBadImage = static_cast<int>(0xC000007BL);   // STATUS_INVALID_IMAGE_FORMAT
constexpr int kExitInitFailed = static_cast<int>(0xC0000142L); // STATUS_DLL_INIT_FAILED

// Milliseconds since the host process was created.
double process_age_ms() {
    FILETIME created, exited, kernel, user, now;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return 0;
    GetSystemTimePreciseAsFileTime(&now);
    const auto value = [](const FILETIME& t) { return (std::uint64_t{t.dwHighDateTime} << 32) | t.dwLowDateTime; };
    return static_cast<double>(value(now) - value(created)) / 1e4;
}

// The guest is an ARM64 process: give it the processor variables an ARM64
// process sees on ARM64 Windows (Python's platform.machine() reads
// PROCESSOR_ARCHITEW6432, then PROCESSOR_ARCHITECTURE; platform.processor()
// PROCESSOR_IDENTIFIER). _wputenv_s updates the host UCRT's tables, which
// the guest's CRT imports share, and the process environment block. The
// abi's CreateProcess override gives x64 children the host values back.
void set_guest_processor_environment() {
    static const struct {
        const wchar_t* name;
        const wchar_t* value;
    } kVariables[] = {
        {L"PROCESSOR_ARCHITECTURE", L"ARM64"},
        {L"PROCESSOR_IDENTIFIER", L"ARMv8 (64-bit) Family 8 Model 0 Revision   0, Transmute fx"},
        {L"PROCESSOR_LEVEL", L"1"},
        {L"PROCESSOR_REVISION", L"0000"},
        {L"PROCESSOR_ARCHITEW6432", L""},   // removed: no WOW64 layer between guest and "kernel"
    };
    for (const auto& v : kVariables) _wputenv_s(v.name, v.value);
}

} // namespace

void resolve_stack_sizes(std::uint64_t option, const PeHeaders& exe, std::uint64_t& reserve,
                         std::uint64_t& commit, const char** source) {
    reserve = exe.stack_reserve;
    const char* from = "image";
    if (option != 0) {
        reserve = option;
        from = "option";
    } else {
        wchar_t text[64];
        const DWORD n = GetEnvironmentVariableW(L"TRANSMUTE_FX_STACK_SIZE", text, 64);
        std::uint64_t value = 0;
        if (n > 0 && n < 64) {
            if (parse_stack_size(std::wstring_view(text, n), value)) {
                reserve = value;
                from = "environment";
            } else {
                std::fwprintf(stderr, L"transmute-fx: ignoring invalid TRANSMUTE_FX_STACK_SIZE=%ls\n", text);
            }
        }
    }
    commit = exe.stack_commit;
    if (reserve != 0 && commit > reserve) commit = reserve;
    if (source) *source = from;
}

} // namespace loader

using namespace loader;

std::uint64_t guest_image_stack_reserve() noexcept {
    return g_stack_reserve.load(std::memory_order_acquire);
}

std::uint64_t guest_image_stack_commit() noexcept {
    return g_stack_commit.load(std::memory_order_acquire);
}

const std::wstring& guest_command_line() { return state().command_line; }

const std::wstring& guest_image_path() { return state().image_path; }

// --profile or TRANSMUTE_FX_STATS=1: runtime counters on stderr at exit.
void print_runtime_stats_if_requested() {
    char value[8]{};
    const bool env = GetEnvironmentVariableA("TRANSMUTE_FX_STATS", value, sizeof(value)) > 0 &&
                     value[0] != '0';
    if (!env && !state().profile) return;
    const auto st = runtime_stats();
    static const char* const reasons[] = {"none", "link", "lookup_miss", "interpret", "svc", "brk",
                                          "return_to_host", "interrupt", "raise", "redispatch", "tier_up",
                                          "tier_up_loop", "table_shadow"};
    std::fprintf(stderr, "transmute-fx stats: regions=%llu translations=%llu cache_hits=%llu links=%llu "
                 "interpreted=%llu smc_faults=%llu host_callbacks=%llu guest_exceptions=%llu "
                 "threads=%llu arena_used=%llu arena_committed=%llu reclaimed=%llu reclaim_passes=%llu "
                 "evicted=%llu\n",
                 (unsigned long long)st.regions, (unsigned long long)st.translations,
                 (unsigned long long)st.cache_hits, (unsigned long long)st.links,
                 (unsigned long long)st.interpreted, (unsigned long long)st.smc_write_faults,
                 (unsigned long long)st.host_callbacks, (unsigned long long)st.guest_exceptions,
                 (unsigned long long)st.threads_created, (unsigned long long)st.arena_used,
                 (unsigned long long)st.arena_committed, (unsigned long long)st.reclaimed_bytes,
                 (unsigned long long)st.reclaim_passes, (unsigned long long)st.evicted_regions);
    const auto cs = cache_stats();
    std::fprintf(stderr, "transmute-fx cache: modules=%llu with_cache=%llu lookups=%llu hits=%llu "
                 "rejected=%llu guest_mismatch=%llu stores=%llu ignored=%llu unrelocatable=%llu\n",
                 (unsigned long long)cs.modules_loaded, (unsigned long long)cs.modules_with_cache,
                 (unsigned long long)cs.lookups, (unsigned long long)cs.hits,
                 (unsigned long long)cs.records_rejected, (unsigned long long)cs.guest_mismatches,
                 (unsigned long long)cs.stores, (unsigned long long)cs.stores_ignored,
                 (unsigned long long)cs.stores_unrelocatable);
    FILETIME created{}, exited{}, kernel{}, user{};
    GetThreadTimes(GetCurrentThread(), &created, &exited, &kernel, &user);
    const auto ft = [](const FILETIME& t) { return static_cast<double>((std::uint64_t{t.dwHighDateTime} << 32) | t.dwLowDateTime) / 1e4; };
    PROCESS_MEMORY_COUNTERS_EX memory{};
    memory.cb = sizeof(memory);
    K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memory), sizeof(memory));
    std::fprintf(stderr, "transmute-fx time: exiting thread cpu %.1f ms (kernel %.1f); page faults %lu, peak working set %llu MB, private %llu MB\n",
                 ft(kernel) + ft(user), ft(kernel), memory.PageFaultCount,
                 static_cast<unsigned long long>(memory.PeakWorkingSetSize >> 20),
                 static_cast<unsigned long long>(memory.PrivateUsage >> 20));
    std::fprintf(stderr, "transmute-fx time: process %.1f ms, translate %.1f ms (cache %.1f, jit %.1f, install %.1f), "
                 "preloaded %llu\n",
                 process_age_ms(), st.translate_seconds * 1e3, st.cache_seconds * 1e3, st.jit_seconds * 1e3,
                 st.install_seconds * 1e3, static_cast<unsigned long long>(st.preloaded));
    {
        LARGE_INTEGER now{}, frequency{};
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&frequency);
        const double drained = st.preload_drained_qpc
                                   ? process_age_ms() - static_cast<double>(now.QuadPart - static_cast<LONGLONG>(st.preload_drained_qpc)) *
                                                            1e3 / static_cast<double>(frequency.QuadPart)
                                   : -1.0;
        std::fprintf(stderr, "transmute-fx startup: map lock waits %llu (%.1f ms); preloader busy %.1f ms, queue drained at %.1f ms\n",
                     (unsigned long long)st.map_waits, st.map_wait_seconds * 1e3, st.preload_seconds * 1e3, drained);
    }
    std::fprintf(stderr, "transmute-fx aot: cold_files=%llu cold_hits=%llu promoted=%llu background_requests=%llu "
                 "helpers=%llu; jit %llu guest instructions, %.0f ns/instruction\n",
                 (unsigned long long)cs.cold_files, (unsigned long long)cs.cold_hits,
                 (unsigned long long)cs.cold_promoted, (unsigned long long)cs.aot_requests,
                 (unsigned long long)cs.aot_helpers, (unsigned long long)st.jit_guest_instructions,
                 st.jit_guest_instructions ? st.jit_seconds * 1e9 / static_cast<double>(st.jit_guest_instructions) : 0.0);
    std::fprintf(stderr, "transmute-fx tier: tier0_regions=%llu tier_ups=%llu (cached %llu) tier_up_instructions=%llu "
                 "tier_up_jit %.1f ms, replaced_entries=%llu\n",
                 (unsigned long long)st.tier0_regions, (unsigned long long)st.tier_ups,
                 (unsigned long long)st.tier_up_cached, (unsigned long long)st.tier_up_instructions,
                 st.tier_up_seconds * 1e3, (unsigned long long)st.tier_replaced_entries);
    std::fprintf(stderr, "transmute-fx exits:");
    for (unsigned i = 0; i < 16; ++i)
        if (st.exits[i]) std::fprintf(stderr, " %s=%llu", i < std::size(reasons) ? reasons[i] : "?",
                                      (unsigned long long)st.exits[i]);
    std::fprintf(stderr, "\n");
}

namespace {

// Everything both exit paths do before the host call that ends the process.
// runtime_prepare_exit comes first: it stops the background preloaders
// before anything else (cache flush, profile, guest DLL_PROCESS_DETACH) can
// need the region map, and before ExitProcess terminates them wherever they are.
double g_exit_start_ms = 0;   // TRANSMUTE_FX_STATS: process age when the guest asked to exit

bool stats_requested() {
    char value[8]{};
    return (GetEnvironmentVariableA("TRANSMUTE_FX_STATS", value, sizeof(value)) > 0 && value[0] != '0') ||
           state().profile;
}

void prepare_process_end() {
    const double start = process_age_ms();
    g_exit_start_ms = start;
    runtime_prepare_exit();
    const double prepared = process_age_ms();
    cache_flush();
    const double flushed = process_age_ms();
    runtime_profile_flush();
    std::fflush(stdout);
    print_runtime_stats_if_requested();
    if (stats_requested())
        std::fprintf(stderr, "transmute-fx exit: requested at %.1f ms; stop background %.1f ms, cache flush %.1f ms\n",
                     start, prepared - start, flushed - prepared);
    std::fflush(stderr);
}

} // namespace

void loader_process_exit(std::uint32_t exit_code) {
    auto& s = state();
    // Windows ExitProcess semantics: every other thread is terminated before
    // DLL_PROCESS_DETACH runs, so detach code never waits on live threads and
    // the loader lock is never held here while other threads need it (their
    // LdrShutdownThread holds the host loader lock and runs the runtime's
    // teardown, which enters the loader). Guest DLL_PROCESS_DETACH therefore
    // runs from the host process-detach hook below, inside host ExitProcess.
    s.exiting = true;
    if (s.config.run_initializers) s.detach_at_exit = true;
    prepare_process_end();
    if (stats_requested()) {
        std::fprintf(stderr, "transmute-fx exit: ExitProcess at %.1f ms\n", process_age_ms());
        std::fflush(stderr);
    }
    ExitProcess(exit_code);
}

void loader_process_terminate(std::uint32_t exit_code) {
    auto& s = state();
    // TerminateProcess semantics: no DLL_PROCESS_DETACH, guest or host.
    s.exiting = true;
    s.detach_at_exit = false;
    prepare_process_end();
    TerminateProcess(GetCurrentProcess(), exit_code);
    ExitProcess(exit_code);   // not reached
}

// Runs on the exiting thread from transmute-fx.exe's TLS callback
// (DLL_PROCESS_DETACH) inside host ExitProcess: all other threads are gone and
// this thread holds the host loader lock (i.e. the loader lock).
void process_detach_hook(void* reserved) {
    auto& s = state();
    if (!s.detach_at_exit.load()) return;   // no guest process ran (e.g. unit tests)
    const double hook_start = process_age_ms();
    s.exiting = true;
    (void)reserved;   // NULL for EXE TLS callbacks even at process exit
    if (!s.config.run_initializers) return;
    // Every other thread is gone. One that died inside the runtime or the
    // cache (holding the region map, the arena, a cache lookup epoch, ...)
    // would deadlock the first translation miss of the detach code: skip
    // guest detach then. runtime_prepare_exit makes this rare for guest exits
    // (it parks the background preloaders); host code calling ExitProcess
    // directly gets the same check.
    if (!runtime_exit_safe()) {
        std::fprintf(stderr, "transmute-fx: guest DLL_PROCESS_DETACH skipped: a thread terminated by "
                             "ExitProcess held a runtime lock\n");
        std::fflush(stderr);
        return;
    }
    // Recursive: succeeds for ExitProcess called from inside a guest DllMain.
    std::unique_lock lock(s.lock, std::try_to_lock);
    if (!lock.owns_lock()) return;
    const std::vector<Module*> order = s.init_order;
    for (auto it = order.rbegin(); it != order.rend(); ++it) {
        Module* m = *it;
        if (!m->initialized) continue;
        m->initialized = false;
        if (m->has_tls && m->tls.callbacks) {
            // run_tls_callbacks refuses once exiting is set; call directly.
            for (unsigned i = 0; i < 1024; ++i) {
                const std::uint64_t cb = reinterpret_cast<const std::uint64_t*>(m->tls.callbacks)[i];
                if (!cb) break;
                call_guest_init(cb, m->info.base, DLL_PROCESS_DETACH, 1);
            }
        }
        if (m->info.is_dll && m->info.entry) {
            call_guest_init(m->info.entry, m->info.base, DLL_PROCESS_DETACH, 1);
        }
    }
    std::fflush(stdout);
    // Detach code runs after cache_flush: keep what it translated, or every
    // warm run translates it again (Blender: ~6k regions, ~60 ms at exit).
    cache_flush_journals();
    if (stats_requested()) {
        const auto st = runtime_stats();
        std::fprintf(stderr, "transmute-fx exit: guest DLL_PROCESS_DETACH from %.1f to %.1f ms (translations so far %llu)\n",
                     hook_start, process_age_ms(), (unsigned long long)st.translations);
        std::fflush(stderr);
    }
}

int launch_guest_process(const LaunchOptions& options) {
    auto& s = state();
    const double launch_ms = process_age_ms();
    const std::wstring path = full_path(options.image_path);
    PeHeaders headers;
    std::string error;
    if (!file_exists(path)) {
        std::fwprintf(stderr, L"transmute-fx: cannot find %ls\n", path.c_str());
        return kExitNotFound;
    }
    if (!read_image_headers(path, headers, error)) {
        std::fwprintf(stderr, L"transmute-fx: %ls: %hs\n", path.c_str(), error.c_str());
        return kExitBadImage;
    }
    if (headers.machine != kMachineArm64 || headers.is_dll()) {
        std::fwprintf(stderr, L"transmute-fx: %ls is not an ARM64 executable (machine 0x%04x%ls)\n",
                      path.c_str(), headers.machine, headers.is_dll() ? L", DLL" : L"");
        return kExitBadImage;
    }
    std::uint64_t stack_reserve = 0, stack_commit = 0;
    resolve_stack_sizes(options.stack_reserve, headers, stack_reserve, stack_commit);
    g_stack_reserve.store(stack_reserve, std::memory_order_release);
    g_stack_commit.store(stack_commit, std::memory_order_release);

    LoaderConfig config;
    {
        std::lock_guard lock(s.lock);
        config.verbose = s.config.verbose;
        config.force_relocation = s.config.force_relocation;
    }
    config.application_path = path;
    config.dll_directories = options.dll_directories;
    loader_configure(config);
    {
        std::lock_guard lock(s.lock);
        s.image_path = path;
        s.command_line = options.command_line.empty() ? build_guest_command_line(path, {})
                                                      : options.command_line;
    }

    s.profile = options.runtime.profile;
    const double runtime_start_ms = process_age_ms();
    loader::set_guest_processor_environment();
    runtime_initialize(options.runtime);
    GuestContext& ctx = ensure_thread_context();
    loader_thread_attach(ctx);   // idempotent; registers the main thread for static TLS

    bool init_failed = false;
    const double load_start_ms = process_age_ms();
    const std::uint64_t base = load_exe(path, error, init_failed);
    if (!base) {
        std::fwprintf(stderr, L"transmute-fx: cannot load %ls: %hs\n", path.c_str(), error.c_str());
        return init_failed ? kExitInitFailed : kExitBadImage;
    }
    if (config.verbose) {
        const auto stats = loader_stats();
        std::fprintf(stderr,
                     "transmute-fx loader: %llu modules (%llu relocated), %llu guest / %llu host / "
                     "%llu missing imports, map %.1f ms, bind %.1f ms, protect %.1f ms, init %.1f ms, "
                     "since process start %.1f ms\n",
                     static_cast<unsigned long long>(stats.modules_mapped),
                     static_cast<unsigned long long>(stats.modules_relocated),
                     static_cast<unsigned long long>(stats.guest_imports),
                     static_cast<unsigned long long>(stats.host_imports),
                     static_cast<unsigned long long>(stats.missing_imports), stats.map_seconds * 1e3,
                     stats.bind_seconds * 1e3, stats.protect_seconds * 1e3, stats.init_seconds * 1e3,
                     process_age_ms());
        std::fprintf(stderr, "transmute-fx loader: launch at %.1f ms, runtime init at %.1f ms, load_exe at %.1f ms\n",
                     launch_ms, runtime_start_ms, load_start_ms);
    }

    std::uint64_t entry = 0;
    {
        std::lock_guard lock(s.lock);
        if (s.exe) entry = s.exe->info.entry;
    }
    s.detach_at_exit = true;   // from here on, ExitProcess runs guest DLL_PROCESS_DETACH
    // The EXE manifest's shared assemblies form the guest's default activation
    // context: keep it active on the main thread (threads it creates inherit it).
    if (void* context = guest_activation_context()) {
        ULONG_PTR cookie = 0;
        ActivateActCtx(context, &cookie);
    }
    // The process entry point receives the PEB (RtlUserThreadStart convention).
    const std::uint64_t args[1] = {__readgsqword(0x60)};
    const std::uint64_t result = call_guest(entry, args);
    loader_process_exit(static_cast<std::uint32_t>(result));
}

} // namespace transmute::fx

// ---- host process-detach hook -----------------------------------------------------------
// An EXE TLS callback: host ExitProcess calls it with DLL_PROCESS_DETACH after
// terminating every other thread. .CRT$XLB sorts before the CRT's own
// callbacks (.CRT$XLC/XLD, thread_local destructors), so runtime thread_locals
// are still alive while guest detach code runs.
extern "C" void NTAPI fx_loader_process_detach(PVOID, DWORD reason, PVOID reserved) {
    if (reason == DLL_PROCESS_DETACH) transmute::fx::process_detach_hook(reserved);
}
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:fx_loader_tls_callback")
#pragma const_seg(push)
#pragma const_seg(".CRT$XLB")
extern "C" const PIMAGE_TLS_CALLBACK fx_loader_tls_callback = &fx_loader_process_detach;
#pragma const_seg(pop)
