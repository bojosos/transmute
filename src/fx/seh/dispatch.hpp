#pragma once

// Internals shared between the dispatcher, the Rtl* entry points and the
// function-table lookup.
//
// Nesting model. The dispatcher runs guest code (language handlers, filters,
// consolidation callbacks) in nested call_guest invocations. Each such call is
// recorded as a *boundary*: the guest SP it started at, plus the context a
// frame walk continues from when it unwinds out of that call (the exception
// context for a handler, the target frame for a consolidation callback, ...).
// Frame walks cross boundaries at kReturnToHostPc, so a rethrow inside a C++
// catch block (which runs inside a consolidation callback) still finds the
// handlers of the catching function and its callers.
//
// A transfer (RtlUnwindEx, RtlRestoreContext, longjmp, a handler-initiated
// unwind) to a target outside the innermost guest segment is recorded as
// pending and propagated outward: each nested call is ended (ctx.pc =
// kReturnToHostPc) until the host code that owns the target's segment applies
// it. Every seh entry point is wrapped by run_entry for that.

#include "fx/seh/seh.hpp"

#include <initializer_list>
#include <span>

namespace transmute::fx::seh {

// Runs guest function `function` with integer arguments and returns x0. Thin
// wrapper over transmute::fx::call_guest (stubbed by the seh tests).
std::uint64_t invoke_guest_function(std::uint64_t function, std::initializer_list<std::uint64_t> args);
std::uint64_t invoke_guest_function(std::uint64_t function, std::span<const std::uint64_t> args);

// Scratch region in guest-accessible memory for the structures guest handlers
// read. Stack discipline: entry points release what they allocated on exit.
std::uint64_t allocate_guest_scratch(std::size_t size, std::size_t alignment = 16);
std::size_t guest_scratch_mark();
void guest_scratch_release(std::size_t mark);
void free_guest_scratch();

// ---- boundaries -------------------------------------------------------------

enum class BoundaryKind {
    exception_handler,   // first-pass language handler; continue at the exception context
    unwind_handler,      // second-pass handler; continue at the caller of the frame being unwound
    consolidate,         // STATUS_UNWIND_CONSOLIDATE callback; continue at the unwind target
    filter,              // vectored handler / unhandled filter; continue at the exception context
    callback,            // function-table callback; walks end here
};

// Calls guest `function` as a nested call recorded as a boundary of `kind`
// (with `continuation`, if any). If a pending transfer belongs to this nesting
// level or an outer one when the call returns, throws TransferUnwind.
std::uint64_t invoke_nested(BoundaryKind kind, const Arm64Context* continuation, std::uint64_t function,
                            std::initializer_list<std::uint64_t> args);

// When a walk reaches kReturnToHostPc with `context.Sp`, replaces `context`
// with the continuation of the matching boundary. False if the walk ends.
// `limit` (initially boundary_count()) bounds the search to boundaries
// outside the ones already crossed, so walks always move outward even when
// nested calls start at the same SP.
bool cross_boundary(Arm64Context& context, std::size_t& limit);
std::size_t boundary_count();

struct TransferUnwind {};   // C++ exception used only inside the seh component

// ---- entry points -------------------------------------------------------------

struct EntryScope {
    explicit EntryScope(GuestContext& ctx);
    ~EntryScope();
    EntryScope(const EntryScope&) = delete;
    EntryScope& operator=(const EntryScope&) = delete;
    GuestContext& ctx;
    std::size_t depth;          // boundaries outside this entry point
    std::size_t scratch_mark;
};

// Applies (if it belongs to this entry's segment) or propagates (ctx.pc =
// kReturnToHostPc) the pending transfer. Always returns continue_at_pc.
GuestResolution finish_pending_transfer(EntryScope& scope);

template <typename Body>
GuestResolution run_entry(GuestContext& ctx, Body&& body) {
    EntryScope scope(ctx);
    try {
        return body(scope);
    } catch (const TransferUnwind&) {
        return finish_pending_transfer(scope);
    }
}

// Moves the thread to `target`: applies it to ctx when it lies in the current
// guest segment, otherwise records it as pending and ends the nested calls in
// between. Called inside an entry point.
GuestResolution transfer_to(EntryScope& scope, const Arm64Context& target);

// ---- unwinding --------------------------------------------------------------

inline constexpr std::uint64_t kUnwindAllFrames = ~0ull;

struct UnwindRequest {
    std::uint64_t target_frame = 0;        // establisher frame to stop at (kUnwindAllFrames = exit unwind)
    std::uint64_t target_pc = 0;           // continuation address after the unwind
    std::uint64_t return_value = 0;        // becomes x0 at the target
    GuestExceptionRecord record{};         // the exception driving the unwind
    std::uint64_t record_address = 0;      // guest EXCEPTION_RECORD (consolidate / longjmp semantics)
    const Arm64Context* resume_context = nullptr;  // overrides the target state (longjmp buffer)
};

struct UnwindOutcome {
    enum class Status { transferred, bad_stack, bad_unwind, invalid_disposition, target_not_found };
    Status status;
    Arm64Context target;   // valid when transferred
};

// Runs the second (unwinding) pass from `start` (the caller of RtlUnwindEx)
// and computes the final context, including RtlRestoreContext record
// semantics. Does not modify the thread context.
UnwindOutcome unwind_to_target(EntryScope& scope, const Arm64Context& start, const UnwindRequest& request);

// RtlRestoreContext record semantics applied to the final context:
//  * STATUS_UNWIND_CONSOLIDATE: ExceptionInformation[10] receives the target's
//    non-volatile registers (DISPATCHER_CONTEXT_NONVOLREG_ARM64), then the
//    callback in ExceptionInformation[0] runs (as a consolidate boundary whose
//    continuation is the target) and returns the continuation PC.
//  * STATUS_LONGJUMP: registers are reloaded from the _JUMP_BUFFER in
//    ExceptionInformation[0].
void apply_restore_record(Arm64Context& target, std::uint64_t record_address);

// The caller's context at a call to a seh entry point: PC = return address
// (x30), CONTEXT_UNWOUND_TO_CALL set.
Arm64Context caller_context(const GuestContext& ctx);

// Reads an EXCEPTION_RECORD (ARM64/x64 shared layout) from guest memory.
GuestExceptionRecord read_exception_record(std::uint64_t address);

// First-pass dispatch. `context` is the exception context (a return address
// with CONTEXT_UNWOUND_TO_CALL for software exceptions). On return the thread
// resumes at ctx (continue-execution) — transfers leave via TransferUnwind.
void dispatch_exception_internal(EntryScope& scope, const Arm64Context& context, const GuestExceptionRecord& record);

// Bumps the STATUS_UNWIND_CONSOLIDATE statistic.
std::uint64_t consolidate_count_bump();

} // namespace transmute::fx::seh
