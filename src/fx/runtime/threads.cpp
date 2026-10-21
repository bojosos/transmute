// Guest threads: per-host-thread GuestContext, guest stack, shadow TEB,
// thread creation/exit and the context registry.

#include "fx/loader/loader.hpp"
#include "fx/translator/emit_system.hpp"
#include "fx/runtime/runtime_internal.hpp"
#include "fx/host/process.hpp"
#include "fx/host/sample.hpp"
#include "fx/host/vm.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <exception>
#include <memory>
#include <stdexcept>

namespace transmute::fx::rt {

const void* g_helpers[kHelperSlots] = {};

namespace {

thread_local ThreadState* t_state = nullptr;
host::RwLock g_registry;
std::vector<ThreadState*> g_threads;

constexpr std::size_t kTebSize = 0x2000;
// Guest TEB layout (the ARM64 and x64 TEBs agree here): NT_TIB StackBase,
// StackLimit and Self, ThreadLocalStoragePointer, DeallocationStack.
constexpr std::size_t kTebStackBase = 0x08;
constexpr std::size_t kTebStackLimit = 0x10;
constexpr std::size_t kTebSelf = 0x30;
constexpr std::size_t kTebThreadLocalStoragePointer = 0x58;
constexpr std::size_t kTebDeallocationStack = 0x1478;
constexpr std::uint64_t kDefaultStack = std::uint64_t{1} << 20;
constexpr std::uint64_t kStackGranularity = 0x10000;
// Guard region below every guest stack (reserved, not committed). The first
// fault in it commits all but its lowest page as emergency space for the
// guest's handlers and raises STATUS_STACK_OVERFLOW; the lowest page stays
// inaccessible (a second overflow is an access violation, as natively).
constexpr std::uint64_t kGuardSize = 0x10000;
constexpr std::uint64_t kHardGuard = 0x1000;

std::uint64_t round_up(std::uint64_t value, std::uint64_t granularity) {
    return (value + granularity - 1) & ~(granularity - 1);
}

// SizeOfStackReserve of the guest executable (set by the loader), or 1 MiB.
std::uint64_t image_stack_reserve() {
    const auto reserve = guest_image_stack_reserve();
    return reserve ? std::max(round_up(reserve, kStackGranularity), kStackGranularity) : kDefaultStack;
}

void set_teb_field(void* teb, std::size_t offset, std::uint64_t value) noexcept {
    std::memcpy(static_cast<std::uint8_t*>(teb) + offset, &value, sizeof(value));
}

// The shadow TEB starts as a copy of the host thread's (host::copy_thread_block).
void* make_shadow_teb(std::uint64_t stack_base, std::uint64_t stack_limit, void* deallocation) {
    auto* teb = static_cast<std::uint8_t*>(host::vm::allocate(nullptr, kTebSize, host::vm::kReadWrite));
    if (!teb) return nullptr;
    host::copy_thread_block(teb, kTebSize);
    set_teb_field(teb, kTebStackBase, stack_base);
    set_teb_field(teb, kTebStackLimit, stack_limit);
    set_teb_field(teb, kTebSelf, reinterpret_cast<std::uint64_t>(teb));
    void* null = nullptr;
    std::memcpy(teb + kTebThreadLocalStoragePointer, &null, sizeof(void*));   // filled by the loader
    std::memcpy(teb + kTebDeallocationStack, &deallocation, sizeof(void*));
    return teb;
}

void destroy_state(ThreadState* ts, bool detach) {
    if (detach && ts->attached) {
        try {
            loader_thread_detach(*ts->ctx);
        } catch (const std::exception& error) {
            std::fprintf(stderr, "fx runtime: loader_thread_detach failed: %s\n", error.what());
        }
    }
    {
        ExclusiveLock lock(g_registry);
        g_threads.erase(std::remove(g_threads.begin(), g_threads.end(), ts), g_threads.end());
        for (unsigned i = 0; i < 16; ++i) count(g_counters.exits[i], ts->exits[i]);
    }
    if (t_state == ts) t_state = nullptr;
    host::sample::close_thread(ts->scan_handle);
    free_guest_stack(ts->own_stack);
    if (ts->teb) host::vm::release(ts->teb);
    delete ts->ctx;
    delete ts;
}

// Thread teardown runs from the host's thread exit hook
// (host::set_thread_exit_hook: a PE TLS callback on DLL_THREAD_DETACH on
// win32). It runs after the thread's FLS callbacks, so guest FLS callbacks
// (reaching guest code through abi trampolines or execute faults) still find
// this thread's state instead of creating a new one, and guest
// DLL_THREAD_DETACH runs after them, as natively. It runs before the CRT's
// thread_local destructors, so every thread_local (e.g. the interpreter's
// engine) is still alive during the teardown. exit_guest_thread therefore
// just exits the host thread.
void thread_exit_hook() {
    ThreadState* ts = t_state;
    if (!ts || host::process_shutting_down()) return;
    destroy_state(ts, true);
}

ThreadState* create_state(std::uint64_t stack_size, bool borrowed) {
    ensure_initialized();
    auto ts = std::make_unique<ThreadState>();
    auto ctx = std::make_unique<GuestContext>();
    ts->thread_id = host::current_thread_id();

    ts->own_stack = allocate_guest_stack(stack_size);
    ts->stack = ts->own_stack;
    const auto base = ts->stack.base;
    const auto limit = ts->stack.limit;
    auto* stack = reinterpret_cast<void*>(ts->stack.allocation);
    ts->teb = make_shadow_teb(base, limit, stack);
    if (!ts->teb) {
        free_guest_stack(ts->own_stack);
        throw std::bad_alloc();
    }

    ctx->thread = ts.get();
    ctx->x[18] = reinterpret_cast<std::uint64_t>(ts->teb);
    ctx->x[31] = base;
    ctx->shadow_teb = ts->teb;
    ctx->host_teb = host::thread_block();
    ctx->guest_stack_base = base;
    ctx->guest_stack_limit = limit;
    ctx->lookup_table = lookup_table();
    ctx->lookup_mask = kLookupMask;
    ctx->stub_dispatch_exit = g_stubs.dispatch_exit_live;   // translated code: convention registers live
    ctx->stub_indirect = g_stubs.indirect;
    ctx->stub_indirect_live = g_stubs.indirect_live;
    ctx->stub_host_return = g_stubs.host_return;
    ctx->stub_link = g_stubs.link;
    ctx->arena_base = arena_base();   // translated code inlines the stub_indirect lookup
    ts->ctx = ctx.release();
    {
        ExclusiveLock lock(g_registry);
        std::memcpy(ts->ctx->helpers, g_helpers, sizeof(g_helpers));
        install_system_helpers(*ts->ctx);   // helpers[48..63] (slot 63 is data: the timer scale)
        g_threads.push_back(ts.get());
    }
    t_state = ts.get();
    ts->borrowed = borrowed;
    // The guest EXE manifest context (comctl32 v6, GDI+, ...) is the guest's
    // process-default activation context: make it active on every thread
    // that runs guest code. On threads fx creates it stays active until the
    // thread exits. A borrowed thread must return to its owner with a balanced
    // activation stack (the thread pool fail-fasts with
    // STATUS_SXS_INVALID_DEACTIVATION otherwise), so invoke() scopes it there.
    if (void* activation = borrowed ? nullptr : guest_activation_context())
        ts->activation_cookie = host::activate_context(activation);
    return ts.release();
}

// GuestThreadStart::stack_reserve is the reserve (the abi converts a
// CreateThread commit size); 0 = the image default.
std::uint64_t thread_stack_size(const GuestThreadStart& start) {
    return start.stack_reserve != 0 ? start.stack_reserve : image_stack_reserve();
}

std::uint32_t guest_thread_main(void* parameter) {
    std::unique_ptr<GuestThreadStart> owned(static_cast<GuestThreadStart*>(parameter));
    const GuestThreadStart start = *owned;
    owned.reset();
    std::uint64_t result = 0;
    try {
        ThreadState* ts = create_state(thread_stack_size(start), false);
        ts->attached = true;
        loader_thread_attach(*ts->ctx);
        const std::uint64_t args[1] = {start.argument};
        result = call_guest(start.entry, args);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "fx runtime: guest thread failed: %s\n", error.what());
        result = 0xC0000001u;   // STATUS_UNSUCCESSFUL
    }
    exit_guest_thread(static_cast<std::uint32_t>(result));
}

} // namespace

ThreadState* current_thread_state() noexcept { return t_state; }

StackFault guest_stack_fault(std::uint32_t code, std::uint64_t address) noexcept {
    ThreadState* ts = t_state;
    if (!ts || !ts->stack.allocation) return StackFault::none;
    GuestStack& stack = ts->stack;
    if (address < stack.allocation || address - stack.allocation >= stack.size) return StackFault::none;
    const auto set_limit = [&](std::uint64_t limit) {
        stack.limit = limit;
        ts->ctx->guest_stack_limit = limit;
        set_teb_field(ts->teb, kTebStackLimit, limit);
    };
    if (code == kStatusGuardPageViolation) {
        // The kernel only grows the host thread's own stack. Do the same for
        // the guest stack: the touched guard page is now committed; move the
        // guard one page down while it stays above the guard region (a guard
        // the guest's _resetstkoflw re-armed somewhere in the stack).
        const auto page = address & ~std::uint64_t{0xfff};
        if (page >= stack.guard_end + 0x1000) {
            if (host::vm::protect(reinterpret_cast<void*>(page - 0x1000), 0x1000,
                                  host::vm::kReadWrite | host::vm::kGuard)) {
                if (page < stack.limit) set_limit(page);
                return StackFault::grown;
            }
        }
    } else if (code != kStatusAccessViolation || address >= stack.guard_end) {
        return StackFault::none;
    }
    // Overflow: any fault in the guard region, or a guard page that cannot
    // move further. The first one commits all but the lowest guard page as
    // emergency space for the guest's handlers.
    if (!stack.overflowed) {
        stack.overflowed = true;
        const auto emergency = stack.allocation + kHardGuard;
        if (host::vm::commit(reinterpret_cast<void*>(emergency), stack.guard_end - emergency, host::vm::kReadWrite))
            set_limit(emergency);
    }
    return StackFault::overflow;
}

void threads_initialize() { host::set_thread_exit_hook(&thread_exit_hook); }

std::size_t live_thread_states() {
    SharedLock lock(g_registry);
    return g_threads.size();
}

bool registry_lock_free() noexcept {
    if (!g_registry.try_lock()) return false;
    g_registry.unlock();
    return true;
}

void for_each_context(const std::function<void(GuestContext&)>& fn) {
    SharedLock lock(g_registry);
    for (ThreadState* ts : g_threads) fn(*ts->ctx);
}

void registered_threads(std::vector<std::pair<std::uint32_t, GuestContext*>>& threads) {
    SharedLock lock(g_registry);
    threads.clear();
    for (ThreadState* ts : g_threads) threads.emplace_back(ts->thread_id, ts->ctx);
}

void sum_exit_counts(std::uint64_t (&exits)[16]) {
    SharedLock lock(g_registry);
    for (unsigned i = 0; i < 16; ++i) exits[i] = g_counters.exits[i].load(std::memory_order_relaxed);
    for (ThreadState* ts : g_threads)
        for (unsigned i = 0; i < 16; ++i) exits[i] += ts->exits[i];
}

__declspec(noinline) void clear_stack_residue() noexcept {
    volatile std::uint64_t words[1024];   // 8 KiB below the caller
    for (auto& word : words) word = 0;
}

bool scan_guest_threads(const ThreadScan& scan) {
    namespace sample = host::sample;
    SharedLock lock(g_registry);
    const std::uint32_t self = host::current_thread_id();
    // Before anything is suspended: opening a handle and growing the list
    // may allocate.
    for (ThreadState* ts : g_threads)
        if (!ts->scan_handle && ts->thread_id != self) ts->scan_handle = sample::open_thread(ts->thread_id);
    static std::vector<ThreadState*> suspended;   // reclaim passes are serialized
    suspended.clear();
    suspended.reserve(g_threads.size());
    bool ok = true;
    for (ThreadState* ts : g_threads) {
        // The caller's own stack holds the scan's state: it cannot be judged.
        if (ts->thread_id == self || !ts->scan_handle || !sample::suspend(ts->scan_handle)) {
            ok = false;
            break;
        }
        suspended.push_back(ts);
    }
    // Nothing below allocates or takes a lock until every thread is resumed.
    for (std::size_t i = 0; ok && i < suspended.size(); ++i) {
        ThreadState* ts = suspended[i];
        std::uint64_t words[64];
        std::uint64_t sp = 0;
        const std::size_t n = sample::capture_words(ts->scan_handle, words, 64, sp);
        if (n == 0) {
            ok = false;
            break;
        }
        scan.thread(scan.user, ts->thread_id, sp);
        for (std::size_t w = 0; w < n; ++w) scan.word(scan.user, words[w]);
        if (!sample::scan_stack(sp, scan.word, scan.user)) {
            ok = false;
            break;
        }
        scan.context(scan.user, *ts->ctx);
    }
    for (ThreadState* ts : suspended) sample::resume(ts->scan_handle);
    return ok;
}

void refresh_helpers(unsigned id, const void* function) {
    ExclusiveLock lock(g_registry);
    g_helpers[id] = function;
    for (ThreadState* ts : g_threads) ts->ctx->helpers[id] = function;
}

} // namespace transmute::fx::rt

namespace transmute::fx {

GuestStack allocate_guest_stack(std::uint64_t reserve) {
    if (reserve == 0) reserve = rt::image_stack_reserve();
    reserve = std::max(rt::round_up(reserve, rt::kStackGranularity), rt::kStackGranularity);
    const auto total = reserve + rt::kGuardSize;
    auto* memory = static_cast<std::uint8_t*>(host::vm::reserve(nullptr, total, host::vm::kReadWrite));
    if (!memory || !host::vm::commit(memory + rt::kGuardSize, reserve, host::vm::kReadWrite)) {
        if (memory) host::vm::release(memory);
        throw std::bad_alloc();
    }
    GuestStack stack;
    stack.allocation = reinterpret_cast<std::uint64_t>(memory);
    stack.size = total;
    stack.guard_end = stack.allocation + rt::kGuardSize;
    stack.base = stack.allocation + total;
    stack.limit = stack.guard_end;
    return stack;
}

void free_guest_stack(const GuestStack& stack) {
    if (stack.allocation) host::vm::release(reinterpret_cast<void*>(stack.allocation));
}

GuestStack switch_guest_stack(const GuestStack& stack) {
    auto* ts = rt::current_thread_state();
    if (!ts) throw std::logic_error("switch_guest_stack: no guest context on this thread");
    const GuestStack previous = ts->stack;
    ts->stack = stack;
    ts->ctx->guest_stack_base = stack.base;
    ts->ctx->guest_stack_limit = stack.limit;
    rt::set_teb_field(ts->teb, rt::kTebStackBase, stack.base);
    rt::set_teb_field(ts->teb, rt::kTebStackLimit, stack.limit);
    rt::set_teb_field(ts->teb, rt::kTebDeallocationStack, stack.allocation);
    return previous;
}

bool rearm_guest_stack_guard(GuestContext& ctx) {
    auto* ts = rt::current_thread_state();
    if (!ts || !ts->stack.allocation) return false;
    GuestStack& stack = ts->stack;
    // The code calling this needs some stack above the region it re-arms.
    if (ctx.x[31] < stack.guard_end + 2 * rt::kHardGuard || ctx.x[31] > stack.base) return false;
    if (!stack.overflowed) return true;   // the guard region is still intact
    const auto emergency = stack.allocation + rt::kHardGuard;
    if (!host::vm::decommit(reinterpret_cast<void*>(emergency), stack.guard_end - emergency)) return false;
    stack.overflowed = false;
    stack.limit = stack.guard_end;
    ctx.guest_stack_limit = stack.guard_end;
    rt::set_teb_field(ts->teb, rt::kTebStackLimit, stack.guard_end);
    return true;
}

GuestContext* current_context() noexcept {
    auto* ts = rt::current_thread_state();
    return ts ? ts->ctx : nullptr;
}

GuestContext& ensure_thread_context() {
    if (auto* ts = rt::current_thread_state()) return *ts->ctx;
    rt::ThreadState* ts = rt::create_state(rt::image_stack_reserve(), true);
    ts->attached = true;
    loader_thread_attach(*ts->ctx);
    return *ts->ctx;
}

std::uint64_t create_guest_thread(const GuestThreadStart& start, std::uint32_t* thread_id) {
    rt::ensure_initialized();
    auto* parameter = new GuestThreadStart(start);
    std::uint32_t id = 0;
    host::ThreadHandle handle = host::create_thread(&rt::guest_thread_main, parameter, 0,
                                                    start.creation_flags & ~host::kThreadStackReservation, &id);
    if (!handle) {
        const auto error = host::last_error();
        delete parameter;
        host::set_last_error(error);
        return 0;
    }
    if (thread_id) *thread_id = id;
    rt::count(rt::g_counters.threads_created);
    return reinterpret_cast<std::uint64_t>(handle);
}

[[noreturn]] void exit_guest_thread(std::uint32_t exit_code) {
    // The thread state outlives the thread's FLS callbacks; the thread exit
    // hook runs DLL_THREAD_DETACH and frees it afterwards.
    host::exit_thread(exit_code);
}

void register_helper(unsigned id, const void* function) {
    rt::ensure_initialized();
    if (id >= kHelperSlots) throw std::out_of_range("register_helper: helper id out of range");
    rt::refresh_helpers(id, function);
}

void request_interrupt(GuestContext& ctx, std::function<void(GuestContext&)> request) {
    auto* ts = static_cast<rt::ThreadState*>(ctx.thread);
    {
        rt::ExclusiveLock lock(ts->interrupt_lock);
        ts->interrupts.push_back(std::move(request));
    }
    ctx.interrupt_pending = 1;
}

} // namespace transmute::fx
