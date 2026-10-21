// CRT overrides (owner: abi): process arguments, initializer and exit
// tables, process exit and the floating-point environment.
//
// Functions that merely take guest callbacks (qsort, bsearch, signal,
// _set_invalid_parameter_handler, FlsAlloc, thread-pool APIs, ...) need no
// override: their database signatures carry '@' function-pointer types and
// the generated thunks pass host_callable trampolines automatically.

#include "fx/abi/abi_internal.hpp"
#include "fx/abi/overrides_internal.hpp"
#include "transmute/fx/process.hpp"

#include <windows.h>
#include <xmmintrin.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>

namespace transmute::fx::abi {
namespace {

using R = OverrideResult;
constexpr R kReturn = R::return_to_caller;

// ============================================================================
// Command line and argv (UCRT parsing rules)
// ============================================================================

template <typename Char>
std::vector<std::basic_string<Char>> parse_command_line(const Char* p) {
    std::vector<std::basic_string<Char>> args;
    if (p == nullptr) return args;
    // Program name: quotes toggle and are dropped, no escapes.
    std::basic_string<Char> current;
    bool in_quotes = false;
    while (*p != 0 && (in_quotes || (*p != ' ' && *p != '\t'))) {
        if (*p == '"') in_quotes = !in_quotes;
        else current.push_back(*p);
        ++p;
    }
    args.push_back(current);
    in_quotes = false;
    for (;;) {
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == 0) break;
        current.clear();
        for (;;) {
            bool copy = true;
            unsigned backslashes = 0;
            while (*p == '\\') { ++p; ++backslashes; }
            if (*p == '"') {
                if (backslashes % 2 == 0) {
                    if (in_quotes && p[1] == '"') ++p;   // "" inside quotes: literal quote
                    else { copy = false; in_quotes = !in_quotes; }
                }
                backslashes /= 2;
            }
            current.append(backslashes, static_cast<Char>('\\'));
            if (*p == 0 || (!in_quotes && (*p == ' ' || *p == '\t'))) break;
            if (copy) current.push_back(*p);
            ++p;
        }
        args.push_back(current);
    }
    return args;
}

// The command line after the program name (_get_*_winmain_command_line).
template <typename Char>
const Char* winmain_command_line(const Char* p) {
    bool in_quotes = false;
    while (*p > ' ' || (*p != 0 && in_quotes)) {
        if (*p == '"') in_quotes = !in_quotes;
        ++p;
    }
    while (*p != 0 && *p <= ' ') ++p;
    return p;
}

struct GuestArgs {
    int argc{};
    std::vector<std::string> narrow_storage;
    std::vector<std::wstring> wide_storage;
    std::vector<char*> argv;
    std::vector<wchar_t*> wargv;
    char** argv_ptr{};
    wchar_t** wargv_ptr{};
    std::string acmdln_storage;
    std::wstring wcmdln_storage;
    char* acmdln{};
    wchar_t* wcmdln{};
    std::string pgm_storage;
    std::wstring wpgm_storage;
    char* pgmptr{};
    wchar_t* wpgmptr{};
};

GuestArgs& guest_args() {
    static GuestArgs* args = [] {
        auto* a = new GuestArgs();    // pointers into it are handed to the guest forever
        a->wcmdln_storage = guest_command_line();
        a->acmdln_storage = wide_to_ansi(a->wcmdln_storage);
        a->wide_storage = parse_command_line(a->wcmdln_storage.c_str());
        for (const auto& w : a->wide_storage) a->narrow_storage.push_back(wide_to_ansi(w));
        for (auto& w : a->wide_storage) a->wargv.push_back(w.data());
        for (auto& n : a->narrow_storage) a->argv.push_back(n.data());
        a->wargv.push_back(nullptr);
        a->argv.push_back(nullptr);
        a->argc = static_cast<int>(a->wide_storage.size());
        a->argv_ptr = a->argv.data();
        a->wargv_ptr = a->wargv.data();
        a->acmdln = a->acmdln_storage.data();
        a->wcmdln = a->wcmdln_storage.data();
        a->wpgm_storage = guest_image_path();
        if (a->wpgm_storage.empty() && !a->wide_storage.empty()) a->wpgm_storage = a->wide_storage[0];
        a->pgm_storage = wide_to_ansi(a->wpgm_storage);
        a->pgmptr = a->pgm_storage.data();
        a->wpgmptr = a->wpgm_storage.data();
        return a;
    }();
    return *args;
}

R o_p___argc(GuestContext& ctx, const OverrideBinding&) { ctx.x[0] = reinterpret_cast<std::uint64_t>(&guest_args().argc); return kReturn; }
R o_p___argv(GuestContext& ctx, const OverrideBinding&) { ctx.x[0] = reinterpret_cast<std::uint64_t>(&guest_args().argv_ptr); return kReturn; }
R o_p___wargv(GuestContext& ctx, const OverrideBinding&) { ctx.x[0] = reinterpret_cast<std::uint64_t>(&guest_args().wargv_ptr); return kReturn; }
R o_p__acmdln(GuestContext& ctx, const OverrideBinding&) { ctx.x[0] = reinterpret_cast<std::uint64_t>(&guest_args().acmdln); return kReturn; }
R o_p__wcmdln(GuestContext& ctx, const OverrideBinding&) { ctx.x[0] = reinterpret_cast<std::uint64_t>(&guest_args().wcmdln); return kReturn; }
R o_p__pgmptr(GuestContext& ctx, const OverrideBinding&) { ctx.x[0] = reinterpret_cast<std::uint64_t>(&guest_args().pgmptr); return kReturn; }
R o_p__wpgmptr(GuestContext& ctx, const OverrideBinding&) { ctx.x[0] = reinterpret_cast<std::uint64_t>(&guest_args().wpgmptr); return kReturn; }

R o_configure_argv(GuestContext& ctx, const OverrideBinding&) {
    (void)guest_args();
    ctx.x[0] = 0;
    return kReturn;
}
R o_get_pgmptr(GuestContext& ctx, const OverrideBinding&) {
    if (ctx.x[0] == 0) { ctx.x[0] = EINVAL; return kReturn; }
    *reinterpret_cast<char**>(ctx.x[0]) = guest_args().pgmptr;
    ctx.x[0] = 0;
    return kReturn;
}
R o_get_wpgmptr(GuestContext& ctx, const OverrideBinding&) {
    if (ctx.x[0] == 0) { ctx.x[0] = EINVAL; return kReturn; }
    *reinterpret_cast<wchar_t**>(ctx.x[0]) = guest_args().wpgmptr;
    ctx.x[0] = 0;
    return kReturn;
}
R o_get_narrow_winmain_command_line(GuestContext& ctx, const OverrideBinding&) {
    ctx.x[0] = reinterpret_cast<std::uint64_t>(winmain_command_line(guest_args().acmdln));
    return kReturn;
}
R o_get_wide_winmain_command_line(GuestContext& ctx, const OverrideBinding&) {
    ctx.x[0] = reinterpret_cast<std::uint64_t>(winmain_command_line(guest_args().wcmdln));
    return kReturn;
}

// msvcrt: __getmainargs(int* argc, char*** argv, char*** env, int wildcard, _startupinfo*)
R getmainargs(GuestContext& ctx, const OverrideBinding& b, bool wide) {
    std::uint64_t result = 0;
    if (b.native && ctx.x[2] != 0) {
        // The native call provides the (shared) environment; argc/argv are the guest's.
        int argc = 0;
        void* argv = nullptr;
        using Fn = int(__cdecl*)(int*, void**, void**, int, void*);
        result = static_cast<std::uint32_t>(reinterpret_cast<Fn>(const_cast<void*>(b.native))(
            &argc, &argv, reinterpret_cast<void**>(ctx.x[2]), static_cast<int>(ctx.x[3]),
            reinterpret_cast<void*>(ctx.x[4])));
    }
    auto& args = guest_args();
    if (ctx.x[0]) *reinterpret_cast<int*>(ctx.x[0]) = args.argc;
    if (ctx.x[1]) {
        if (wide) *reinterpret_cast<wchar_t***>(ctx.x[1]) = args.wargv_ptr;
        else *reinterpret_cast<char***>(ctx.x[1]) = args.argv_ptr;
    }
    ctx.x[0] = result;
    return kReturn;
}
R o_getmainargs(GuestContext& ctx, const OverrideBinding& b) { return getmainargs(ctx, b, false); }
R o_wgetmainargs(GuestContext& ctx, const OverrideBinding& b) { return getmainargs(ctx, b, true); }

// ============================================================================
// Initializers and exit tables
// ============================================================================

// _initterm(_PVFV* first, _PVFV* last)
R o_initterm(GuestContext& ctx, const OverrideBinding&) {
    const auto first = ctx.x[0], last = ctx.x[1];
    for (auto p = first; p < last; p += 8) {
        const auto fn = *reinterpret_cast<const std::uint64_t*>(p);
        if (fn != 0) call_guest_prepared(ctx, fn);
    }
    ctx.x[0] = 0;
    return kReturn;
}
// int _initterm_e(_PIFV* first, _PIFV* last)
R o_initterm_e(GuestContext& ctx, const OverrideBinding&) {
    const auto first = ctx.x[0], last = ctx.x[1];
    for (auto p = first; p < last; p += 8) {
        const auto fn = *reinterpret_cast<const std::uint64_t*>(p);
        if (fn == 0) continue;
        call_guest_prepared(ctx, fn);
        const auto result = static_cast<std::int32_t>(ctx.x[0]);
        if (result != 0) {
            ctx.x[0] = static_cast<std::uint32_t>(result);
            return kReturn;
        }
    }
    ctx.x[0] = 0;
    return kReturn;
}

struct ExitTables {
    std::mutex mutex;
    std::vector<std::uint64_t> at_exit;
    std::vector<std::uint64_t> at_quick_exit;
};
ExitTables& exit_tables() {
    static auto* t = new ExitTables();
    return *t;
}

void run_exit_table(GuestContext& ctx, std::vector<std::uint64_t>& table) {
    for (;;) {
        std::uint64_t fn = 0;
        {
            std::lock_guard lock(exit_tables().mutex);
            if (table.empty()) return;
            fn = table.back();
            table.pop_back();
        }
        if (fn != 0) call_guest_prepared(ctx, fn);
    }
}

void flush_host_streams() {
    static const auto flushall = [] {
        HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
        return ucrt ? reinterpret_cast<int(__cdecl*)()>(GetProcAddress(ucrt, "_flushall")) : nullptr;
    }();
    if (flushall) flushall();
    else std::fflush(nullptr);
}

R o_atexit(GuestContext& ctx, const OverrideBinding&) {          // atexit, _crt_atexit
    std::lock_guard lock(exit_tables().mutex);
    exit_tables().at_exit.push_back(ctx.x[0]);
    ctx.x[0] = 0;
    return kReturn;
}
R o_onexit(GuestContext& ctx, const OverrideBinding&) {          // _onexit returns the function
    std::lock_guard lock(exit_tables().mutex);
    exit_tables().at_exit.push_back(ctx.x[0]);
    return kReturn;
}
R o_at_quick_exit(GuestContext& ctx, const OverrideBinding&) {   // at_quick_exit, _crt_at_quick_exit
    std::lock_guard lock(exit_tables().mutex);
    exit_tables().at_quick_exit.push_back(ctx.x[0]);
    ctx.x[0] = 0;
    return kReturn;
}

// _onexit_table_t { _PVFV* _first, *_last, *_end } holding raw guest
// function pointers in a host allocation. All three table functions are
// overridden together, so no encoding is shared with the host CRT.
struct OnexitTable {
    std::uint64_t* first;
    std::uint64_t* last;
    std::uint64_t* end;
};

R o_initialize_onexit_table(GuestContext& ctx, const OverrideBinding&) {
    auto* table = reinterpret_cast<OnexitTable*>(ctx.x[0]);
    if (table == nullptr) { ctx.x[0] = static_cast<std::uint32_t>(-1); return kReturn; }
    ctx.x[0] = 0;
    return kReturn;
}
R o_register_onexit_function(GuestContext& ctx, const OverrideBinding&) {
    auto* table = reinterpret_cast<OnexitTable*>(ctx.x[0]);
    if (table == nullptr) { ctx.x[0] = static_cast<std::uint32_t>(-1); return kReturn; }
    std::lock_guard lock(exit_tables().mutex);
    if (table->last == table->end) {
        const std::size_t count = static_cast<std::size_t>(table->last - table->first);
        const std::size_t capacity = std::max<std::size_t>(16, count * 2);
        auto* grown = static_cast<std::uint64_t*>(std::realloc(table->first, capacity * sizeof(std::uint64_t)));
        if (grown == nullptr) { ctx.x[0] = static_cast<std::uint32_t>(-1); return kReturn; }
        table->first = grown;
        table->last = grown + count;
        table->end = grown + capacity;
    }
    *table->last++ = ctx.x[1];
    ctx.x[0] = 0;
    return kReturn;
}
R o_execute_onexit_table(GuestContext& ctx, const OverrideBinding&) {
    auto* table = reinterpret_cast<OnexitTable*>(ctx.x[0]);
    if (table == nullptr) { ctx.x[0] = static_cast<std::uint32_t>(-1); return kReturn; }
    OnexitTable taken{};
    {
        std::lock_guard lock(exit_tables().mutex);
        taken = *table;
        *table = OnexitTable{};
    }
    for (auto* p = taken.last; p != taken.first;) {
        const auto fn = *--p;
        if (fn != 0) call_guest_prepared(ctx, fn);
    }
    std::free(taken.first);
    ctx.x[0] = 0;
    return kReturn;
}

// _register_thread_local_exe_atexit_callback(PIMAGE_TLS_CALLBACK): the guest
// EXE's thread_local destructor callback. The host UCRT accepts exactly one
// registration per process and terminates on a second, and transmute-fx.exe
// itself already made one, so the guest's is kept here. Other threads' TLS
// destructors run through the EXE's TLS callbacks (loader thread detach);
// exit() runs the exiting thread's here, before the atexit table, as the
// UCRT does.
std::atomic<std::uint64_t> g_tls_exit_callback{0};

R o_register_thread_local_exe_atexit_callback(GuestContext& ctx, const OverrideBinding&) {
    std::uint64_t expected = 0;
    g_tls_exit_callback.compare_exchange_strong(expected, ctx.x[0]);
    return kReturn;
}

void run_thread_local_exit_callback(GuestContext& ctx) {
    const auto callback = g_tls_exit_callback.exchange(0);
    if (callback == 0) return;
    ctx.x[0] = 0;                    // DllHandle
    ctx.x[1] = DLL_PROCESS_DETACH;   // Reason
    ctx.x[2] = 0;                    // Reserved
    call_guest_prepared(ctx, callback);
}

[[noreturn]] void guest_exit(GuestContext& ctx, const OverrideBinding& b, std::uint32_t code, bool run_atexit,
                             bool quick) {
    trace_exit(b.name.c_str(), code, ctx.x[30]);
    if (quick) run_exit_table(ctx, exit_tables().at_quick_exit);
    if (run_atexit) {
        run_thread_local_exit_callback(ctx);
        run_exit_table(ctx, exit_tables().at_exit);
    }
    flush_host_streams();
    loader_process_exit(code);
}

R o_exit(GuestContext& ctx, const OverrideBinding& b) { guest_exit(ctx, b, static_cast<std::uint32_t>(ctx.x[0]), true, false); }
R o__exit(GuestContext& ctx, const OverrideBinding& b) { guest_exit(ctx, b, static_cast<std::uint32_t>(ctx.x[0]), false, false); }
R o_quick_exit(GuestContext& ctx, const OverrideBinding& b) { guest_exit(ctx, b, static_cast<std::uint32_t>(ctx.x[0]), false, true); }
// ExitProcess(code), RtlExitUserProcess(status)
R o_ExitProcess(GuestContext& ctx, const OverrideBinding& b) {
    trace_exit(b.name.c_str(), static_cast<std::uint32_t>(ctx.x[0]), ctx.x[30]);
    loader_process_exit(static_cast<std::uint32_t>(ctx.x[0]));
}
// abort(): SIGABRT (a guest handler may run), then exit code 3 through the
// loader instead of the host CRT's _exit.
R o_abort(GuestContext& ctx, const OverrideBinding& b) {
    trace_exit(b.name.c_str(), 3, ctx.x[30]);
    static const auto raise_fn = [] {
        HMODULE ucrt = GetModuleHandleW(L"ucrtbase.dll");
        return ucrt ? reinterpret_cast<int(__cdecl*)(int)>(GetProcAddress(ucrt, "raise")) : nullptr;
    }();
    if (raise_fn) raise_fn(22);   // SIGABRT
    flush_host_streams();
    loader_process_exit(3);
}
// TerminateProcess(process, code) / NtTerminateProcess(process, status): the
// current process goes through the loader's termination path (background
// work stopped, cache flush, stats; no DLL_PROCESS_DETACH, as natively);
// other processes, and NtTerminateProcess(NULL) (which ends the other
// threads only), go to the host.
R o_TerminateProcess(GuestContext& ctx, const OverrideBinding& b) {
    const auto handle = ctx.x[0];
    if (handle != 0 && is_current_process(handle)) {
        trace_exit(b.name.c_str(), static_cast<std::uint32_t>(ctx.x[1]), ctx.x[30]);
        loader_process_terminate(static_cast<std::uint32_t>(ctx.x[1]));
    }
    std::uint64_t result = 0;
    if (b.native) {
        using Fn = std::uint64_t (*)(std::uint64_t, std::uint64_t);
        result = reinterpret_cast<Fn>(const_cast<void*>(b.native))(handle, ctx.x[1]);
    }
    ctx.x[0] = result & 0xffffffffULL;
    return kReturn;
}
R o_cexit(GuestContext& ctx, const OverrideBinding&) {
    run_thread_local_exit_callback(ctx);
    run_exit_table(ctx, exit_tables().at_exit);
    flush_host_streams();
    return kReturn;
}
R o_c_exit(GuestContext&, const OverrideBinding&) { return kReturn; }

// ============================================================================
// CRT threads (same stack-size policy as CreateThread)
// ============================================================================

// uintptr_t _beginthreadex(security, stack_size, start, arglist, initflag, thrdaddr*)
R o_beginthreadex(GuestContext& ctx, const OverrideBinding&) {
    std::uint32_t id = 0;
    ctx.x[0] = start_guest_thread(ctx.x[2], ctx.x[3], ctx.x[1], static_cast<std::uint32_t>(ctx.x[4]), &id);
    if (ctx.x[0] && ctx.x[5]) *reinterpret_cast<unsigned*>(ctx.x[5]) = id;
    if (ctx.x[0] == 0) _set_errno(EAGAIN);
    return kReturn;
}
// uintptr_t _beginthread(start, stack_size, arglist): -1 on failure.
R o_beginthread(GuestContext& ctx, const OverrideBinding&) {
    const auto handle = start_guest_thread(ctx.x[0], ctx.x[2], ctx.x[1], 0, nullptr);
    if (handle == 0) _set_errno(EAGAIN);
    ctx.x[0] = handle ? handle : ~std::uint64_t{0};
    return kReturn;
}
R o_endthreadex(GuestContext& ctx, const OverrideBinding&) { exit_guest_thread(static_cast<std::uint32_t>(ctx.x[0])); }
R o_endthread(GuestContext&, const OverrideBinding&) { exit_guest_thread(0); }

// ============================================================================
// Floating-point environment: keep ctx.fpcr and ctx.fpsr in step with MXCSR
// ============================================================================
//
// These run the host (x64) CRT, which reads and writes MXCSR only. The
// architectural FPSR is ctx.fpsr | flags(MXCSR) (emit_fp.hpp), and ctx.fpsr
// keeps every flag folded in at a dispatcher exit; the next region entry
// loads them back into MXCSR. A host clear of the MXCSR flags alone is
// therefore undone by the next dispatcher round trip: feclearexcept, some
// arithmetic, fetestexcept then reported a stale FE_INVALID/FE_DIVBYZERO
// from whatever the thread computed earlier. (Blender's simple-expression
// drivers failed that way, a different driver depending on which thread
// evaluated it, so multithreaded renders differed.) Every host call below
// first moves the mirrored flags into MXCSR, where the host sees and owns
// all of them, and afterwards takes the host's result back into ctx.fpsr:
// the override thunk (generate_override_thunk) folds the MXCSR flags into
// ctx.fpsr on entry and loads MXCSR from ctx.fpcr/ctx.fpsr on return
// (src/fx/runtime/fp_boundary.hpp), so ctx.fpsr is what the guest sees.

// FPSR IOC DZC OFC UFC IXC <-> MXCSR IE ZE OE UE PE (emit_fp.hpp).
constexpr std::uint32_t kFpsrMirroredFlags = 0x1fu;
constexpr std::uint32_t kMxcsrStatusFlags = 0x3fu;

void move_fpsr_flags_to_mxcsr(GuestContext& ctx) noexcept {
    const std::uint32_t flags = ctx.fpsr & kFpsrMirroredFlags;
    if (flags != 0) _mm_setcsr(_mm_getcsr() | (flags & 1u) | ((flags & 0x1eu) << 1));
    ctx.fpsr &= ~kFpsrMirroredFlags;
}

// The host's flags after the call (IE ZE OE UE PE -> IOC DZC OFC UFC IXC).
void take_mxcsr_flags(GuestContext& ctx) noexcept {
    const unsigned csr = _mm_getcsr();
    ctx.fpsr = (ctx.fpsr & ~kFpsrMirroredFlags) | (csr & 1u) | ((csr >> 1) & 0x1eu);
}

std::uint64_t call_native_fenv(GuestContext& ctx, const OverrideBinding& b) {
    if (!b.native) return 0;
    using Fn = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
    return reinterpret_cast<Fn>(const_cast<void*>(b.native))(ctx.x[0], ctx.x[1], ctx.x[2], ctx.x[3]);
}

// Environment and exception-flag functions (fesetenv, feclearexcept,
// fetestexcept, _clearfp, _statusfp, ...): the host's semantics on the
// complete flag set.
R o_fenv(GuestContext& ctx, const OverrideBinding& b) {
    move_fpsr_flags_to_mxcsr(ctx);
    ctx.x[0] = call_native_fenv(ctx, b) & 0xffffffffULL;
    sync_fpcr_from_mxcsr(ctx);
    take_mxcsr_flags(ctx);
    return kReturn;
}

// Control-word functions (_control87, _controlfp, fesetround, ...): x64's
// _control87 drops the MXCSR status flags whenever the control word changes
// (TBB calls it around tasks on every worker). An A64 FPCR write leaves
// FPSR alone, so the flags are kept.
R o_fenv_control(GuestContext& ctx, const OverrideBinding& b) {
    move_fpsr_flags_to_mxcsr(ctx);
    const std::uint32_t flags = _mm_getcsr() & kMxcsrStatusFlags;
    ctx.x[0] = call_native_fenv(ctx, b) & 0xffffffffULL;
    sync_fpcr_from_mxcsr(ctx);
    _mm_setcsr((_mm_getcsr() & ~kMxcsrStatusFlags) | flags);
    take_mxcsr_flags(ctx);
    return kReturn;
}

struct OverrideEntry {
    const char* name;
    OverrideHandler handler;
};

// Sorted by name (strcmp order).
constexpr OverrideEntry kCrtOverrides[] = {
    {"ExitProcess", o_ExitProcess},
    {"NtTerminateProcess", o_TerminateProcess},
    {"RtlExitUserProcess", o_ExitProcess},
    {"TerminateProcess", o_TerminateProcess},
    {"ZwTerminateProcess", o_TerminateProcess},
    {"_Exit", o__exit},
    {"__control87_2", o_fenv_control},
    {"__getmainargs", o_getmainargs},
    {"__p___argc", o_p___argc},
    {"__p___argv", o_p___argv},
    {"__p___wargv", o_p___wargv},
    {"__p__acmdln", o_p__acmdln},
    {"__p__pgmptr", o_p__pgmptr},
    {"__p__wcmdln", o_p__wcmdln},
    {"__p__wpgmptr", o_p__wpgmptr},
    {"__wgetmainargs", o_wgetmainargs},
    {"_beginthread", o_beginthread},
    {"_beginthreadex", o_beginthreadex},
    {"_c_exit", o_c_exit},
    {"_cexit", o_cexit},
    {"_clearfp", o_fenv},
    {"_configure_narrow_argv", o_configure_argv},
    {"_configure_wide_argv", o_configure_argv},
    {"_control87", o_fenv_control},
    {"_controlfp", o_fenv_control},
    {"_controlfp_s", o_fenv_control},
    {"_crt_at_quick_exit", o_at_quick_exit},
    {"_crt_atexit", o_atexit},
    {"_endthread", o_endthread},
    {"_endthreadex", o_endthreadex},
    {"_execute_onexit_table", o_execute_onexit_table},
    {"_exit", o__exit},
    {"_fpreset", o_fenv},
    {"_get_narrow_winmain_command_line", o_get_narrow_winmain_command_line},
    {"_get_pgmptr", o_get_pgmptr},
    {"_get_wide_winmain_command_line", o_get_wide_winmain_command_line},
    {"_get_wpgmptr", o_get_wpgmptr},
    {"_initialize_onexit_table", o_initialize_onexit_table},
    {"_initterm", o_initterm},
    {"_initterm_e", o_initterm_e},
    {"_onexit", o_onexit},
    {"_register_onexit_function", o_register_onexit_function},
    {"_register_thread_local_exe_atexit_callback", o_register_thread_local_exe_atexit_callback},
    {"_set_controlfp", o_fenv_control},
    {"_statusfp", o_fenv},
    {"_statusfp2", o_fenv},
    {"abort", o_abort},
    {"at_quick_exit", o_at_quick_exit},
    {"atexit", o_atexit},
    {"exit", o_exit},
    {"feclearexcept", o_fenv},
    {"fegetenv", o_fenv},
    {"fegetexceptflag", o_fenv},
    {"feholdexcept", o_fenv},
    {"feraiseexcept", o_fenv},
    {"fesetenv", o_fenv},
    {"fesetexceptflag", o_fenv},
    {"fesetround", o_fenv_control},
    {"fetestexcept", o_fenv},
    {"feupdateenv", o_fenv},
    {"quick_exit", o_quick_exit},
};

} // namespace

static_assert(std::is_sorted(std::begin(kCrtOverrides), std::end(kCrtOverrides),
                             [](const OverrideEntry& a, const OverrideEntry& b) {
                                 return std::string_view(a.name) < std::string_view(b.name);
                             }),
              "kCrtOverrides must be sorted by name");

OverrideHandler find_crt_override(std::string_view name) noexcept {
    const auto* it = std::lower_bound(std::begin(kCrtOverrides), std::end(kCrtOverrides), name,
        [](const OverrideEntry& e, std::string_view key) { return std::string_view(e.name) < key; });
    if (it != std::end(kCrtOverrides) && std::string_view(it->name) == name) return it->handler;
    return nullptr;
}

std::size_t crt_override_count() noexcept { return std::size(kCrtOverrides); }

// Exposed for tests: CRT command-line parsing.
std::vector<std::wstring> parse_guest_command_line(const wchar_t* text) { return parse_command_line(text); }

} // namespace transmute::fx::abi
