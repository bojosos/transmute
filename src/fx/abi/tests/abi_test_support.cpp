// Test support and component stubs for fx_abi_tests (owner: abi).
//
// fx_abi calls into the runtime, loader and seh components. The tests link
// none of them: this file provides minimal implementations of exactly the
// entry points the library references.

#include "fx/abi/tests/abi_test_support.hpp"

#include "fx/common/x64_assembler.hpp"
#include "fx/loader/loader.hpp"
#include "fx/runtime/runtime.hpp"
#include "transmute/fx/process.hpp"

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>

namespace transmute::fx {
namespace abi_test {
namespace {

using x64::Gp;
using x64::ptr;

std::mutex g_mutex;
thread_local GuestContext* t_current = nullptr;

Records& records_storage() {
    static Records r;
    return r;
}

// ---- dispatcher -----------------------------------------------------------------

struct Dispatcher {
    using Entry = void (*)(void* biased_context, std::uint64_t code);
    Entry entry{};
    const void* return_stub{};
    const void* hit_stub{};
};

constexpr std::uint32_t kHitMarker = 0x5253;   // "RS"

const Dispatcher& dispatcher() {
    static const Dispatcher d = [] {
        x64::Assembler a;
        const Gp saved[] = {Gp::rbx, Gp::rbp, Gp::rdi, Gp::rsi, Gp::r12, Gp::r13, Gp::r14, Gp::r15};
        const auto host_rsp = ctx_disp(offsetof(GuestContext, host_rsp));
        // entry(rcx = biased context, rdx = code)
        for (auto r : saved) a.push(r);
        a.sub(64, Gp::rsp, 40);                 // 16-aligned, 32 bytes of shadow space
        a.mov(64, Gp::r15, Gp::rcx);
        a.mov(64, ptr(Gp::r15, host_rsp), Gp::rsp);
        a.jmp(Gp::rdx);
        a.align(16);
        const auto ret_label = a.bound_label();
        a.mov(64, Gp::rsp, ptr(Gp::r15, host_rsp));
        a.add(64, Gp::rsp, 40);
        for (int i = 7; i >= 0; --i) a.pop(saved[i]);
        a.ret();
        a.align(16);
        const auto hit_label = a.bound_label();
        a.mov(32, ptr(Gp::r15, ctx_disp(offsetof(GuestContext, exit_reason))), static_cast<std::int32_t>(kHitMarker));
        a.jmp(ret_label);
        a.finalize();
        auto* memory = static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!memory) throw std::runtime_error("cannot allocate the test dispatcher");
        std::memcpy(memory, a.data(), a.size());
        Dispatcher out;
        out.entry = reinterpret_cast<Dispatcher::Entry>(memory);
        out.return_stub = memory + a.offset_of(ret_label);
        out.hit_stub = memory + a.offset_of(hit_label);
        return out;
    }();
    return d;
}

// ---- fake guest functions ----------------------------------------------------------

constexpr std::size_t kGuestSpace = 1 << 20;

struct GuestSpace {
    std::uint8_t* base{};
    std::map<std::uint64_t, GuestBody> bodies;
    std::map<std::uint64_t, std::uint64_t> calls;
    std::uint64_t next{};
};

GuestSpace& guest_space() {
    static GuestSpace* space = [] {
        auto* s = new GuestSpace();
        s->base = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, kGuestSpace, MEM_RESERVE, PAGE_NOACCESS));
        return s;
    }();
    return *space;
}

std::vector<std::pair<std::uint64_t, std::uint64_t>>& registered_ranges() {
    static std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
    return ranges;
}

std::vector<GuestModule>& test_modules() {
    static std::vector<GuestModule> modules;
    return modules;
}
std::uint64_t g_stack_reserve = 0;

std::vector<std::unique_ptr<TestThread>>& adopted_threads() {
    static std::vector<std::unique_ptr<TestThread>> threads;
    return threads;
}

// Arena for allocate_code.
struct CodeArena {
    std::uint8_t* base{};
    std::size_t used{};
    std::size_t size{};
};

} // namespace

std::unique_ptr<TestThread> make_thread() {
    auto t = std::make_unique<TestThread>();
    t->ctx = std::make_unique<GuestContext>();
    std::memset(t->ctx.get(), 0, sizeof(GuestContext));
    t->stack.assign(1 << 20, 0);
    t->teb.assign(0x2000, 0);
    auto& ctx = *t->ctx;
    const auto top = reinterpret_cast<std::uint64_t>(t->stack.data() + t->stack.size());
    ctx.guest_stack_base = top;
    ctx.guest_stack_limit = reinterpret_cast<std::uint64_t>(t->stack.data());
    ctx.x[31] = (top - 0x400) & ~std::uint64_t{15};
    ctx.shadow_teb = t->teb.data();
    ctx.host_teb = NtCurrentTeb();
    ctx.stub_indirect = dispatcher().return_stub;
    ctx.stub_dispatch_exit = dispatcher().return_stub;
    ctx.fpcr = 0;
    return t;
}

bool run_guest_call(GuestContext& ctx, std::uint64_t code, std::uint64_t return_pc) {
    const auto& d = dispatcher();
    ctx.stub_indirect = d.return_stub;
    ctx.x[30] = return_pc;
    ctx.exit_reason = 0;
    GuestContext* previous = t_current;
    t_current = &ctx;
    const auto saved_host_rsp = ctx.host_rsp;
    d.entry(reinterpret_cast<char*>(&ctx) + kContextBias, code);
    ctx.host_rsp = saved_host_rsp;
    t_current = previous;
    const bool hit = ctx.exit_reason == kHitMarker;
    ctx.exit_reason = 0;
    return hit;
}

void push_rsb_hit(GuestContext& ctx, std::uint64_t guest_return) {
    const auto index = ctx.rsb_top & (kRsbEntries - 1);
    ctx.rsb[index].guest_return = guest_return;
    ctx.rsb[index].host_entry = dispatcher().hit_stub;
    ctx.rsb_top = (index + 1) & (kRsbEntries - 1);
}

const void* return_stub() { return dispatcher().return_stub; }

std::uint64_t make_guest_function(GuestBody body) {
    auto& s = guest_space();
    std::lock_guard lock(g_mutex);
    const auto address = reinterpret_cast<std::uint64_t>(s.base) + 16 * (++s.next);
    s.bodies.emplace(address, std::move(body));
    return address;
}

std::uint64_t guest_call_count(std::uint64_t function) {
    std::lock_guard lock(g_mutex);
    const auto it = guest_space().calls.find(function);
    return it == guest_space().calls.end() ? 0 : it->second;
}

Records& records() { return records_storage(); }
void reset_records() {
    std::lock_guard lock(g_mutex);
    records_storage() = Records{};
}

void set_test_modules(std::vector<GuestModule> modules) {
    std::lock_guard lock(g_mutex);
    test_modules() = std::move(modules);
}
void set_test_stack_reserve(std::uint64_t reserve) { g_stack_reserve = reserve; }

} // namespace abi_test

using namespace abi_test;

// ============================================================================
// Runtime stubs
// ============================================================================

void* allocate_code(std::size_t size, std::size_t alignment) {
    static std::mutex mutex;
    static abi_test::CodeArena arena;
    std::lock_guard lock(mutex);
    if (arena.base == nullptr) {
        arena.size = 64ull << 20;
        arena.base = static_cast<std::uint8_t*>(
            VirtualAlloc(nullptr, arena.size, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
        if (!arena.base) throw std::bad_alloc();
    }
    arena.used = (arena.used + alignment - 1) & ~(alignment - 1);
    if (arena.used + size > arena.size) throw std::bad_alloc();
    void* p = arena.base + arena.used;
    arena.used += size;
    return p;
}

void register_host_entry(std::uint64_t, const void*) {
    std::lock_guard lock(g_mutex);
    ++records().host_entries;
}

GuestContext* current_context() noexcept { return t_current; }

GuestContext& ensure_thread_context() {
    if (t_current) return *t_current;
    auto thread = make_thread();
    t_current = thread->ctx.get();
    std::lock_guard lock(g_mutex);
    ++records().contexts_created;
    adopted_threads().push_back(std::move(thread));
    return *t_current;
}

void call_guest_prepared(GuestContext& ctx, std::uint64_t entry) {
    GuestBody body;
    {
        std::lock_guard lock(g_mutex);
        auto& s = guest_space();
        const auto it = s.bodies.find(entry);
        if (it != s.bodies.end()) {
            body = it->second;
            ++s.calls[entry];
        }
    }
    if (body) {
        const auto sp = ctx.x[31];
        body(ctx);
        // Behave like compiled guest code: caller-saved state is garbage
        // afterwards (results in x0/x1 and v0-v3 survive), x30 is the
        // return-to-host address.
        for (unsigned n = 2; n <= 17; ++n) if (n != 8) ctx.x[n] = 0xdead0000ull + n;
        for (unsigned n = 4; n < 8; ++n) ctx.v[n][0] = ctx.v[n][1] = 0xbad0000ull + n;
        ctx.x[30] = kReturnToHostPc;
        ctx.x[31] = sp;
        return;
    }
    if (is_abi_thunk(entry)) {
        run_guest_call(ctx, entry, kReturnToHostPc);
        return;
    }
    throw std::runtime_error("call_guest_prepared: unknown guest entry");
}

void invalidate_guest_code(std::uint64_t address, std::uint64_t size) {
    std::lock_guard lock(g_mutex);
    records().invalidated.emplace_back(address, size);
}

namespace {
struct ThreadStart {
    GuestThreadStart start;
};
DWORD WINAPI test_thread_proc(void* parameter) {
    auto* start = static_cast<ThreadStart*>(parameter);
    auto& ctx = ensure_thread_context();
    ctx.x[0] = start->start.argument;
    const auto entry = start->start.entry;
    delete start;
    call_guest_prepared(ctx, entry);
    return static_cast<DWORD>(ctx.x[0]);
}
} // namespace

std::uint64_t create_guest_thread(const GuestThreadStart& start, std::uint32_t* thread_id) {
    {
        std::lock_guard lock(g_mutex);
        records().threads.push_back(start);
    }
    DWORD id = 0;
    HANDLE h = CreateThread(nullptr, 0, test_thread_proc, new ThreadStart{start},
                            start.creation_flags & CREATE_SUSPENDED, &id);
    if (thread_id) *thread_id = id;
    return reinterpret_cast<std::uint64_t>(h);
}

[[noreturn]] void exit_guest_thread(std::uint32_t exit_code) { ExitThread(exit_code); }

void register_guest_code_memory(std::uint64_t base, std::uint64_t size) {
    std::lock_guard lock(g_mutex);
    records().registered_code.emplace_back(base, size);
    registered_ranges().emplace_back(base, base + size);
}

void unregister_guest_code_memory(std::uint64_t base, std::uint64_t size) {
    std::lock_guard lock(g_mutex);
    records().unregistered_code.emplace_back(base, size);
    auto& ranges = registered_ranges();
    ranges.erase(std::remove_if(ranges.begin(), ranges.end(),
                                [&](const auto& r) { return r.first >= base && r.second <= base + size; }),
                 ranges.end());
}

// ---- runtime.hpp: guest stacks and FP mapping ------------------------------------

GuestStack allocate_guest_stack(std::uint64_t reserve) {
    if (reserve == 0) reserve = 1ull << 20;
    constexpr std::uint64_t guard = 64 * 1024;
    auto* memory = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, reserve + guard, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!memory) throw std::bad_alloc();
    GuestStack stack;
    stack.allocation = reinterpret_cast<std::uint64_t>(memory);
    stack.size = reserve + guard;
    stack.guard_end = stack.allocation + guard;
    stack.base = stack.allocation + stack.size;
    stack.limit = stack.guard_end;
    std::lock_guard lock(g_mutex);
    ++records().guest_stacks_allocated;
    return stack;
}

void free_guest_stack(const GuestStack& stack) {
    if (stack.allocation) VirtualFree(reinterpret_cast<void*>(stack.allocation), 0, MEM_RELEASE);
    std::lock_guard lock(g_mutex);
    ++records().guest_stacks_freed;
}

GuestStack switch_guest_stack(const GuestStack& stack) {
    GuestContext* ctx = t_current;
    if (!ctx) throw std::logic_error("switch_guest_stack: no context");
    GuestStack previous;
    previous.base = ctx->guest_stack_base;
    previous.limit = ctx->guest_stack_limit;
    previous.allocation = ctx->guest_stack_limit;
    previous.guard_end = ctx->guest_stack_limit;
    previous.size = previous.base - previous.allocation;
    ctx->guest_stack_base = stack.base;
    ctx->guest_stack_limit = stack.limit;
    auto* tib = static_cast<NT_TIB*>(ctx->shadow_teb);
    tib->StackBase = reinterpret_cast<void*>(stack.base);
    tib->StackLimit = reinterpret_cast<void*>(stack.limit);
    return previous;
}

std::uint32_t mxcsr_for_guest(std::uint32_t fpcr, std::uint32_t) noexcept {
    static constexpr std::uint32_t kRc[4] = {0, 2, 1, 3};   // FPCR RMode -> MXCSR RC
    std::uint32_t csr = 0x1f80 | (kRc[(fpcr >> 22) & 3] << 13);
    if (fpcr & (1u << 24)) csr |= (1u << 15) | (1u << 6);
    return csr;
}

bool is_guest_code(std::uint64_t address) noexcept {
    const auto base = reinterpret_cast<std::uint64_t>(guest_space().base);
    if (address >= base && address < base + kGuestSpace) return true;
    std::lock_guard lock(g_mutex);
    for (const auto& r : registered_ranges())
        if (address >= r.first && address < r.second) return true;
    for (const auto& m : test_modules())
        if (address >= m.text_begin && address < m.text_end) return true;
    return false;
}

// ============================================================================
// Loader stubs
// ============================================================================

const GuestModule* find_guest_module(std::uint64_t address) noexcept {
    for (const auto& m : test_modules())
        if (address >= m.base && address < m.base + m.size) return &m;
    return nullptr;
}

std::vector<const GuestModule*> guest_modules() {
    std::vector<const GuestModule*> out;
    for (const auto& m : test_modules()) out.push_back(&m);
    return out;
}

std::uint64_t guest_load_library(std::wstring_view name, std::uint32_t flags) {
    return reinterpret_cast<std::uint64_t>(LoadLibraryExW(std::wstring(name).c_str(), nullptr, flags));
}

std::uint64_t guest_get_module_handle(std::wstring_view name) {
    for (const auto& m : test_modules())
        if (_wcsicmp(m.name.c_str(), std::wstring(name).c_str()) == 0) return m.base;
    return reinterpret_cast<std::uint64_t>(GetModuleHandleW(std::wstring(name).c_str()));
}

std::uint64_t guest_get_proc_address(std::uint64_t module, const char* name) {
    auto* p = GetProcAddress(reinterpret_cast<HMODULE>(module), name);
    if (!p) return 0;
    return bind_host_function("test-module.dll", reinterpret_cast<std::uintptr_t>(name) < 0x10000 ? "" : name,
                              reinterpret_cast<const void*>(p));
}

bool guest_free_library(std::uint64_t module) {
    return FreeLibrary(reinterpret_cast<HMODULE>(module)) != FALSE;
}
bool guest_pin_library(std::uint64_t module) {
    return find_guest_module(module) != nullptr;
}

const std::wstring& guest_command_line() {
    static const std::wstring line = L"\"C:\\guest dir\\app.exe\" one \"two words\" a\\\\\\\"b \"c\"\"d\" e\\f";
    return line;
}

const std::wstring& guest_image_path() {
    static const std::wstring path = L"C:\\guest dir\\app.exe";
    return path;
}

std::uint64_t guest_image_stack_reserve() noexcept { return g_stack_reserve; }

// ---- loader.hpp ------------------------------------------------------------------

namespace {
void loader_record(std::string call) {
    std::lock_guard lock(g_mutex);
    records().loader_calls.push_back(std::move(call));
}
std::string narrow(std::wstring_view text) {
    std::string out;
    for (const wchar_t ch : text) out.push_back(static_cast<char>(ch));   // ASCII test paths
    return out;
}
} // namespace

bool guest_disable_thread_library_calls(std::uint64_t module) {
    loader_record("DisableThreadLibraryCalls");
    return find_guest_module(module) != nullptr;
}

std::uint64_t guest_add_dll_directory(std::wstring_view directory) {
    loader_record("AddDllDirectory " + narrow(directory));
    return 0xc00c1e;
}

bool guest_remove_dll_directory(std::uint64_t cookie) {
    loader_record("RemoveDllDirectory");
    return cookie == 0xc00c1e;
}

bool guest_set_dll_directory(std::wstring_view directory) {
    loader_record("SetDllDirectory " + narrow(directory));
    return true;
}

std::wstring guest_module_file_name(std::uint64_t module) {
    if (module == 0) return guest_image_path();
    for (const auto& m : test_modules())
        if (m.base == module) return m.path;
    return {};
}

std::uint64_t guest_module_from_address(std::uint64_t address) noexcept {
    const auto* m = find_guest_module(address);
    return m ? m->base : 0;
}

std::uint64_t guest_resolve_delay_loaded_api(std::uint64_t parent_module, std::uint64_t descriptor,
                                             std::uint64_t failure_dll_hook, std::uint64_t failure_system_hook,
                                             std::uint64_t thunk_address, std::uint32_t flags) {
    loader_record("ResolveDelayLoadedAPI");
    std::lock_guard lock(g_mutex);
    records().delay_load_args = {parent_module, descriptor, failure_dll_hook, failure_system_hook, thunk_address, flags};
    return 0xde1a7;
}

[[noreturn]] void loader_process_exit(std::uint32_t exit_code) {
    {
        std::lock_guard lock(g_mutex);
        records().last_exit_code = exit_code;
        ++records().process_exits;
    }
    ExitThread(exit_code);   // tests only exit from worker threads
}

[[noreturn]] void loader_process_terminate(std::uint32_t exit_code) {
    {
        std::lock_guard lock(g_mutex);
        records().last_exit_code = exit_code;
        ++records().process_terminations;
    }
    ExitThread(exit_code);   // tests only exit from worker threads
}

// ============================================================================
// SEH stubs
// ============================================================================

namespace {
void seh_record(const char* name) {
    std::lock_guard lock(g_mutex);
    records().seh_calls.emplace_back(name);
}
} // namespace

void guest_raise_exception(GuestContext& ctx, std::uint32_t code, std::uint32_t, std::uint32_t count,
                           const std::uint64_t* arguments) {
    seh_record("raise");
    ctx.x[0] = code;
    ctx.x[1] = count ? arguments[0] : 0;
}

GuestResolution guest_RaiseException(GuestContext& ctx) {
    seh_record("RaiseException");
    // Pretend a vectored handler returned EXCEPTION_CONTINUE_EXECUTION at
    // the caller (ctx.pc is the return address set by the override thunk).
    ctx.x[0] = 0;
    return GuestResolution::continue_at_pc;
}
GuestResolution guest_RtlCaptureContext(GuestContext& ctx) {
    seh_record("RtlCaptureContext");
    *reinterpret_cast<std::uint64_t*>(ctx.x[0]) = ctx.pc;
    return GuestResolution::continue_at_caller;
}
GuestResolution guest_RtlRestoreContext(GuestContext& ctx) {
    seh_record("RtlRestoreContext");
    ctx.pc = *reinterpret_cast<const std::uint64_t*>(ctx.x[0]);
    return GuestResolution::continue_at_pc;
}
GuestResolution guest_longjmp(GuestContext& ctx) {
    seh_record("longjmp");
    ctx.pc = *reinterpret_cast<const std::uint64_t*>(ctx.x[0]);
    ctx.x[0] = ctx.x[1] ? ctx.x[1] : 1;
    return GuestResolution::continue_at_pc;
}
#define FX_SEH_STUB(api)                                    \
    GuestResolution guest_##api(GuestContext& ctx) {        \
        seh_record(#api);                                   \
        ctx.x[0] = 0x5e40;                                  \
        return GuestResolution::continue_at_caller;         \
    }
FX_SEH_STUB(RtlVirtualUnwind)
FX_SEH_STUB(RtlLookupFunctionEntry)
FX_SEH_STUB(RtlUnwind)
FX_SEH_STUB(RtlUnwindEx)
FX_SEH_STUB(RtlAddFunctionTable)
FX_SEH_STUB(RtlDeleteFunctionTable)
FX_SEH_STUB(RtlInstallFunctionTableCallback)
FX_SEH_STUB(RtlAddGrowableFunctionTable)
FX_SEH_STUB(RtlGrowFunctionTable)
FX_SEH_STUB(RtlDeleteGrowableFunctionTable)
FX_SEH_STUB(SetUnhandledExceptionFilter)
FX_SEH_STUB(UnhandledExceptionFilter)
FX_SEH_STUB(AddVectoredExceptionHandler)
FX_SEH_STUB(AddVectoredContinueHandler)
FX_SEH_STUB(RemoveVectoredExceptionHandler)
FX_SEH_STUB(_resetstkoflw)
#undef FX_SEH_STUB

} // namespace transmute::fx
