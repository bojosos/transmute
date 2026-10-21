// Guest fibers (owner: abi).
//
// On ARM64, GetCurrentFiber() and GetFiberData() are inline reads of the
// guest TEB (NtTib.FiberData at x18 + 0x20, then *(void**)fiber), so the
// host's fiber APIs alone cannot serve guest code. Each guest fiber is a
// GuestFiber whose first field is the fiber parameter, backed by:
//   * a host fiber, so host stacks, host FLS (FlsGetValue/FlsSetValue and
//     the FLS callbacks DeleteFiber runs) and nested host->guest callbacks
//     stay per fiber;
//   * its own guest stack from the runtime (allocate_guest_stack), made
//     current with switch_guest_stack (GuestContext bounds, shadow TEB
//     StackBase/StackLimit/DeallocationStack, overflow tracking).
//
// SwitchToFiber saves the per-fiber part of GuestContext (registers, pc,
// host_rsp, nesting, RSB, ...), installs the target's guest stack and
// FiberData, and switches host fibers. The resumed side restores its own
// snapshot and returns from its own SwitchToFiber call; a fiber that never
// ran starts in fiber_host_start, which runs the guest start routine with
// call_guest_prepared. Thread-level fields (stubs, helpers, lookup table,
// TEB pointers, interrupt_pending) are never swapped.

#include "fx/abi/abi_internal.hpp"
#include "fx/abi/overrides_internal.hpp"
#include "fx/runtime/runtime.hpp"
#include "transmute/fx/process.hpp"

#include <windows.h>
#include <xmmintrin.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <memory>
#include <mutex>
#include <new>
#include <unordered_set>

namespace transmute::fx::abi {
namespace {

using R = OverrideResult;
constexpr R kReturn = R::return_to_caller;

constexpr std::size_t kTebFiberData = 0x20;              // NT_TIB.FiberData
constexpr std::size_t kTebGuaranteedStackBytes = 0x1748;

struct GuestFiber {
    void* fiber_data{};               // FIRST: guest GetFiberData() == *(void**)fiber
    void* host_fiber{};
    GuestStack stack{};               // guest stack (for the thread's fiber: filled at its first switch)
    std::uint64_t start{};            // guest LPFIBER_START_ROUTINE (0 for a converted thread)
    bool converted_thread{};          // made by ConvertThreadToFiber
    bool owns_stack{};                // stack allocated here (CreateFiber)
    bool float_switch{};              // FIBER_FLAG_FLOAT_SWITCH: FPCR/FPSR switch with the fiber
    bool host_fiber_owned{};          // host fiber created/converted here
    std::uint32_t guaranteed_stack_bytes{};
    std::uint64_t teb_fiber_data_before{};   // shadow TEB FiberData before ConvertThreadToFiber
    std::unique_ptr<GuestContext> saved{std::make_unique<GuestContext>()};
};
static_assert(offsetof(GuestFiber, fiber_data) == 0, "GetFiberData() reads the first field");

thread_local GuestFiber* t_fiber = nullptr;

std::mutex g_fibers_mutex;
std::unordered_set<GuestFiber*>& fibers() {
    static auto* set = new std::unordered_set<GuestFiber*>();
    return *set;
}
bool is_guest_fiber(GuestFiber* fiber) {
    std::lock_guard lock(g_fibers_mutex);
    return fibers().count(fiber) != 0;
}

std::uint8_t* teb(GuestContext& ctx) { return static_cast<std::uint8_t*>(ctx.shadow_teb); }
void set_teb_fiber_data(GuestContext& ctx, std::uint64_t value) {
    if (ctx.shadow_teb) std::memcpy(teb(ctx) + kTebFiberData, &value, sizeof value);
}
std::uint64_t teb_fiber_data(GuestContext& ctx) {
    std::uint64_t value = 0;
    if (ctx.shadow_teb) std::memcpy(&value, teb(ctx) + kTebFiberData, sizeof value);
    return value;
}
std::uint32_t& teb_guarantee(GuestContext& ctx) {
    return *reinterpret_cast<std::uint32_t*>(teb(ctx) + kTebGuaranteedStackBytes);
}

// Copies the per-fiber part of the context (everything except thread-level
// runtime linkage and TEB pointers).
void restore_fiber_state(GuestContext& ctx, const GuestContext& saved, bool float_switch) {
    std::memcpy(ctx.x, saved.x, sizeof ctx.x);
    ctx.pc = saved.pc;
    ctx.nzcv = saved.nzcv;
    if (float_switch && (ctx.fpcr != saved.fpcr || ctx.fpsr != saved.fpsr)) {
        // FIBER_FLAG_FLOAT_SWITCH: the fiber's own FP environment, which
        // translated code expects mirrored in MXCSR.
        ctx.fpcr = saved.fpcr;
        ctx.fpsr = saved.fpsr;
        _mm_setcsr(mxcsr_for_guest(ctx.fpcr, ctx.fpsr));
    }
    ctx.tpidr_el0 = saved.tpidr_el0;
    ctx.exit_reason = saved.exit_reason;
    ctx.exit_arg = saved.exit_arg;
    ctx.host_rsp = saved.host_rsp;
    std::memcpy(ctx.v, saved.v, sizeof ctx.v);
    ctx.excl_address = saved.excl_address;
    std::memcpy(ctx.excl_value, saved.excl_value, sizeof ctx.excl_value);
    ctx.excl_size = saved.excl_size;
    ctx.rsb_top = saved.rsb_top;
    std::memcpy(ctx.rsb, saved.rsb, sizeof ctx.rsb);
    ctx.host_mxcsr = saved.host_mxcsr;
    ctx.nesting = saved.nesting;
    std::memcpy(ctx.spill, saved.spill, sizeof ctx.spill);
    std::memcpy(ctx.vspill, saved.vspill, sizeof ctx.vspill);
    // guest_stack_base/limit come from switch_guest_stack.
}

// Makes `target` the thread's current guest fiber (guest stack, TEB fields).
// The previous fiber's stack state is stored back into `current`.
void install_fiber(GuestContext& ctx, GuestFiber* current, GuestFiber* target) {
    if (current) {
        current->guaranteed_stack_bytes = teb_guarantee(ctx);
        current->stack = switch_guest_stack(target->stack);
    } else {
        switch_guest_stack(target->stack);
    }
    teb_guarantee(ctx) = target->guaranteed_stack_bytes;
    set_teb_fiber_data(ctx, reinterpret_cast<std::uint64_t>(target));
    t_fiber = target;
}

// Host fiber entry for a guest fiber that has not run yet. The switcher has
// installed this fiber's stack and FiberData.
void WINAPI fiber_host_start(void* parameter) {
    auto* fiber = static_cast<GuestFiber*>(parameter);
    GuestContext& ctx = *current_context();
    for (unsigned n = 0; n < 31; ++n)
        if (n != 18) ctx.x[n] = 0;               // x18 stays the TEB
    ctx.x[0] = reinterpret_cast<std::uint64_t>(fiber->fiber_data);
    ctx.x[31] = fiber->stack.base & ~std::uint64_t{15};
    for (auto& v : ctx.v) v[0] = v[1] = 0;
    ctx.nzcv = 0;
    call_guest_prepared(ctx, fiber->start);
    // Returning from a fiber's start routine ends the thread.
    exit_guest_thread(static_cast<std::uint32_t>(ctx.x[0]));
}

std::uint64_t fiber_stack_reserve(std::uint64_t commit, std::uint64_t reserve) {
    if (reserve != 0) return std::max(reserve, commit);
    // CreateFiber(dwStackSize): the commit; reserve as for threads.
    return thread_stack_reserve(commit, 0);
}

// ConvertThreadToFiber(param) / ConvertThreadToFiberEx(param, flags)
R convert_thread_to_fiber(GuestContext& ctx, std::uint64_t parameter, std::uint32_t flags) {
    if (t_fiber) {
        SetLastError(ERROR_ALREADY_FIBER);
        ctx.x[0] = 0;
        return kReturn;
    }
    auto* fiber = new GuestFiber();
    fiber->fiber_data = reinterpret_cast<void*>(parameter);
    fiber->converted_thread = true;
    fiber->float_switch = (flags & FIBER_FLAG_FLOAT_SWITCH) != 0;
    fiber->host_fiber = ConvertThreadToFiberEx(fiber, flags);
    if (fiber->host_fiber) {
        fiber->host_fiber_owned = true;
    } else if (GetLastError() == ERROR_ALREADY_FIBER) {
        fiber->host_fiber = GetCurrentFiber();     // the host side already made this thread a fiber
    } else {
        delete fiber;
        ctx.x[0] = 0;
        return kReturn;
    }
    fiber->guaranteed_stack_bytes = teb_guarantee(ctx);
    fiber->teb_fiber_data_before = teb_fiber_data(ctx);
    {
        std::lock_guard lock(g_fibers_mutex);
        fibers().insert(fiber);
    }
    t_fiber = fiber;
    set_teb_fiber_data(ctx, reinterpret_cast<std::uint64_t>(fiber));
    ctx.x[0] = reinterpret_cast<std::uint64_t>(fiber);
    return kReturn;
}
R o_ConvertThreadToFiber(GuestContext& ctx, const OverrideBinding&) { return convert_thread_to_fiber(ctx, ctx.x[0], 0); }
R o_ConvertThreadToFiberEx(GuestContext& ctx, const OverrideBinding&) {
    return convert_thread_to_fiber(ctx, ctx.x[0], static_cast<std::uint32_t>(ctx.x[1]));
}

// CreateFiber(stack, start, param) / CreateFiberEx(commit, reserve, flags, start, param)
R create_fiber(GuestContext& ctx, std::uint64_t commit, std::uint64_t reserve, std::uint32_t flags, std::uint64_t start,
               std::uint64_t parameter) {
    if (start == 0) {
        SetLastError(ERROR_INVALID_PARAMETER);
        ctx.x[0] = 0;
        return kReturn;
    }
    auto* fiber = new GuestFiber();
    fiber->fiber_data = reinterpret_cast<void*>(parameter);
    fiber->start = start;
    fiber->float_switch = (flags & FIBER_FLAG_FLOAT_SWITCH) != 0;
    try {
        fiber->stack = allocate_guest_stack(fiber_stack_reserve(commit, reserve));
    } catch (const std::bad_alloc&) {
        delete fiber;
        SetLastError(ERROR_NOT_ENOUGH_MEMORY);
        ctx.x[0] = 0;
        return kReturn;
    }
    fiber->owns_stack = true;
    fiber->host_fiber = CreateFiberEx(0, 0, flags, &fiber_host_start, fiber);
    if (fiber->host_fiber == nullptr) {
        const DWORD error = GetLastError();
        free_guest_stack(fiber->stack);
        delete fiber;
        SetLastError(error);
        ctx.x[0] = 0;
        return kReturn;
    }
    fiber->host_fiber_owned = true;
    // The guest's saved state is set up by fiber_host_start on first switch;
    // FPCR is inherited from the creating fiber.
    fiber->saved->fpcr = ctx.fpcr;
    fiber->saved->fpsr = ctx.fpsr;
    {
        std::lock_guard lock(g_fibers_mutex);
        fibers().insert(fiber);
    }
    ctx.x[0] = reinterpret_cast<std::uint64_t>(fiber);
    return kReturn;
}
R o_CreateFiber(GuestContext& ctx, const OverrideBinding&) {
    return create_fiber(ctx, ctx.x[0], 0, 0, ctx.x[1], ctx.x[2]);
}
R o_CreateFiberEx(GuestContext& ctx, const OverrideBinding&) {
    return create_fiber(ctx, ctx.x[0], ctx.x[1], static_cast<std::uint32_t>(ctx.x[2]), ctx.x[3], ctx.x[4]);
}

// SwitchToFiber(fiber)
R o_SwitchToFiber(GuestContext& ctx, const OverrideBinding& b) {
    auto* target = reinterpret_cast<GuestFiber*>(ctx.x[0]);
    GuestFiber* current = t_fiber;
    if (target == current) return kReturn;
    if (current == nullptr || !is_guest_fiber(target)) {
        // Not a guest fiber switch we can track: let the host do it.
        if (b.native) reinterpret_cast<void(WINAPI*)(void*)>(const_cast<void*>(b.native))(target);
        return kReturn;
    }
    *current->saved = ctx;                 // ctx.pc is our return address
    // The fiber's own FP environment includes the flags accumulated in MXCSR
    // since the last region entry (FPSR = ctx.fpsr | flags(MXCSR)).
    if (current->float_switch) {
        const unsigned csr = _mm_getcsr();   // IE ZE OE UE PE -> IOC DZC OFC UFC IXC (DE has no FPSR bit)
        current->saved->fpsr = ctx.fpsr | (csr & 1u) | ((csr >> 1) & 0x1eu);
    }
    install_fiber(ctx, current, target);
    SwitchToFiber(target->host_fiber);
    // ---- resumed: another fiber switched back to `current` ----
    restore_fiber_state(ctx, *current->saved, current->float_switch);
    return kReturn;
}

// DeleteFiber(fiber)
R o_DeleteFiber(GuestContext& ctx, const OverrideBinding& b) {
    auto* fiber = reinterpret_cast<GuestFiber*>(ctx.x[0]);
    if (!is_guest_fiber(fiber)) {
        if (b.native) reinterpret_cast<void(WINAPI*)(void*)>(const_cast<void*>(b.native))(fiber);
        return kReturn;
    }
    if (fiber == t_fiber) {
        // A fiber deleting itself ends its thread.
        exit_guest_thread(0);
    }
    {
        std::lock_guard lock(g_fibers_mutex);
        fibers().erase(fiber);
    }
    // Runs the fiber's FLS callbacks (host FLS is per host fiber) on this
    // fiber, then frees its host stack.
    if (fiber->host_fiber_owned && fiber->host_fiber && !fiber->converted_thread) DeleteFiber(fiber->host_fiber);
    if (fiber->owns_stack) free_guest_stack(fiber->stack);
    delete fiber;
    return kReturn;
}

// ConvertFiberToThread()
R o_ConvertFiberToThread(GuestContext& ctx, const OverrideBinding&) {
    GuestFiber* fiber = t_fiber;
    if (fiber == nullptr) {
        SetLastError(ERROR_ALREADY_THREAD);
        ctx.x[0] = FALSE;
        return kReturn;
    }
    BOOL ok = TRUE;
    if (fiber->host_fiber_owned) ok = ConvertFiberToThread();
    if (!ok) {
        ctx.x[0] = FALSE;
        return kReturn;
    }
    set_teb_fiber_data(ctx, fiber->converted_thread ? fiber->teb_fiber_data_before : 0);
    t_fiber = nullptr;
    if (fiber->converted_thread) {
        std::lock_guard lock(g_fibers_mutex);
        fibers().erase(fiber);
        delete fiber;
    }
    ctx.x[0] = TRUE;
    return kReturn;
}

// IsThreadAFiber()
R o_IsThreadAFiber(GuestContext& ctx, const OverrideBinding&) {
    ctx.x[0] = t_fiber != nullptr ? TRUE : FALSE;
    return kReturn;
}

struct OverrideEntry {
    const char* name;
    OverrideHandler handler;
};

constexpr OverrideEntry kFiberOverrides[] = {
    {"ConvertFiberToThread", o_ConvertFiberToThread},
    {"ConvertThreadToFiber", o_ConvertThreadToFiber},
    {"ConvertThreadToFiberEx", o_ConvertThreadToFiberEx},
    {"CreateFiber", o_CreateFiber},
    {"CreateFiberEx", o_CreateFiberEx},
    {"DeleteFiber", o_DeleteFiber},
    {"IsThreadAFiber", o_IsThreadAFiber},
    {"SwitchToFiber", o_SwitchToFiber},
};
static_assert(std::is_sorted(std::begin(kFiberOverrides), std::end(kFiberOverrides),
                             [](const OverrideEntry& a, const OverrideEntry& b) {
                                 return std::string_view(a.name) < std::string_view(b.name);
                             }),
              "kFiberOverrides must be sorted by name");

} // namespace

OverrideHandler find_fiber_override(std::string_view name) noexcept {
    const auto* it = std::lower_bound(std::begin(kFiberOverrides), std::end(kFiberOverrides), name,
        [](const OverrideEntry& e, std::string_view key) { return std::string_view(e.name) < key; });
    if (it != std::end(kFiberOverrides) && std::string_view(it->name) == name) return it->handler;
    return nullptr;
}

std::size_t fiber_override_count() noexcept { return std::size(kFiberOverrides); }

} // namespace transmute::fx::abi
