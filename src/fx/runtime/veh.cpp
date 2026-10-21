// Host fault handler (host::install_fault_handler: the vectored exception
// handler on win32):
//  1. write faults on write-protected translated guest pages (SMC): invalidate
//     the page's translations, restore its protection, retry the write;
//  2. host calls into guest code (execute fault at a guest code address):
//     continue in the host-call bridge, which runs the guest function;
//  3. faults inside translated code covered by a FaultSite: rebuild the guest
//     state (pins, flags, pc) and leave translated code with ExitReason::raise
//     so the dispatcher raises the guest exception outside the handler.
// Before 2 and 3, any access fault may be a first touch of memory the
// personality commits on demand (set_demand_commit_hook): the hook commits the
// page and the access is retried.

#include "fx/runtime/runtime_internal.hpp"
#include "fx/host/env.hpp"
#include "fx/host/vm.hpp"

#include <atomic>
#include <cstdio>
#include <cstring>

namespace transmute::fx::rt {
namespace {

// The personality's demand-commit hook. Entries are published once and never
// freed (a replaced one may still be read by a fault in flight).
struct DemandCommitEntry {
    DemandCommitHook hook;
    void* user;
};
std::atomic<const DemandCommitEntry*> g_demand_commit{nullptr};

std::uint64_t nzcv_from_eflags(std::uint32_t eflags, FlagsLocation location) {
    const bool cf = eflags & 0x1, zf = eflags & 0x40, sf = eflags & 0x80, of = eflags & 0x800;
    std::uint64_t nzcv = (sf ? 1ull << 31 : 0) | (zf ? 1ull << 30 : 0);
    switch (location) {
    case FlagsLocation::host_sub: nzcv |= (cf ? 0 : 1ull << 29) | (of ? 1ull << 28 : 0); break;
    case FlagsLocation::host_add: nzcv |= (cf ? 1ull << 29 : 0) | (of ? 1ull << 28 : 0); break;
    default: break;
    }
    return nzcv;
}

thread_local std::uint64_t t_last_write_retry = 0, t_last_write_changes = 0;
thread_local unsigned t_write_retries = 0;

bool handle_write(std::uint64_t address) {
    std::uint64_t changes = 0;
    switch (smc_handle_write_fault(address, changes)) {
    case WriteFault::smc:
        t_last_write_retry = 0;
        return true;
    case WriteFault::other:
        return false;
    case WriteFault::writable:
        break;
    }
    // Another thread restored the page between our fault and the check: retry.
    // The same store faulting again is progress if fx changed a page's
    // protection in between (the page was translated from and protected
    // again, then restored by another writer: V8 code pages with several
    // writers and running code). Only faults on a page that stays writable,
    // with no change by fx, are given up on after a few retries.
    if (address == t_last_write_retry && changes == t_last_write_changes) {
        if (++t_write_retries > 4) { t_last_write_retry = 0; return false; }
    } else {
        t_last_write_retry = address;
        t_last_write_changes = changes;
        t_write_retries = 0;
    }
    return true;
}

bool handle_translated_fault(const host::FaultRecord& fault, host::CpuContext& c) {
    FaultInfo info;
    if (!find_fault_site(host::rip(c), info)) return false;
    ThreadState* ts = current_thread_state();
    if (!ts || reinterpret_cast<std::uint64_t>(ts->ctx) + kContextBias != host::gpr(c, 15)) return false;
    GuestContext& ctx = *ts->ctx;
    // Pins, then the site's fault map (dirty write-back cache registers;
    // entries override pins of the same guest register).
    const std::vector<FaultMap> maps{{0, static_cast<std::uint32_t>(info.map.size())}};
    for_each_fault_register(info.pins, info.site, maps, info.map, [&](unsigned guest, unsigned reg, bool vector) {
        if (vector) std::memcpy(ctx.v[guest & 31], host::xmm(c, reg), 16);
        else ctx.x[guest & 31] = host::gpr(c, static_cast<std::uint8_t>(reg));
    });
    switch (info.site.flags) {
    case FlagsLocation::context: break;
    case FlagsLocation::host_register: ctx.nzcv = host::gpr(c, info.site.flags_register) & 0xf0000000u; break;
    default: ctx.nzcv = nzcv_from_eflags(host::eflags(c), info.site.flags); break;
    }
    ctx.pc = info.site.guest_pc;
    ts->pending = guest_fault_record(fault, ctx.pc);
    ts->pending_valid = true;
    ctx.exit_reason = static_cast<std::uint32_t>(ExitReason::raise);
    ctx.exit_arg = ts->pending.code;
    host::rip(c) = reinterpret_cast<std::uint64_t>(g_stubs.dispatch_exit);
    return true;
}

bool arena_frame(std::uint64_t pc) { return in_arena(pc); }

host::FaultAction fault_handler(host::FaultRecord& record, host::CpuContext& c) {
    static const bool trace = host::get_env("TRANSMUTE_FX_TRACE_HOST_EXCEPTIONS", nullptr, 0) > 0;
    if (trace) host::trace_fault(record, c, &arena_frame);
    // Guest stack growth through guard pages, from any code (translated,
    // interpreter, or host code writing to a guest stack buffer).
    if (record.code == kStatusGuardPageViolation && record.parameter_count >= 2 &&
        guest_stack_fault(record.code, record.parameters[1]) == StackFault::grown) {
        return host::FaultAction::resume;
    }
    if (record.code == kStatusAccessViolation && record.parameter_count >= 2) {
        const auto kind = record.parameters[0];
        const auto address = record.parameters[1];
        if (kind == host::kFaultWrite && handle_write(address)) return host::FaultAction::resume;
        // First touch of memory committed on demand (any thread, any code).
        if (demand_commit(address, static_cast<std::uint32_t>(kind))) return host::FaultAction::resume;
        if (kind == host::kFaultExecute && address == host::rip(c) && !in_arena(address) && is_guest_code(address)) {
            // A host call (or tail jump) into guest code: run it as a callback.
            host::gpr(c, 10) = host::rip(c);
            host::rip(c) = reinterpret_cast<std::uint64_t>(g_stubs.host_call_bridge);
            return host::FaultAction::resume;
        }
    }
    if (in_arena(host::rip(c)) && handle_translated_fault(record, c)) return host::FaultAction::resume;
    return host::FaultAction::pass;
}

} // namespace

bool demand_commit(std::uint64_t address, std::uint32_t access) noexcept {
    const auto* entry = g_demand_commit.load(std::memory_order_acquire);
    return entry && entry->hook(address, access, entry->user);
}

GuestExceptionRecord guest_fault_record(const host::FaultRecord& fault, std::uint64_t pc) noexcept {
    GuestExceptionRecord record{};
    record.code = fault.code;
    record.flags = fault.flags & host::kFaultNoncontinuable;
    record.address = pc;
    record.parameter_count = fault.parameter_count < 15 ? fault.parameter_count : 15;
    for (std::uint32_t i = 0; i < record.parameter_count; ++i) record.parameters[i] = fault.parameters[i];
    if ((fault.code == kStatusAccessViolation || fault.code == kStatusGuardPageViolation) &&
        fault.parameter_count >= 2 &&
        guest_stack_fault(fault.code, fault.parameters[1]) == StackFault::overflow) {
        record.code = kStatusStackOverflow;
    }
    return record;
}

void veh_initialize() {
    if (!host::install_fault_handler(&fault_handler)) fatal("fx runtime: AddVectoredExceptionHandler failed");
}

} // namespace transmute::fx::rt

namespace transmute::fx {

void set_demand_commit_hook(DemandCommitHook hook, void* user) {
    rt::g_demand_commit.store(hook ? new rt::DemandCommitEntry{hook, user} : nullptr, std::memory_order_release);
}

} // namespace transmute::fx
