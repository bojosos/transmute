// Stand-ins for the runtime, abi and cache components (test only).

#include "test_support.hpp"

#include "fx/cache/cache.hpp"
#include "fx/common/interpreter.hpp"
#include "fx/runtime/runtime.hpp"
#include "fx/loader/loader.hpp"
#include "fx/testing/test.hpp"

#include <windows.h>
#include <dbghelp.h>

#include <cstdio>
#include <memory>
#include <stdexcept>

namespace fxl_test {
namespace {

using namespace transmute::fx;

struct FakeThread {
    std::unique_ptr<GuestContext> ctx;
    std::vector<std::uint8_t> teb;
    std::uint8_t* stack = nullptr;
    std::size_t stack_size = 0;
    ~FakeThread() {
        if (stack) VirtualFree(stack, 0, MEM_RELEASE);
    }
};

thread_local std::unique_ptr<FakeThread> t_thread;

constexpr std::size_t kFakeRegion = 1 << 20;

struct FakeRegion {
    std::uint8_t* base = nullptr;
    std::vector<HostFn> functions;
};

FakeRegion& fakes() {
    static FakeRegion* region = [] {
        auto* r = new FakeRegion;
        r->base = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kFakeRegion, MEM_RESERVE, PAGE_NOACCESS));
        return r;
    }();
    return *region;
}

std::map<std::tuple<std::string, std::string, const void*>, std::uint64_t>& bind_cache() {
    static auto* cache = new std::map<std::tuple<std::string, std::string, const void*>, std::uint64_t>;
    return *cache;
}

std::string lower_ascii(std::string_view s) {
    std::string out(s);
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    }
    return out;
}

// SEH guard around one interpreted instruction (no C++ objects here).
int interpret_guarded(GuestContext* ctx, DWORD* code) {
    __try {
        interpret_one(*ctx);
        return 0;
    } __except (GetExceptionCode() == 0xE06D7363 ? EXCEPTION_CONTINUE_SEARCH
                                                  : (*code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER)) {
        return 1;
    }
}

// Prints a symbolized backtrace for crashes in the test process (no debugger
// is assumed to be available on the test machine).
LONG WINAPI crash_report(EXCEPTION_POINTERS* info) {
    const auto* record = info->ExceptionRecord;
    std::fprintf(stderr, "fx_loader_tests: unhandled exception 0x%08lx at %p (access %llx)\n",
                 record->ExceptionCode, record->ExceptionAddress,
                 record->NumberParameters > 1 ? static_cast<unsigned long long>(record->ExceptionInformation[1]) : 0ull);
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitialize(process, nullptr, TRUE);
    CONTEXT context = *info->ContextRecord;
    STACKFRAME64 frame{};
    frame.AddrPC.Offset = context.Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context.Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context.Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    for (int i = 0; i < 40; ++i) {
        if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, GetCurrentThread(), &frame, &context, nullptr,
                         SymFunctionTableAccess64, SymGetModuleBase64, nullptr) ||
            frame.AddrPC.Offset == 0) {
            break;
        }
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD line_displacement = 0;
        const bool named = SymFromAddr(process, frame.AddrPC.Offset, &displacement, symbol) != FALSE;
        const bool lined = SymGetLineFromAddr64(process, frame.AddrPC.Offset, &line_displacement, &line) != FALSE;
        std::fprintf(stderr, "  #%02d %016llx %s+0x%llx %s:%lu\n", i,
                     static_cast<unsigned long long>(frame.AddrPC.Offset), named ? symbol->Name : "?",
                     static_cast<unsigned long long>(displacement), lined ? line.FileName : "",
                     lined ? line.LineNumber : 0ul);
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}

const bool g_crash_reporter = [] {
    SetUnhandledExceptionFilter(crash_report);
    return true;
}();

} // namespace

Stubs& stubs() {
    static Stubs* s = new Stubs;
    return *s;
}

void reset_stubs() {
    auto& s = stubs();
    std::lock_guard lock(s.lock);
    s.host_binds.clear();
    s.replacements.clear();
    s.replacement_queries.clear();
    s.emulations.clear();
    s.cache_loaded.clear();
    s.cache_unloaded.clear();
    s.invalidated.clear();
    s.guest_calls.clear();
    s.execute = true;
    s.result = 1;
    s.steps = 0;
}

std::uint64_t fake_host(HostFn fn) {
    auto& s = stubs();
    std::lock_guard lock(s.lock);
    auto& r = fakes();
    const std::uint64_t address = reinterpret_cast<std::uint64_t>(r.base) + r.functions.size() * 16;
    r.functions.push_back(std::move(fn));
    return address;
}

bool is_fake_host(std::uint64_t address) {
    const auto base = reinterpret_cast<std::uint64_t>(fakes().base);
    return address >= base && address < base + kFakeRegion;
}

std::uint64_t run_guest(std::uint64_t entry, std::initializer_list<std::uint64_t> args) {
    return call_guest(entry, std::span<const std::uint64_t>(args.begin(), args.size()));
}

void end_thread_context() {
    if (!t_thread) return;
    loader_thread_detach(*t_thread->ctx);
    t_thread.reset();
}

namespace {
std::wstring backslashes(std::wstring path) {
    for (auto& c : path) {
        if (c == L'/') c = L'\\';
    }
    return path;
}
} // namespace

std::wstring fixture(const wchar_t* name) {
    return backslashes(std::wstring(L"" FXL_FIXTURE_DIR) + L"\\" + name);
}

std::wstring legacy_fixture(const wchar_t* name) {
    return backslashes(std::wstring(L"" FXL_LEGACY_FIXTURE_DIR) + L"\\" + name);
}

bool exists(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

void fresh_loader() {
    loader_reset_for_testing();
    loader_set_missing_import_handler(nullptr);
    reset_stubs();
}

} // namespace fxl_test

// ============================================================================
// Runtime stand-ins
// ============================================================================

namespace transmute::fx {

using namespace fxl_test;

void runtime_initialize(const RuntimeOptions&) {
    std::lock_guard lock(stubs().lock);
    ++stubs().runtime_initialized;
}

GuestContext* current_context() noexcept { return t_thread ? t_thread->ctx.get() : nullptr; }

GuestContext& ensure_thread_context() {
    if (t_thread) return *t_thread->ctx;
    auto thread = std::make_unique<FakeThread>();
    thread->ctx = std::make_unique<GuestContext>();
    thread->teb.assign(0x2000, 0);
    thread->stack_size = 1 << 20;
    thread->stack = static_cast<std::uint8_t*>(
        VirtualAlloc(nullptr, thread->stack_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    {
        std::lock_guard lock(stubs().lock);
        stubs().stack_reserve_seen = guest_image_stack_reserve();
        stubs().stack_commit_seen = guest_image_stack_commit();
    }
    GuestContext& ctx = *thread->ctx;
    ctx.shadow_teb = thread->teb.data();
    ctx.host_teb = NtCurrentTeb();
    ctx.guest_stack_base = reinterpret_cast<std::uint64_t>(thread->stack) + thread->stack_size;
    ctx.guest_stack_limit = reinterpret_cast<std::uint64_t>(thread->stack);
    ctx.x[31] = ctx.guest_stack_base;
    ctx.fpcr = 0;
    t_thread = std::move(thread);
    loader_thread_attach(ctx);   // as the runtime does for a new thread
    return ctx;
}

std::uint64_t call_guest(std::uint64_t entry, std::span<const std::uint64_t> args) {
    GuestContext& ctx = ensure_thread_context();
    auto& s = stubs();
    {
        std::lock_guard lock(s.lock);
        s.guest_calls.push_back({entry, std::vector<std::uint64_t>(args.begin(), args.end())});
        if (!s.execute) return s.result;
    }
    if (entry == 0) throw std::runtime_error("call_guest(0)");
    const auto saved = std::make_unique<GuestContext>(ctx);
    for (std::size_t i = 0; i < 8; ++i) ctx.x[i] = i < args.size() ? args[i] : 0;
    ctx.x[18] = reinterpret_cast<std::uint64_t>(ctx.shadow_teb);
    ctx.x[30] = kReturnToHostPc;
    ctx.x[31] = (ctx.x[31] - 256) & ~std::uint64_t{15};
    ctx.pc = entry;
    std::uint64_t steps = 0;
    while (ctx.pc != kReturnToHostPc) {
        if (++steps > 50'000'000) throw std::runtime_error("guest call did not return");
        if (is_fake_host(ctx.pc)) {
            HostFn fn;
            {
                std::lock_guard lock(s.lock);
                const auto index = (ctx.pc - reinterpret_cast<std::uint64_t>(fakes().base)) / 16;
                if (index >= fakes().functions.size()) throw std::runtime_error("bad fake host address");
                fn = fakes().functions[index];
            }
            ctx.x[0] = fn(ctx);
            ctx.pc = ctx.x[30];
            continue;
        }
        DWORD code = 0;
        if (interpret_guarded(&ctx, &code)) {
            char message[96];
            std::snprintf(message, sizeof(message), "guest exception 0x%08lx at pc 0x%llx", code,
                          static_cast<unsigned long long>(ctx.pc));
            ctx = *saved;
            throw std::runtime_error(message);
        }
    }
    const std::uint64_t result = ctx.x[0];
    {
        std::lock_guard lock(s.lock);
        s.steps += steps;
    }
    ctx = *saved;
    return result;
}

RuntimeStats runtime_stats() { return {}; }
void runtime_profile_flush() {}
void runtime_prepare_exit() {}
bool runtime_exit_safe() noexcept { return true; }
CacheStats cache_stats() noexcept { return {}; }

void invalidate_guest_code(std::uint64_t address, std::uint64_t size) {
    std::lock_guard lock(stubs().lock);
    stubs().invalidated.emplace_back(address, size);
}

void* allocate_code(std::size_t size, std::size_t alignment) {
    static std::mutex lock;
    static std::uint8_t* cursor = nullptr;
    static std::uint8_t* end = nullptr;
    std::lock_guard guard(lock);
    auto align = [&](std::uint8_t* p) {
        const auto a = reinterpret_cast<std::uintptr_t>(p);
        return reinterpret_cast<std::uint8_t*>((a + alignment - 1) / alignment * alignment);
    };
    if (!cursor || align(cursor) + size > end) {
        cursor = static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, 1 << 20, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
        end = cursor + (1 << 20);
    }
    std::uint8_t* p = align(cursor);
    cursor = p + size;
    return p;
}

// ============================================================================
// ABI stand-ins
// ============================================================================

std::uint64_t bind_host_function(std::string_view dll, std::string_view function,
                                 const void* host_address) {
    auto& s = stubs();
    std::lock_guard lock(s.lock);
    const auto key = std::make_tuple(lower_ascii(dll), std::string(function), host_address);
    auto& cache = bind_cache();
    std::uint64_t guest = 0;
    if (const auto it = cache.find(key); it != cache.end()) {
        guest = it->second;
    } else {
        const std::string name(function);
        guest = fake_host([name](GuestContext& ctx) -> std::uint64_t {
            HostFn emulation;
            {
                std::lock_guard inner(stubs().lock);
                const auto it = stubs().emulations.find(name);
                if (it != stubs().emulations.end()) emulation = it->second;
            }
            return emulation ? emulation(ctx) : 0;
        });
        cache.emplace(key, guest);
    }
    s.host_binds.push_back({std::string(dll), std::string(function), host_address, guest});
    return guest;
}

std::uint64_t bind_unknown_host_target(const void* host_address) {
    using Fn = std::uint64_t (*)(std::uint64_t, std::uint64_t, std::uint64_t, std::uint64_t);
    const auto fn = reinterpret_cast<Fn>(const_cast<void*>(host_address));
    return fake_host([fn](GuestContext& ctx) { return fn(ctx.x[0], ctx.x[1], ctx.x[2], ctx.x[3]); });
}

std::uint64_t abi_guest_import_replacement(std::string_view dll, std::string_view function) {
    auto& s = stubs();
    std::lock_guard lock(s.lock);
    s.replacement_queries.push_back(std::string(dll) + "!" + std::string(function));
    const auto it = s.replacements.find(std::string(function));
    return it == s.replacements.end() ? 0 : it->second;
}

// ============================================================================
// Cache stand-ins
// ============================================================================

void cache_module_loaded(const GuestModule& module) {
    std::lock_guard lock(stubs().lock);
    stubs().cache_loaded.push_back(module.name);
}

void cache_module_unloaded(const GuestModule& module) {
    std::lock_guard lock(stubs().lock);
    stubs().cache_unloaded.push_back(module.name);
}

void cache_module_mapped(const GuestModule&) {}

void cache_flush_journals() noexcept {}

void cache_flush() {
    std::lock_guard lock(stubs().lock);
    ++stubs().cache_flushes;
    char marker[8];
    if (GetEnvironmentVariableA("FXL_CHILD_REPORT", marker, sizeof(marker))) {
        std::printf("stack=%llu,%llu\n", static_cast<unsigned long long>(stubs().stack_reserve_seen),
                    static_cast<unsigned long long>(stubs().stack_commit_seen));
        std::printf("cache_flush\n");
        std::fflush(stdout);
    }
}

} // namespace transmute::fx

// ---- host TLS callbacks (test process) --------------------------------------------------
namespace {

void write_stdout(const std::string& text) {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
}

// Like the real runtime: guest thread teardown runs from a TLS callback on
// DLL_THREAD_DETACH, i.e. inside LdrShutdownThread with the host loader lock held.
void NTAPI stub_thread_detach(PVOID, DWORD reason, PVOID) {
    if (reason == DLL_THREAD_DETACH) fxl_test::end_thread_context();
}

// After the loader's process-detach hook (.CRT$XLB): child-process tests
// report the guest state observed after guest DLL_PROCESS_DETACH.
void NTAPI stub_process_detach_report(PVOID, DWORD reason, PVOID) {
    if (reason != DLL_PROCESS_DETACH) return;
    char marker[8];
    if (!GetEnvironmentVariableA("FXL_CHILD_REPORT", marker, sizeof(marker))) return;
    using namespace transmute::fx;
    if (const auto module = guest_get_module_handle(L"fxl_tls.dll")) {
        const auto count = guest_get_proc_address(module, "g_event_count");
        const auto events = guest_get_proc_address(module, "g_events");
        if (count && events) {
            const long n = *reinterpret_cast<const volatile long*>(count);
            std::string line = "events:";
            char item[16];
            for (long i = 0; i < n; ++i) {
                std::snprintf(item, sizeof(item), " %lx", reinterpret_cast<const volatile unsigned long*>(events)[i]);
                line += item;
            }
            write_stdout(line + "\n");
        }
    }
    write_stdout("process_detach_done\n");
}

} // namespace

extern "C" void NTAPI fxl_stub_thread_detach(PVOID m, DWORD r, PVOID x) { stub_thread_detach(m, r, x); }
extern "C" void NTAPI fxl_stub_process_report(PVOID m, DWORD r, PVOID x) { stub_process_detach_report(m, r, x); }
#pragma comment(linker, "/INCLUDE:_tls_used")
#pragma comment(linker, "/INCLUDE:fxl_stub_tls_detach")
#pragma comment(linker, "/INCLUDE:fxl_stub_tls_report")
#pragma const_seg(push)
#pragma const_seg(".CRT$XLB")
extern "C" const PIMAGE_TLS_CALLBACK fxl_stub_tls_detach = &fxl_stub_thread_detach;
#pragma const_seg(".CRT$XLY")
extern "C" const PIMAGE_TLS_CALLBACK fxl_stub_tls_report = &fxl_stub_process_report;
#pragma const_seg(pop)
