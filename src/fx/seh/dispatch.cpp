// Guest exception dispatch (first pass), RtlUnwindEx (second pass), nested
// guest calls (boundaries), transfers, the unhandled-exception filter path
// and process termination. See dispatch.hpp for the nesting model.

#include "fx/seh/dispatch.hpp"
#include "fx/seh/vectored.hpp"
#include "fx/host/env.hpp"

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

namespace transmute::fx::seh {
namespace {

bool trace_seh() {
    static const bool enabled = [] {
        char value[8] = {};
        return host::get_env("TRANSMUTE_FX_TRACE_SEH", value, sizeof(value)) > 0 && value[0] != '0';
    }();
    return enabled;
}

// Opt-in tracing (TRANSMUTE_FX_TRACE_SEH=1): one line per dispatch, handler
// call, unwind, consolidation, boundary crossing and transfer.
void seh_trace(const char* format, ...) {
    char line[512];
    va_list args;
    va_start(args, format);
    std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    std::fprintf(stderr, "fx seh: %s\n", line);
    std::fflush(stderr);
}

#define SEH_TRACE(...)                           \
    do {                                         \
        if (trace_seh()) seh_trace(__VA_ARGS__); \
    } while (0)

std::atomic<std::uint64_t> g_dispatches{0};
std::atomic<std::uint64_t> g_handler_calls{0};
std::atomic<std::uint64_t> g_unwind_handler_calls{0};
std::atomic<std::uint64_t> g_consolidations{0};
std::atomic<std::uint64_t> g_nested{0};
std::atomic<std::uint64_t> g_collided{0};

constexpr std::int64_t kExceptionContinueExecution = -1;  // guest filter/VEH result (LONG)
constexpr unsigned kMaxDispatchNesting = 64;
constexpr unsigned kMaxFrames = 1u << 16;

struct Boundary {
    std::uint64_t sp;
    BoundaryKind kind;
    bool has_continuation;
    Arm64Context continuation;
};

struct PendingTransfer {
    bool active = false;
    std::size_t level = 0;      // boundaries outside the target's guest segment
    Arm64Context context{};
};

struct ThreadState {
    std::vector<Boundary> boundaries;           // outermost first
    std::vector<GuestContext*> entries;         // active seh entry points (thread context)
    PendingTransfer pending;
    unsigned dispatch_nesting = 0;
};

thread_local ThreadState t_state;

// EXCEPTION_POINTERS in guest memory: {PEXCEPTION_RECORD, PCONTEXT}.
struct GuestExceptionPointers {
    std::uint64_t record;
    std::uint64_t context;
};

std::uint64_t place_exception_record(const GuestExceptionRecord& record, std::uint64_t nested) {
    EXCEPTION_RECORD native{};
    native.ExceptionCode = record.code;
    native.ExceptionFlags = record.flags;
    native.ExceptionRecord = reinterpret_cast<EXCEPTION_RECORD*>(static_cast<std::uintptr_t>(nested));
    native.ExceptionAddress = reinterpret_cast<void*>(static_cast<std::uintptr_t>(record.address));
    native.NumberParameters = record.parameter_count;
    for (unsigned i = 0; i < record.parameter_count && i < EXCEPTION_MAXIMUM_PARAMETERS; ++i)
        native.ExceptionInformation[i] = static_cast<ULONG_PTR>(record.parameters[i]);
    const std::uint64_t address = allocate_guest_scratch(sizeof(native), 16);
    write_guest(address, &native, sizeof(native));
    return address;
}

std::uint64_t place_context(const Arm64Context& context) {
    const std::uint64_t address = allocate_guest_scratch(sizeof(context), 16);
    write_guest(address, &context, sizeof(context));
    return address;
}

bool within_bounds(const GuestContext& guest, std::uint64_t frame) {
    if (guest.guest_stack_base == 0 && guest.guest_stack_limit == 0) return true;  // bounds unknown
    return frame >= guest.guest_stack_limit && frame <= guest.guest_stack_base;
}

GuestContext* current_entry_context() {
    if (!t_state.entries.empty()) return t_state.entries.back();
    return current_context();
}

// Number of boundaries the target lies outside of: boundaries whose start SP
// is above the target's SP were entered after the target's frame was live.
std::size_t segment_level(std::uint64_t sp) {
    std::size_t level = 0;
    while (level < t_state.boundaries.size() && t_state.boundaries[level].sp > sp) ++level;
    return level;
}

struct BoundaryPush {
    explicit BoundaryPush(const Boundary& boundary) : level(t_state.boundaries.size()) {
        t_state.boundaries.push_back(boundary);
    }
    ~BoundaryPush() { t_state.boundaries.resize(level); }
    std::size_t level;
};

} // namespace

// ---------------------------------------------------------------------------
// Nested calls and boundaries
// ---------------------------------------------------------------------------

std::uint64_t invoke_nested(BoundaryKind kind, const Arm64Context* continuation, std::uint64_t function,
                            std::initializer_list<std::uint64_t> args) {
    Boundary boundary{};
    const GuestContext* ctx = current_entry_context();
    // call_guest starts the callee at the thread's current SP (16-aligned).
    boundary.sp = ctx ? (ctx->x[31] & ~std::uint64_t{15}) : 0;
    boundary.kind = kind;
    boundary.has_continuation = continuation != nullptr;
    if (continuation) boundary.continuation = *continuation;
    std::uint64_t result;
    std::size_t level;
    {
        BoundaryPush push(boundary);
        level = push.level;
        result = invoke_guest_function(function, args);
    }
    if (t_state.pending.active && t_state.pending.level <= level) {
        SEH_TRACE("transfer: leaving nested call (level %zu, target level %zu)", level, t_state.pending.level);
        throw TransferUnwind{};
    }
    return result;
}

std::size_t boundary_count() { return t_state.boundaries.size(); }

bool cross_boundary(Arm64Context& context, std::size_t& limit) {
    if (limit > t_state.boundaries.size()) limit = t_state.boundaries.size();
    for (std::size_t i = limit; i-- > 0;) {
        const Boundary& boundary = t_state.boundaries[i];
        if (boundary.sp != context.Sp) continue;
        if (!boundary.has_continuation) return false;
        SEH_TRACE("walk: crossing boundary %zu (kind %d) to %s", i, static_cast<int>(boundary.kind),
                  describe_guest_address(boundary.continuation.Pc).c_str());
        context = boundary.continuation;
        limit = i;
        return true;
    }
    return false;
}

EntryScope::EntryScope(GuestContext& c) : ctx(c), depth(t_state.boundaries.size()), scratch_mark(guest_scratch_mark()) {
    t_state.entries.push_back(&c);
}

EntryScope::~EntryScope() {
    t_state.entries.pop_back();
    guest_scratch_release(scratch_mark);
}

GuestResolution finish_pending_transfer(EntryScope& scope) {
    PendingTransfer& pending = t_state.pending;
    if (!pending.active) return GuestResolution::continue_at_pc;
    if (pending.level >= scope.depth) {
        SEH_TRACE("transfer: applying at level %zu: %s sp 0x%llx", scope.depth,
                  describe_guest_address(pending.context.Pc).c_str(),
                  static_cast<unsigned long long>(pending.context.Sp));
        pending.active = false;
        arm64_to_guest_context(pending.context, scope.ctx);
        scope.ctx.pc = pending.context.Pc;
    } else {
        // The target is in an outer segment: end the innermost nested call.
        scope.ctx.pc = kReturnToHostPc;
    }
    return GuestResolution::continue_at_pc;
}

GuestResolution transfer_to(EntryScope& scope, const Arm64Context& target) {
    const std::size_t level = segment_level(target.Sp);
    SEH_TRACE("transfer: to %s sp 0x%llx (target level %zu, entry level %zu)", describe_guest_address(target.Pc).c_str(),
              static_cast<unsigned long long>(target.Sp), level, scope.depth);
    t_state.pending.active = true;
    t_state.pending.level = level;
    t_state.pending.context = target;
    t_state.pending.context.ContextFlags |= kCtxFull;
    return finish_pending_transfer(scope);
}

Arm64Context caller_context(const GuestContext& ctx) {
    Arm64Context context;
    guest_to_arm64_context(ctx, context, kCtxFull | kCtxUnwoundToCall);
    context.Pc = ctx.x[30];
    return context;
}

GuestExceptionRecord read_exception_record(std::uint64_t address) {
    EXCEPTION_RECORD native{};
    read_guest(address, &native, sizeof(native));
    GuestExceptionRecord record{};
    record.code = native.ExceptionCode;
    record.flags = native.ExceptionFlags;
    record.address = reinterpret_cast<std::uint64_t>(native.ExceptionAddress);
    record.parameter_count = native.NumberParameters;
    for (unsigned i = 0; i < record.parameter_count && i < EXCEPTION_MAXIMUM_PARAMETERS; ++i)
        record.parameters[i] = native.ExceptionInformation[i];
    return record;
}

void apply_restore_record(Arm64Context& target, std::uint64_t record_address) {
    EXCEPTION_RECORD record{};
    if (!record_address || !read_guest(record_address, &record, sizeof(record))) return;
    if (record.ExceptionCode == kStatusUnwindConsolidate && record.NumberParameters >= 1) {
        consolidate_count_bump();
        if (record.NumberParameters > 10) {
            Arm64NonvolatileRegisters nonvol{};
            fill_nonvolatile_registers(target, nonvol);
            const std::uint64_t nonvol_address = allocate_guest_scratch(sizeof(nonvol), 16);
            write_guest(nonvol_address, &nonvol, sizeof(nonvol));
            write_guest(record_address + offsetof(EXCEPTION_RECORD, ExceptionInformation) + 10 * 8,
                        &nonvol_address, sizeof(nonvol_address));
        }
        const std::uint64_t callback = record.ExceptionInformation[0];
        SEH_TRACE("consolidate: callback %s record 0x%llx target sp 0x%llx", describe_guest_address(callback).c_str(),
                  static_cast<unsigned long long>(record_address), static_cast<unsigned long long>(target.Sp));
        // An exception raised by the callback (a rethrow in a catch block)
        // continues its search in the target frame, as through Windows'
        // consolidation frame.
        const Arm64Context continuation = target;
        const std::uint64_t pc = invoke_nested(BoundaryKind::consolidate, &continuation, callback, {record_address});
        SEH_TRACE("consolidate: continuation %s", describe_guest_address(pc).c_str());
        if (pc) target.Pc = pc;
    } else if (record.ExceptionCode == kStatusLongjump && record.NumberParameters >= 1) {
        struct JumpBuffer {
            std::uint64_t Frame, Reserved, X[10], Fp, Lr, Sp;
            std::uint32_t Fpcr, Fpsr;
            std::uint64_t D[8];
        } jb{};
        if (!read_guest(record.ExceptionInformation[0], &jb, sizeof(jb))) return;
        for (unsigned i = 0; i < 10; ++i) target.X[19 + i] = jb.X[i];
        target.Fp = jb.Fp;
        target.Lr = jb.Lr;
        target.Sp = jb.Sp;
        target.Fpcr = jb.Fpcr;
        target.Fpsr = jb.Fpsr;
        for (unsigned i = 0; i < 8; ++i) target.V[8 + i].Low = jb.D[i];
    }
}

namespace {

// Calls one language handler with its four arguments in guest memory.
// `context_argument` is the ContextRecord passed in x2 (the original
// exception context in the first pass; 0 = the dispatcher context, as the
// second pass does). `dispatcher_context` is what DispatcherContext
// .ContextRecord points at. `continuation` is where a walk out of the handler
// continues.
std::uint32_t dispatch_to_handler(BoundaryKind kind, const Arm64Context& continuation, std::uint64_t handler,
                                  std::uint64_t record_address, std::uint64_t establisher_frame,
                                  std::uint64_t context_argument, const Arm64Context& dispatcher_context,
                                  Arm64DispatcherContext& dispatcher, std::uint64_t function_entry,
                                  std::uint64_t image_base, std::uint64_t control_pc, std::uint64_t handler_data,
                                  std::uint64_t target_pc, std::uint64_t nonvol_address, bool control_pc_is_unwound) {
    const bool unwinding = kind == BoundaryKind::unwind_handler;
    const std::uint64_t context_address = place_context(dispatcher_context);
    if (!context_argument) context_argument = context_address;
    dispatcher.ControlPc = control_pc;
    dispatcher.ImageBase = image_base;
    dispatcher.FunctionEntry = reinterpret_cast<PARM64_RUNTIME_FUNCTION>(static_cast<std::uintptr_t>(function_entry));
    dispatcher.EstablisherFrame = establisher_frame;
    dispatcher.TargetPc = target_pc;
    dispatcher.ContextRecord = reinterpret_cast<PARM64_NT_CONTEXT>(static_cast<std::uintptr_t>(context_address));
    dispatcher.LanguageHandler = reinterpret_cast<PEXCEPTION_ROUTINE>(static_cast<std::uintptr_t>(handler));
    dispatcher.HandlerData = reinterpret_cast<PVOID>(static_cast<std::uintptr_t>(handler_data));
    dispatcher.HistoryTable = nullptr;
    dispatcher.ScopeIndex = 0;
    dispatcher.ControlPcIsUnwound = control_pc_is_unwound ? TRUE : FALSE;
    dispatcher.NonVolatileRegisters = reinterpret_cast<PBYTE>(static_cast<std::uintptr_t>(nonvol_address));
    const std::uint64_t dispatcher_address = allocate_guest_scratch(sizeof(dispatcher), 16);
    write_guest(dispatcher_address, &dispatcher, sizeof(dispatcher));

    (unwinding ? g_unwind_handler_calls : g_handler_calls).fetch_add(1, std::memory_order_relaxed);
    SEH_TRACE("%s handler %s frame 0x%llx control %s", unwinding ? "unwind" : "dispatch",
              describe_guest_address(handler).c_str(), static_cast<unsigned long long>(establisher_frame),
              describe_guest_address(control_pc).c_str());
    const std::uint32_t disposition = static_cast<std::uint32_t>(invoke_nested(
        kind, &continuation, handler, {record_address, establisher_frame, context_argument, dispatcher_address}));
    SEH_TRACE("  -> disposition %u", disposition);
    // The handler may have updated the dispatcher context (collided unwind).
    read_guest(dispatcher_address, &dispatcher, sizeof(dispatcher));
    return disposition;
}

// One frame of a walk: looks up the function at `context.Pc` and unwinds it,
// or treats it as a leaf. Crosses boundaries at kReturnToHostPc. Returns
// false when the walk ends (end of stack, unknown host boundary, bad data).
struct FrameStep {
    std::uint64_t control_pc = 0;
    std::uint64_t image_base = 0;
    std::uint64_t function_entry = 0;
    bool control_pc_is_unwound = false;
    Arm64Context before{};          // the frame's own state
    VirtualUnwindResult unwind{};   // handler / establisher (function frames)
};

bool step_frame(Arm64Context& context, std::size_t& limit, std::uint32_t handler_type, FrameStep& step) {
    for (unsigned crossings = 0; context.Pc == kReturnToHostPc; ++crossings) {
        if (crossings > 64 || !cross_boundary(context, limit)) return false;
    }
    if (context.Pc == 0) return false;
    step = FrameStep{};
    step.control_pc = context.Pc;
    step.control_pc_is_unwound = (context.ContextFlags & kCtxUnwoundToCall) != 0;
    step.before = context;
    const std::uint64_t lookup_pc =
        step.control_pc_is_unwound && step.control_pc >= 4 ? step.control_pc - 4 : step.control_pc;
    step.function_entry = lookup_function_entry(lookup_pc, &step.image_base);
    if (!step.function_entry) {
        // Leaf: the return address is in Lr and nothing was pushed.
        if (context.Lr == 0 || context.Lr == context.Pc) return false;
        step.unwind.establisher_frame = context.Sp;
        context.Pc = context.Lr;
        context.ContextFlags |= kCtxUnwoundToCall;
        return true;
    }
    Arm64RuntimeFunction entry;
    if (!read_guest_value(step.function_entry, entry)) return false;
    step.unwind = virtual_unwind(handler_type, step.image_base, step.control_pc, entry, context, nullptr);
    return step.unwind.status == UnwindStatus::ok;
}

[[noreturn]] void report_unhandled(const GuestExceptionRecord& record, const Arm64Context& context) {
    std::ostringstream out;
    out << "fx: unhandled guest exception\n";
    out << "  code:    0x" << std::hex << record.code << std::dec << '\n';
    out << "  address: " << describe_guest_address(record.address) << '\n';
    if ((record.code == kStatusAccessViolation || record.code == kStatusInPageError) && record.parameter_count >= 2) {
        out << "  access:  " << (record.parameters[0] == 1 ? "write" : record.parameters[0] == 8 ? "execute" : "read")
            << " at 0x" << std::hex << record.parameters[1] << std::dec << '\n';
    }
    out << format_guest_stack(context);
    terminate_process(record.code, out.str());
}

} // namespace

// ---------------------------------------------------------------------------
// Second pass: unwind from `start` to request.target_frame.
// ---------------------------------------------------------------------------
UnwindOutcome unwind_to_target(EntryScope& scope, const Arm64Context& start, const UnwindRequest& request) {
    const bool exit_unwind = request.target_frame == kUnwindAllFrames;
    SEH_TRACE("unwind: target frame 0x%llx pc %s code 0x%08x from %s",
              static_cast<unsigned long long>(request.target_frame), describe_guest_address(request.target_pc).c_str(),
              request.record.code, describe_guest_address(start.Pc).c_str());
    UnwindOutcome outcome{UnwindOutcome::Status::target_not_found, {}};

    GuestExceptionRecord record = request.record;
    record.flags |= kFlagUnwinding;
    if (exit_unwind) record.flags |= kFlagExitUnwind;
    // Handlers see the caller's record when there is one (FH3/FH4 read their
    // own fields from it); flags are updated in place as on Windows.
    std::uint64_t record_address = request.record_address;
    if (!record_address) record_address = place_exception_record(record, 0);

    Arm64Context context = start;
    std::size_t limit = boundary_count();
    Arm64Context target_state{};
    bool reached = false;
    for (unsigned frames = 0; frames < kMaxFrames; ++frames) {
        FrameStep step;
        if (!step_frame(context, limit, kHandlerTypeUnwind, step)) break;
        const std::uint64_t establisher = step.unwind.establisher_frame;
        if (!exit_unwind && (establisher > request.target_frame || !within_bounds(scope.ctx, establisher))) {
            outcome.status = UnwindOutcome::Status::bad_stack;
            return outcome;
        }
        const bool is_target = !exit_unwind && establisher == request.target_frame;
        if (step.unwind.handler) {
            std::uint32_t flags = record.flags | (is_target ? kFlagTargetUnwind : 0);
            write_guest(record_address + offsetof(EXCEPTION_RECORD, ExceptionFlags), &flags, sizeof(flags));
            Arm64NonvolatileRegisters nonvol{};
            fill_nonvolatile_registers(step.before, nonvol);
            const std::uint64_t nonvol_address = allocate_guest_scratch(sizeof(nonvol), 16);
            write_guest(nonvol_address, &nonvol, sizeof(nonvol));
            Arm64Context handler_context = step.before;
            handler_context.ContextFlags |= kCtxFull;
            handler_context.X[0] = request.return_value;
            Arm64DispatcherContext dispatcher{};
            const std::uint32_t disposition = dispatch_to_handler(
                BoundaryKind::unwind_handler, context, step.unwind.handler, record_address, establisher, 0,
                handler_context, dispatcher, step.function_entry, step.image_base, step.control_pc,
                step.unwind.handler_data, request.target_pc, nonvol_address, step.control_pc_is_unwound);
            if (disposition == disposition_collided_unwind) {
                g_collided.fetch_add(1, std::memory_order_relaxed);
            } else if (disposition != disposition_continue_search) {
                outcome.status = UnwindOutcome::Status::invalid_disposition;
                return outcome;
            }
        }
        if (is_target) {
            target_state = step.before;   // the target frame's own state, not its caller's
            reached = true;
            break;
        }
    }
    if (!reached && !request.resume_context) return outcome;

    // Final state: the jump buffer state if given, otherwise the target frame;
    // PC = target_pc, x0 = return value, then RtlRestoreContext record
    // semantics (consolidate / longjmp).
    Arm64Context final_context = request.resume_context ? *request.resume_context : target_state;
    final_context.ContextFlags |= kCtxFull;
    if (request.target_pc) final_context.Pc = request.target_pc;
    final_context.X[0] = request.return_value;
    if (request.record_address) {
        // Clear the unwind flags in the caller's record before the callback.
        std::uint32_t flags = request.record.flags;
        write_guest(record_address + offsetof(EXCEPTION_RECORD, ExceptionFlags), &flags, sizeof(flags));
        apply_restore_record(final_context, request.record_address);
    }
    outcome.status = UnwindOutcome::Status::transferred;
    outcome.target = final_context;
    return outcome;
}

// ---------------------------------------------------------------------------
// Last chance: fault hooks.
// ---------------------------------------------------------------------------
namespace {

// The last-chance stage of the dispatch running on this thread (innermost),
// for the built-in hook that calls the guest's unhandled-exception filter.
struct LastChance {
    const Arm64Context* fault_context;
    std::uint64_t pointers_address;   // guest EXCEPTION_POINTERS
    std::uint64_t context_address;    // guest CONTEXT (the filter may edit it)
};
thread_local const LastChance* t_last_chance = nullptr;

GuestFaultInfo fault_info(const GuestExceptionRecord& record) {
    GuestFaultInfo info{};
    info.code = record.code;
    info.flags = record.flags;
    info.pc = record.address;
    info.parameter_count = std::min<std::uint32_t>(record.parameter_count, 15);
    for (std::uint32_t i = 0; i < info.parameter_count; ++i) info.parameters[i] = record.parameters[i];
    const bool access = record.code == kStatusAccessViolation || record.code == kStatusInPageError ||
                        record.code == kStatusGuardPageViolation;
    if (access && info.parameter_count >= 2) {
        info.address = info.parameters[1];
        info.write = info.parameters[0] == 1;
        info.execute = info.parameters[0] == 8;
    }
    return info;
}

// set_unhandled_exception_filter as a GuestFaultHook: runs the guest filter
// with the dispatch's EXCEPTION_POINTERS; EXCEPTION_CONTINUE_EXECUTION
// resumes at the CONTEXT record (which the filter may have edited).
bool unhandled_filter_fault_hook(GuestContext& ctx, const GuestFaultInfo&, void*) {
    const LastChance* last = t_last_chance;
    const std::uint64_t filter = unhandled_exception_filter_routine();
    if (!last || !filter) return false;
    const auto result = static_cast<std::int32_t>(
        invoke_nested(BoundaryKind::filter, last->fault_context, filter, {last->pointers_address}));
    if (result != kExceptionContinueExecution) return false;
    Arm64Context resumed;
    if (!read_guest(last->context_address, &resumed, sizeof(resumed))) resumed = *last->fault_context;
    arm64_to_guest_context(resumed, ctx);
    ctx.pc = resumed.Pc;
    return true;
}

// Runs the guest's unhandled-exception filter, then the personality hook,
// each on `guest` loaded with the exception context. True: resume at guest.
bool run_fault_hooks(GuestContext& guest, const Arm64Context& fault_context, const GuestExceptionRecord& record,
                     std::uint64_t pointers_address, std::uint64_t context_address) {
    const LastChance last{&fault_context, pointers_address, context_address};
    struct Scope {
        const LastChance* saved;
        explicit Scope(const LastChance* current) : saved(t_last_chance) { t_last_chance = current; }
        ~Scope() { t_last_chance = saved; }
    } scope(&last);
    const GuestFaultInfo info = fault_info(record);
    // The guest filter runs as a nested call from the thread's current state
    // (as before the hook existed); it only writes `guest` when it resumes.
    if (unhandled_filter_fault_hook(guest, info, nullptr)) return true;
    const FaultHookEntry personality = personality_fault_hook();
    if (!personality.hook) return false;
    Arm64Context saved;
    guest_to_arm64_context(guest, saved, kCtxFull);
    const std::uint64_t saved_pc = guest.pc;
    arm64_to_guest_context(fault_context, guest);
    guest.pc = fault_context.Pc;
    if (personality.hook(guest, info, personality.user)) return true;
    arm64_to_guest_context(saved, guest);   // a declining hook's changes are discarded
    guest.pc = saved_pc;
    return false;
}

} // namespace

// ---------------------------------------------------------------------------
// First pass.
// ---------------------------------------------------------------------------
void dispatch_exception_internal(EntryScope& scope, const Arm64Context& fault_context,
                                 const GuestExceptionRecord& record_in) {
    GuestContext& guest = scope.ctx;
    SEH_TRACE("dispatch: code 0x%08x flags 0x%x at %s sp 0x%llx lr %s", record_in.code, record_in.flags,
              describe_guest_address(fault_context.Pc).c_str(), static_cast<unsigned long long>(fault_context.Sp),
              describe_guest_address(fault_context.Lr).c_str());
    g_dispatches.fetch_add(1, std::memory_order_relaxed);
    struct Nesting {
        Nesting() { ++t_state.dispatch_nesting; }
        ~Nesting() { --t_state.dispatch_nesting; }
    } nesting;
    GuestExceptionRecord record = record_in;
    if (t_state.dispatch_nesting > kMaxDispatchNesting) {
        terminate_process(record.code, "fx: guest exception dispatch nested too deeply (recursive faults)\n" +
                                           format_guest_stack(fault_context));
    }

    const std::uint64_t record_address = place_exception_record(record, 0);
    const std::uint64_t context_address = place_context(fault_context);
    GuestExceptionPointers pointers{record_address, context_address};
    const std::uint64_t pointers_address = allocate_guest_scratch(sizeof(pointers), 16);
    write_guest(pointers_address, &pointers, sizeof(pointers));
    auto resume_from_record = [&] {
        Arm64Context resumed;
        if (!read_guest(context_address, &resumed, sizeof(resumed))) resumed = fault_context;
        arm64_to_guest_context(resumed, guest);
        guest.pc = resumed.Pc;
    };
    auto call_continue_handlers = [&] {
        for (std::uint64_t routine : vectored_continue_routines()) {
            const auto result = static_cast<std::int32_t>(
                invoke_nested(BoundaryKind::filter, &fault_context, routine, {pointers_address}));
            if (result == kExceptionContinueExecution) break;
        }
    };

    // ---- vectored exception handlers ----
    for (std::uint64_t routine : vectored_exception_routines()) {
        // LONG result in w0: sign-extend from 32 bits.
        const auto result = static_cast<std::int32_t>(
            invoke_nested(BoundaryKind::filter, &fault_context, routine, {pointers_address}));
        if (result == kExceptionContinueExecution) {
            call_continue_handlers();
            resume_from_record();
            return;
        }
    }

    // ---- frame-based handlers ----
    Arm64Context context = fault_context;
    std::size_t limit = boundary_count();
    for (unsigned frames = 0; frames < kMaxFrames; ++frames) {
        FrameStep step;
        if (!step_frame(context, limit, kHandlerTypeException, step)) break;
        if (!within_bounds(guest, step.unwind.establisher_frame)) break;
        if (!step.unwind.handler) continue;

        Arm64NonvolatileRegisters nonvol{};
        fill_nonvolatile_registers(step.before, nonvol);
        const std::uint64_t nonvol_address = allocate_guest_scratch(sizeof(nonvol), 16);
        write_guest(nonvol_address, &nonvol, sizeof(nonvol));
        write_guest(record_address + offsetof(EXCEPTION_RECORD, ExceptionFlags), &record.flags, sizeof(record.flags));
        Arm64DispatcherContext dispatcher{};
        const std::uint32_t disposition = dispatch_to_handler(
            BoundaryKind::exception_handler, fault_context, step.unwind.handler, record_address,
            step.unwind.establisher_frame, context_address, context, dispatcher, step.function_entry,
            step.image_base, step.control_pc, step.unwind.handler_data, 0, nonvol_address,
            step.control_pc_is_unwound);

        switch (disposition) {
        case disposition_continue_execution:
            if (record.flags & kFlagNoncontinuable) {
                GuestExceptionRecord nested{};
                nested.code = kStatusNoncontinuable;
                nested.flags = kFlagNoncontinuable;
                nested.address = record.address;
                dispatch_exception_internal(scope, fault_context, nested);
                return;
            }
            call_continue_handlers();
            resume_from_record();
            return;
        case disposition_continue_search:
            break;
        case disposition_nested_exception:
            g_nested.fetch_add(1, std::memory_order_relaxed);
            break;
        case disposition_collided_unwind:
            g_collided.fetch_add(1, std::memory_order_relaxed);
            break;
        default: {
            GuestExceptionRecord nested{};
            nested.code = kStatusInvalidDisposition;
            nested.flags = kFlagNoncontinuable;
            nested.address = record.address;
            dispatch_exception_internal(scope, fault_context, nested);
            return;
        }
        }
    }

    // ---- last chance: guest unhandled-exception filter, personality hook ----
    if (run_fault_hooks(guest, fault_context, record, pointers_address, context_address)) return;

    // ---- unhandled: diagnose and terminate ----
    report_unhandled(record, fault_context);
}

std::size_t active_boundary_count() { return t_state.boundaries.size(); }

void reset_thread_state() {
    free_guest_scratch();
    t_state.boundaries.clear();
    t_state.entries.clear();
    t_state.pending = {};
    t_state.dispatch_nesting = 0;
}

DispatchStats dispatch_stats() {
    return {g_dispatches.load(), g_handler_calls.load(), g_unwind_handler_calls.load(),
            g_consolidations.load(), g_nested.load(), g_collided.load()};
}

std::uint64_t consolidate_count_bump() { return g_consolidations.fetch_add(1, std::memory_order_relaxed); }

} // namespace transmute::fx::seh

// Public entry points declared in process.hpp (transmute::fx namespace).
namespace transmute::fx {

void dispatch_guest_exception(GuestContext& ctx, const GuestExceptionRecord& record) {
    seh::run_entry(ctx, [&](seh::EntryScope& scope) {
        seh::Arm64Context context;
        seh::guest_to_arm64_context(ctx, context, seh::kCtxFull);
        seh::dispatch_exception_internal(scope, context, record);
        return GuestResolution::continue_at_pc;
    });
}

void dispatch_guest_fault(GuestContext& ctx, const GuestFault& fault) {
    GuestExceptionRecord record{};
    record.address = fault.faulting_pc ? fault.faulting_pc : ctx.pc;
    record.flags = 0;
    using Kind = GuestFault::Kind;
    switch (fault.kind) {
    case Kind::access_violation:
        record.code = seh::kStatusAccessViolation;
        record.parameter_count = 2;
        record.parameters[0] = fault.access_type;
        record.parameters[1] = fault.access_address;
        break;
    case Kind::in_page_error:
        record.code = seh::kStatusInPageError;
        record.parameter_count = 3;
        record.parameters[0] = fault.access_type;
        record.parameters[1] = fault.access_address;
        record.parameters[2] = fault.nt_status;
        break;
    case Kind::illegal_instruction: record.code = seh::kStatusIllegalInstruction; break;
    case Kind::privileged_instruction: record.code = seh::kStatusPrivilegedInstruction; break;
    case Kind::stack_overflow: record.code = seh::kStatusStackOverflow; break;
    case Kind::breakpoint: record.code = seh::kStatusBreakpoint; break;
    case Kind::datatype_misalignment: record.code = seh::kStatusDatatypeMisalignment; break;
    case Kind::guard_page:
        record.code = seh::kStatusGuardPageViolation;
        record.parameter_count = 2;
        record.parameters[0] = fault.access_type;
        record.parameters[1] = fault.access_address;
        break;
    case Kind::integer_overflow: record.code = seh::kStatusIntegerOverflow; break;
    }
    dispatch_guest_exception(ctx, record);
}

} // namespace transmute::fx
