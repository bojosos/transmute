// Guest-visible SEH/unwind entry points bound by the abi component.
// Each reads ARM64 arguments from x0..x7 and reports how the thread resumes.
// Declared in transmute/fx/process.hpp (namespace transmute::fx).

#include "fx/seh/dispatch.hpp"
#include "fx/seh/vectored.hpp"

#include <cstring>

namespace transmute::fx {
namespace {

using namespace transmute::fx::seh;

std::uint64_t arg(const GuestContext& ctx, unsigned index) { return ctx.x[index]; }
void ret(GuestContext& ctx, std::uint64_t value) { ctx.x[0] = value; }

std::uint32_t unwind_failure_code(UnwindOutcome::Status status) {
    switch (status) {
    case UnwindOutcome::Status::bad_stack: return kStatusBadStack;
    case UnwindOutcome::Status::invalid_disposition: return kStatusInvalidDisposition;
    default: return kStatusInvalidUnwindTarget;
    }
}

// Runs the unwinding pass from the caller of the entry point and transfers
// to the target. A failed unwind raises a noncontinuable exception from the
// caller, as RtlUnwindEx does.
GuestResolution unwind_and_transfer(EntryScope& scope, UnwindRequest& request) {
    const Arm64Context start = caller_context(scope.ctx);
    UnwindOutcome outcome = unwind_to_target(scope, start, request);
    if (outcome.status != UnwindOutcome::Status::transferred) {
        GuestExceptionRecord failure{};
        failure.code = unwind_failure_code(outcome.status);
        failure.flags = kFlagNoncontinuable;
        failure.address = start.Pc;
        dispatch_exception_internal(scope, start, failure);
        // A noncontinuable exception cannot be resumed.
        terminate_process(failure.code, "fx: unwind failed and the failure was not handled\n");
    }
    return transfer_to(scope, outcome.target);
}

GuestResolution do_unwind(GuestContext& ctx, std::uint64_t target_frame, std::uint64_t target_ip,
                          std::uint64_t record_ptr) {
    const std::uint64_t return_value = arg(ctx, 3);
    return run_entry(ctx, [&](EntryScope& scope) {
        UnwindRequest request;
        request.target_frame = target_frame ? target_frame : kUnwindAllFrames;
        request.target_pc = target_ip;
        request.return_value = return_value;
        if (record_ptr) {
            request.record = read_exception_record(record_ptr);
            request.record_address = record_ptr;
        } else {
            request.record.code = kStatusUnwind;
            request.record.address = ctx.x[30];
        }
        return unwind_and_transfer(scope, request);
    });
}

GuestResolution raise(GuestContext& ctx, const GuestExceptionRecord& record) {
    return run_entry(ctx, [&](EntryScope& scope) {
        // The exception context is the caller's: PC = return address, so a
        // handler that continues execution resumes after the call.
        dispatch_exception_internal(scope, caller_context(ctx), record);
        return GuestResolution::continue_at_pc;
    });
}

} // namespace

void guest_raise_exception(GuestContext& ctx, std::uint32_t code, std::uint32_t flags, std::uint32_t count,
                           const std::uint64_t* arguments) {
    GuestExceptionRecord record{};
    record.code = code;
    record.flags = flags & kFlagNoncontinuable;
    record.address = ctx.x[30];
    record.parameter_count = count > 15 ? 15 : count;
    for (unsigned i = 0; i < record.parameter_count; ++i) record.parameters[i] = arguments[i];
    raise(ctx, record);
}

GuestResolution guest_RaiseException(GuestContext& ctx) {
    GuestExceptionRecord record{};
    record.code = static_cast<std::uint32_t>(arg(ctx, 0));
    record.flags = static_cast<std::uint32_t>(arg(ctx, 1)) & kFlagNoncontinuable;
    const std::uint32_t count = static_cast<std::uint32_t>(arg(ctx, 2));
    const std::uint64_t args_ptr = arg(ctx, 3);
    record.address = ctx.x[30];   // caller of RaiseException
    record.parameter_count = args_ptr == 0 ? 0 : (count > 15 ? 15 : count);
    for (unsigned i = 0; i < record.parameter_count; ++i)
        read_guest_value(args_ptr + 8ull * i, record.parameters[i]);
    return raise(ctx, record);
}

GuestResolution guest_RtlCaptureContext(GuestContext& ctx) {
    Arm64Context context;
    guest_to_arm64_context(ctx, context, kCtxFull);
    context.Pc = ctx.x[30];   // the state as seen by the caller
    write_guest(arg(ctx, 0), &context, sizeof(context));
    return GuestResolution::continue_at_caller;
}

GuestResolution guest_RtlRestoreContext(GuestContext& ctx) {
    Arm64Context context;
    if (!read_guest(arg(ctx, 0), &context, sizeof(context))) return GuestResolution::continue_at_caller;
    const std::uint64_t record_ptr = arg(ctx, 1);
    return run_entry(ctx, [&](EntryScope& scope) {
        if (record_ptr) apply_restore_record(context, record_ptr);
        return transfer_to(scope, context);
    });
}

GuestResolution guest_RtlVirtualUnwind(GuestContext& ctx) {
    const std::uint32_t handler_type = static_cast<std::uint32_t>(arg(ctx, 0));
    const std::uint64_t image_base = arg(ctx, 1);
    const std::uint64_t control_pc = arg(ctx, 2);
    const std::uint64_t entry_ptr = arg(ctx, 3);
    const std::uint64_t context_ptr = arg(ctx, 4);
    const std::uint64_t handler_data_out = arg(ctx, 5);
    const std::uint64_t establisher_out = arg(ctx, 6);
    const std::uint64_t pointers_ptr = arg(ctx, 7);

    Arm64RuntimeFunction entry{};
    Arm64Context context{};
    read_guest_value(entry_ptr, entry);
    read_guest(context_ptr, &context, sizeof(context));
    Arm64ContextPointers pointers{};
    Arm64ContextPointers* pointers_arg = pointers_ptr ? &pointers : nullptr;
    if (pointers_arg) read_guest(pointers_ptr, &pointers, sizeof(pointers));

    VirtualUnwindResult result =
        virtual_unwind(handler_type, image_base, control_pc, entry, context, pointers_arg);

    write_guest(context_ptr, &context, sizeof(context));
    if (handler_data_out) write_guest(handler_data_out, &result.handler_data, sizeof(std::uint64_t));
    if (establisher_out) write_guest(establisher_out, &result.establisher_frame, sizeof(std::uint64_t));
    if (pointers_arg) write_guest(pointers_ptr, &pointers, sizeof(pointers));
    ret(ctx, result.handler);
    return GuestResolution::continue_at_caller;
}

GuestResolution guest_RtlLookupFunctionEntry(GuestContext& ctx) {
    // Callback tables may run guest code, so this is an entry point too.
    return run_entry(ctx, [&](EntryScope&) {
        std::uint64_t image_base = 0;
        const std::uint64_t entry = lookup_function_entry(arg(ctx, 0), &image_base);
        if (const std::uint64_t out = arg(ctx, 1)) write_guest(out, &image_base, sizeof(image_base));
        ret(ctx, entry);
        return GuestResolution::continue_at_caller;
    });
}

GuestResolution guest_RtlUnwind(GuestContext& ctx) {
    return do_unwind(ctx, arg(ctx, 0), arg(ctx, 1), arg(ctx, 2));
}

GuestResolution guest_RtlUnwindEx(GuestContext& ctx) {
    return do_unwind(ctx, arg(ctx, 0), arg(ctx, 1), arg(ctx, 2));
}

GuestResolution guest_RtlAddFunctionTable(GuestContext& ctx) {
    ret(ctx, add_function_table(arg(ctx, 0), static_cast<std::uint32_t>(arg(ctx, 1)), arg(ctx, 2)) ? 1 : 0);
    return GuestResolution::continue_at_caller;
}

GuestResolution guest_RtlDeleteFunctionTable(GuestContext& ctx) {
    ret(ctx, delete_function_table(arg(ctx, 0)) ? 1 : 0);
    return GuestResolution::continue_at_caller;
}

GuestResolution guest_RtlInstallFunctionTableCallback(GuestContext& ctx) {
    const bool ok = install_function_table_callback(arg(ctx, 0), arg(ctx, 1),
                                                     static_cast<std::uint32_t>(arg(ctx, 2)),
                                                     arg(ctx, 3), arg(ctx, 4));
    ret(ctx, ok ? 1 : 0);
    return GuestResolution::continue_at_caller;
}

GuestResolution guest_RtlAddGrowableFunctionTable(GuestContext& ctx) {
    std::uint64_t handle = 0;
    const std::uint32_t status = add_growable_function_table(
        &handle, arg(ctx, 1), static_cast<std::uint32_t>(arg(ctx, 2)),
        static_cast<std::uint32_t>(arg(ctx, 3)), arg(ctx, 4), arg(ctx, 5));
    if (status == 0 && arg(ctx, 0)) write_guest(arg(ctx, 0), &handle, sizeof(handle));
    ret(ctx, status);
    return GuestResolution::continue_at_caller;
}

GuestResolution guest_RtlGrowFunctionTable(GuestContext& ctx) {
    grow_function_table(arg(ctx, 0), static_cast<std::uint32_t>(arg(ctx, 1)));
    return GuestResolution::continue_at_caller;
}

GuestResolution guest_RtlDeleteGrowableFunctionTable(GuestContext& ctx) {
    delete_growable_function_table(arg(ctx, 0));
    return GuestResolution::continue_at_caller;
}

GuestResolution guest_SetUnhandledExceptionFilter(GuestContext& ctx) {
    ret(ctx, set_unhandled_exception_filter(arg(ctx, 0)));
    return GuestResolution::continue_at_caller;
}

GuestResolution guest_UnhandledExceptionFilter(GuestContext& ctx) {
    const std::uint64_t pointers = arg(ctx, 0);
    return run_entry(ctx, [&](EntryScope&) {
        const std::uint64_t filter = unhandled_exception_filter_routine();
        if (filter) {
            const Arm64Context caller = caller_context(ctx);
            const auto result = static_cast<std::int32_t>(
                invoke_nested(BoundaryKind::filter, &caller, filter, {pointers}));
            ret(ctx, static_cast<std::uint32_t>(result));
        } else {
            ret(ctx, 1);   // EXCEPTION_EXECUTE_HANDLER: run the default (terminating) handler
        }
        return GuestResolution::continue_at_caller;
    });
}

GuestResolution guest_AddVectoredExceptionHandler(GuestContext& ctx) {
    ret(ctx, add_vectored_exception_handler(arg(ctx, 0) != 0, arg(ctx, 1)));
    return GuestResolution::continue_at_caller;
}

GuestResolution guest_AddVectoredContinueHandler(GuestContext& ctx) {
    ret(ctx, add_vectored_continue_handler(arg(ctx, 0) != 0, arg(ctx, 1)));
    return GuestResolution::continue_at_caller;
}

GuestResolution guest_RemoveVectoredExceptionHandler(GuestContext& ctx) {
    ret(ctx, remove_vectored_exception_handler(arg(ctx, 0)) ? 1 : 0);
    return GuestResolution::continue_at_caller;
}

} // namespace transmute::fx
