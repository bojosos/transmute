// End-to-end exception dispatch with the interpreter-backed call_guest stub.
// Two frame-chained functions (C calls F) with real .xdata; language handlers
// are host-entry stand-ins reached through call_guest, exactly as guest
// __C_specific_handler would be.

#include "fx/seh/dispatch.hpp"
#include "fx/seh/seh.hpp"
#include "fx/seh/tests/image_builder.hpp"
#include "fx/seh/tests/seh_test_util.hpp"
#include "fx/seh/vectored.hpp"
#include "fx/testing/test.hpp"

#include <stdexcept>
#include <string>
#include <vector>

using namespace transmute::fx;
using namespace transmute::fx::seh;
namespace t = fxseh_test;

namespace {

constexpr std::uint32_t kInstrs = 8;   // canonical function (see test_unwind.cpp)
constexpr std::uint64_t kCallerReturn = 0;   // C's caller: end of the walk

struct HandlerCall {
    std::uint32_t flags;
    std::uint64_t establisher;
    std::uint64_t control_pc;
    std::uint64_t target_pc;
};

// Module with F (callee) and C (caller), both canonical:
//   stp x19,x20,[sp,#-16]! ; stp x29,x30,[sp,#-0x90]! ; mov x29,sp ; nop ;
//   mov sp,x29 ; ldp x29,x30,[sp],#0x90 ; ldp x19,x20,[sp],#16 ; ret
struct Scenario {
    t::ImageBuilder image{64 * 1024};
    std::uint64_t f = 0, c = 0;
    std::uint64_t handler_f = 0, handler_c = 0;   // host-entry addresses inside the module
    std::uint64_t pdata = 0;
    // Stack: S0 = C's entry SP; C body SP = c_sp; F body SP = f_sp.
    std::uint64_t s0 = 0, c_sp = 0, f_sp = 0;
    std::uint64_t c_x19 = 0x1919, c_x20 = 0x2020;   // C's values (saved by F)
    std::uint64_t caller_x19 = 0xC19, caller_x20 = 0xC20;  // caller's values (saved by C)
};

Scenario build(bool f_has_handler, bool c_has_handler) {
    Scenario s;
    std::vector<std::uint32_t> words(kInstrs, t::a64::nop());
    s.f = s.image.put(words.data(), words.size() * 4, 16);
    s.c = s.image.put(words.data(), words.size() * 4, 16);
    s.handler_f = s.image.reserve_bytes(16, 16);
    s.handler_c = s.image.reserve_bytes(16, 16);
    const std::vector<std::uint8_t> codes = {0xe1, 0x91, 0x22, 0xe4};
    const std::uint64_t xf = t::build_xdata(s.image, kInstrs, codes, true, 0, {},
                                            f_has_handler ? s.image.rva(s.handler_f) : 0, {0});
    const std::uint64_t xc = t::build_xdata(s.image, kInstrs, codes, true, 0, {},
                                            c_has_handler ? s.image.rva(s.handler_c) : 0, {0});
    // .pdata sorted by BeginAddress (f < c).
    s.pdata = t::build_pdata(s.image, s.image.rva(s.f), s.image.rva(xf));
    t::build_pdata(s.image, s.image.rva(s.c), s.image.rva(xc));
    s.image.register_as_module(s.pdata, 2);

    // Frames: C's frame below S0, F's frame below C's body SP.
    const std::uint64_t region = t::alloc_guest(0x2000, 0x1000);
    s.s0 = region + 0x1800;
    const std::uint64_t c_regs = s.s0 - 0x10;
    s.c_sp = c_regs - 0x90;
    t::store64(c_regs, s.caller_x19);
    t::store64(c_regs + 8, s.caller_x20);
    t::store64(s.c_sp, 0xCA11E2F9);        // caller's fp
    t::store64(s.c_sp + 8, kCallerReturn);  // C returns to 0: walk ends
    const std::uint64_t f_regs = s.c_sp - 0x10;
    s.f_sp = f_regs - 0x90;
    t::store64(f_regs, s.c_x19);
    t::store64(f_regs + 8, s.c_x20);
    t::store64(s.f_sp, s.c_sp);             // F saved C's fp
    t::store64(s.f_sp + 8, s.c + 0x0c);      // F returns into C's body
    return s;
}

GuestContext fault_in_f(const Scenario& s) {
    GuestContext ctx{};
    t::ThreadContextScope thread_scope(ctx);
    ctx.pc = s.f + 0x0c;   // F's body
    ctx.x[31] = s.f_sp;
    ctx.x[29] = s.f_sp;
    ctx.x[30] = s.c + 0x0c;
    ctx.x[19] = 0xF19;
    ctx.x[20] = 0xF20;
    return ctx;
}

GuestExceptionRecord make_record(std::uint32_t code, std::uint64_t address) {
    GuestExceptionRecord record{};
    record.code = code;
    record.address = address;
    record.parameter_count = 1;
    record.parameters[0] = 0x5151;
    return record;
}

HandlerCall capture(const GuestContext& ctx) {
    HandlerCall call{};
    EXCEPTION_RECORD record{};
    read_guest(ctx.x[0], &record, sizeof(record));
    Arm64DispatcherContext dc{};
    read_guest(ctx.x[3], &dc, sizeof(dc));
    call.flags = record.ExceptionFlags;
    call.establisher = ctx.x[1];
    call.control_pc = dc.ControlPc;
    call.target_pc = dc.TargetPc;
    return call;
}

struct TerminationCapture {
    static inline std::string report;
    static inline std::uint32_t code = 0;
    static void hook(std::uint32_t exit_code, const std::string& text) {
        report = text;
        code = exit_code;
        throw std::runtime_error("terminated");
    }
};

} // namespace

FX_TEST(dispatch_handler_continue_execution) {
    t::reset_all();
    Scenario s = build(true, false);
    std::vector<HandlerCall> calls;
    t::register_host_entry(s.handler_f, [&](GuestContext& ctx) {
        calls.push_back(capture(ctx));
        // Skip the faulting instruction in the original context, then resume.
        Arm64Context original{};
        read_guest(ctx.x[2], &original, sizeof(original));
        original.Pc += 4;
        write_guest(ctx.x[2], &original, sizeof(original));
        ctx.x[0] = disposition_continue_execution;
        return GuestResolution::continue_at_caller;
    });
    GuestContext ctx = fault_in_f(s);
    t::ThreadContextScope thread_scope(ctx);
    dispatch_guest_exception(ctx, make_record(0xE0001234, ctx.pc));
    FX_REQUIRE(calls.size() == 1);
    FX_CHECK_EQ(calls[0].establisher, s.c_sp);          // F's entry SP
    FX_CHECK_EQ(calls[0].control_pc, s.f + 0x0c);
    FX_CHECK_EQ(calls[0].flags & kFlagUnwindMask, 0u);  // first pass
    FX_CHECK_EQ(ctx.pc, s.f + 0x10);                    // resumed after the fault
    FX_CHECK_EQ(ctx.x[19], 0xF19u);                     // registers preserved
}

FX_TEST(dispatch_caught_by_caller_via_rtl_unwind_ex) {
    t::reset_all();
    Scenario s = build(true, true);
    std::vector<HandlerCall> f_calls, c_calls;
    const std::uint64_t except_block = s.c + 0x14;   // C's "__except" continuation
    t::register_host_entry(s.handler_f, [&](GuestContext& ctx) {
        f_calls.push_back(capture(ctx));
        ctx.x[0] = disposition_continue_search;
        return GuestResolution::continue_at_caller;
    });
    t::register_host_entry(s.handler_c, [&](GuestContext& ctx) {
        HandlerCall call = capture(ctx);
        c_calls.push_back(call);
        if ((call.flags & kFlagUnwinding) == 0) {
            // First pass: the filter accepts; unwind to our frame (like
            // __C_specific_handler calling RtlUnwindEx).
            const std::uint64_t record = ctx.x[0];
            const std::uint64_t establisher = ctx.x[1];
            const std::uint64_t context = ctx.x[2];
            ctx.x[0] = establisher;
            ctx.x[1] = except_block;
            ctx.x[2] = record;
            ctx.x[3] = 0x77;        // return value
            ctx.x[4] = context;
            ctx.x[5] = 0;
            // RtlUnwindEx ends this nested handler call itself.
            const GuestResolution resolution = guest_RtlUnwindEx(ctx);
            FX_CHECK_EQ(ctx.pc, kReturnToHostPc);
            return resolution;
        }
        ctx.x[0] = disposition_continue_search;
        return GuestResolution::continue_at_caller;
    });

    GuestContext ctx = fault_in_f(s);
    t::ThreadContextScope thread_scope(ctx);
    dispatch_guest_exception(ctx, make_record(kStatusAccessViolation, ctx.pc));

    // First pass: F then C. Second pass: F (unwinding), C (target unwind).
    FX_REQUIRE(f_calls.size() == 2);
    FX_REQUIRE(c_calls.size() == 2);
    FX_CHECK_EQ(f_calls[0].flags & kFlagUnwindMask, 0u);
    FX_CHECK_EQ(c_calls[0].flags & kFlagUnwindMask, 0u);
    FX_CHECK_EQ(c_calls[0].establisher, s.s0);
    FX_CHECK((f_calls[1].flags & kFlagUnwinding) != 0);
    FX_CHECK((f_calls[1].flags & kFlagTargetUnwind) == 0);
    FX_CHECK_EQ(f_calls[1].target_pc, except_block);
    FX_CHECK((c_calls[1].flags & kFlagUnwinding) != 0);
    FX_CHECK((c_calls[1].flags & kFlagTargetUnwind) != 0);

    // Resumed in C's frame at the continuation.
    FX_CHECK_EQ(ctx.pc, except_block);
    FX_CHECK_EQ(ctx.x[31], s.c_sp);
    FX_CHECK_EQ(ctx.x[29], s.c_sp);
    FX_CHECK_EQ(ctx.x[19], s.c_x19);   // restored from F's save area
    FX_CHECK_EQ(ctx.x[20], s.c_x20);
    FX_CHECK_EQ(ctx.x[0], 0x77u);
}

FX_TEST(dispatch_vectored_handler_first) {
    t::reset_all();
    Scenario s = build(true, false);
    bool frame_handler_called = false;
    t::register_host_entry(s.handler_f, [&](GuestContext& ctx) {
        frame_handler_called = true;
        ctx.x[0] = disposition_continue_search;
        return GuestResolution::continue_at_caller;
    });
    const std::uint64_t veh = s.image.reserve_bytes(16, 16);
    std::uint32_t seen_code = 0;
    t::register_host_entry(veh, [&](GuestContext& ctx) {
        std::uint64_t pointers[2];
        read_guest(ctx.x[0], pointers, sizeof(pointers));
        EXCEPTION_RECORD record{};
        read_guest(pointers[0], &record, sizeof(record));
        seen_code = record.ExceptionCode;
        Arm64Context context{};
        read_guest(pointers[1], &context, sizeof(context));
        context.Pc = 0x1234'5678;
        write_guest(pointers[1], &context, sizeof(context));
        ctx.x[0] = 0xFFFFFFFFu;   // LONG EXCEPTION_CONTINUE_EXECUTION in w0
        return GuestResolution::continue_at_caller;
    });
    GuestContext add{};
    add.x[0] = 1;
    add.x[1] = veh;
    guest_AddVectoredExceptionHandler(add);
    const std::uint64_t handle = add.x[0];
    FX_CHECK(handle != 0);

    GuestContext ctx = fault_in_f(s);
    t::ThreadContextScope thread_scope(ctx);
    dispatch_guest_exception(ctx, make_record(0xE0000001, ctx.pc));
    FX_CHECK_EQ(seen_code, 0xE0000001u);
    FX_CHECK(!frame_handler_called);
    FX_CHECK_EQ(ctx.pc, 0x1234'5678u);

    GuestContext remove{};
    remove.x[0] = handle;
    guest_RemoveVectoredExceptionHandler(remove);
    FX_CHECK_EQ(remove.x[0], 1u);
}

FX_TEST(dispatch_unhandled_filter_continue) {
    t::reset_all();
    Scenario s = build(false, false);
    const std::uint64_t filter = s.image.reserve_bytes(16, 16);
    int filter_calls = 0;
    t::register_host_entry(filter, [&](GuestContext& ctx) {
        ++filter_calls;
        std::uint64_t pointers[2];
        read_guest(ctx.x[0], pointers, sizeof(pointers));
        Arm64Context context{};
        read_guest(pointers[1], &context, sizeof(context));
        context.Pc = 0xABCD'0000;
        write_guest(pointers[1], &context, sizeof(context));
        ctx.x[0] = 0xFFFFFFFFu;   // LONG -1 in w0, upper bits clear
        return GuestResolution::continue_at_caller;
    });
    GuestContext set{};
    set.x[0] = filter;
    guest_SetUnhandledExceptionFilter(set);
    FX_CHECK_EQ(set.x[0], 0u);   // no previous filter

    GuestContext ctx = fault_in_f(s);
    t::ThreadContextScope thread_scope(ctx);
    dispatch_guest_exception(ctx, make_record(0xE0000002, ctx.pc));
    FX_CHECK_EQ(filter_calls, 1);
    FX_CHECK_EQ(ctx.pc, 0xABCD'0000u);
}

FX_TEST(dispatch_unhandled_terminates_with_diagnostic) {
    t::reset_all();
    Scenario s = build(false, false);
    auto previous = set_termination_hook(&TerminationCapture::hook);
    GuestContext ctx = fault_in_f(s);
    t::ThreadContextScope thread_scope(ctx);
    bool terminated = false;
    try {
        dispatch_guest_exception(ctx, make_record(kStatusIllegalInstruction, ctx.pc));
    } catch (const std::runtime_error&) {
        terminated = true;
    }
    set_termination_hook(previous);
    FX_CHECK(terminated);
    FX_CHECK_EQ(TerminationCapture::code, kStatusIllegalInstruction);
    const std::string& report = TerminationCapture::report;
    FX_CHECK(report.find("c000001d") != std::string::npos);
    FX_CHECK(report.find("test.dll+0x") != std::string::npos);   // module+offset
    FX_CHECK(report.find("#1") != std::string::npos);           // stack trace has the caller
}

FX_TEST(noncontinuable_continue_raises_nested) {
    t::reset_all();
    Scenario s = build(true, false);
    std::vector<std::uint32_t> codes;
    t::register_host_entry(s.handler_f, [&](GuestContext& ctx) {
        EXCEPTION_RECORD record{};
        read_guest(ctx.x[0], &record, sizeof(record));
        codes.push_back(record.ExceptionCode);
        ctx.x[0] = record.ExceptionCode == kStatusNoncontinuable ? disposition_continue_search
                                                                 : disposition_continue_execution;
        return GuestResolution::continue_at_caller;
    });
    auto previous = set_termination_hook(&TerminationCapture::hook);
    GuestContext ctx = fault_in_f(s);
    t::ThreadContextScope thread_scope(ctx);
    GuestExceptionRecord record = make_record(0xE0000003, ctx.pc);
    record.flags = kFlagNoncontinuable;
    bool terminated = false;
    try {
        dispatch_guest_exception(ctx, record);
    } catch (const std::runtime_error&) {
        terminated = true;
    }
    set_termination_hook(previous);
    FX_CHECK(terminated);
    FX_REQUIRE(codes.size() == 2);
    FX_CHECK_EQ(codes[0], 0xE0000003u);
    FX_CHECK_EQ(codes[1], kStatusNoncontinuable);
}

FX_TEST(rtl_unwind_ex_standalone_runs_termination_handlers) {
    t::reset_all();
    Scenario s = build(true, false);
    std::vector<HandlerCall> f_calls;
    t::register_host_entry(s.handler_f, [&](GuestContext& ctx) {
        f_calls.push_back(capture(ctx));
        ctx.x[0] = disposition_continue_search;
        return GuestResolution::continue_at_caller;
    });
    // F calls RtlUnwindEx(C's frame, target) directly: x30 = return into F's body.
    GuestContext ctx{};
    t::ThreadContextScope thread_scope(ctx);
    ctx.x[31] = s.f_sp;
    ctx.x[29] = s.f_sp;
    ctx.x[30] = s.f + 0x0c;
    ctx.x[0] = s.s0;            // TargetFrame = C's establisher
    ctx.x[1] = s.c + 0x18;      // TargetIp
    ctx.x[2] = 0;               // no record: STATUS_UNWIND
    ctx.x[3] = 0x99;
    ctx.x[4] = 0;
    ctx.x[5] = 0;
    FX_CHECK(guest_RtlUnwindEx(ctx) == GuestResolution::continue_at_pc);
    FX_REQUIRE(f_calls.size() == 1);
    FX_CHECK((f_calls[0].flags & kFlagUnwinding) != 0);
    FX_CHECK_EQ(ctx.pc, s.c + 0x18);
    FX_CHECK_EQ(ctx.x[31], s.c_sp);
    FX_CHECK_EQ(ctx.x[19], s.c_x19);
    FX_CHECK_EQ(ctx.x[0], 0x99u);
}

FX_TEST(longjmp_unwinds_to_setjmp_frame) {
    t::reset_all();
    Scenario s = build(true, false);
    int f_unwinds = 0;
    t::register_host_entry(s.handler_f, [&](GuestContext& ctx) {
        HandlerCall call = capture(ctx);
        if (call.flags & kFlagUnwinding) ++f_unwinds;
        ctx.x[0] = disposition_continue_search;
        return GuestResolution::continue_at_caller;
    });
    // _JUMP_BUFFER captured in C's body: Frame = C's establisher (S0).
    const std::uint64_t jb = t::alloc_guest(24 * 8, 16);
    std::uint64_t buffer[24] = {};
    buffer[0] = s.s0;                    // Frame
    for (unsigned i = 0; i < 10; ++i) buffer[2 + i] = 0x5000 + i;   // x19..x28
    buffer[12] = s.c_sp;                 // Fp
    buffer[13] = s.c + 0x1c;             // Lr (setjmp return address)
    buffer[14] = s.c_sp;                 // Sp
    buffer[15] = 0x00000002ull << 32 | 0x00c00000u;   // Fpcr = RZ, Fpsr = DZC (setjmp time)
    std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(jb)), buffer, sizeof(buffer));

    GuestContext ctx{};
    t::ThreadContextScope thread_scope(ctx);
    ctx.x[31] = s.f_sp;
    ctx.x[29] = s.f_sp;
    ctx.x[30] = s.f + 0x0c;   // longjmp called from F's body
    ctx.x[0] = jb;
    ctx.x[1] = 0;             // longjmp(buf, 0) delivers 1
    ctx.fpcr = 0;
    ctx.fpsr = 0x11;          // IOC IXC at longjmp time
    FX_CHECK(guest_longjmp(ctx) == GuestResolution::continue_at_pc);
    FX_CHECK_EQ(f_unwinds, 1);
    // The jump buffer's FPCR/FPSR (the override thunk loads them into MXCSR).
    FX_CHECK_EQ(ctx.fpcr, 0x00c00000u);
    FX_CHECK_EQ(ctx.fpsr, 0x2u);
    FX_CHECK_EQ(ctx.pc, s.c + 0x1c);
    FX_CHECK_EQ(ctx.x[31], s.c_sp);
    FX_CHECK_EQ(ctx.x[19], 0x5000u);
    FX_CHECK_EQ(ctx.x[28], 0x5009u);
    FX_CHECK_EQ(ctx.x[0], 1u);
}

FX_TEST(capture_and_restore_context) {
    t::reset_all();
    const std::uint64_t buffer = t::alloc_guest(sizeof(Arm64Context), 16);
    GuestContext ctx{};
    t::ThreadContextScope thread_scope(ctx);
    for (unsigned i = 0; i < 31; ++i) ctx.x[i] = 0x100 + i;
    ctx.x[0] = buffer;
    ctx.x[30] = 0x4000'0010;   // return address
    ctx.x[31] = 0x7000'0000;
    ctx.nzcv = 0x60000000;
    // Architectural FPCR/FPSR (the override thunk folded the MXCSR flags in).
    ctx.fpcr = 0x01400000;   // FZ, RP
    ctx.fpsr = 0x93;         // IDC IXC DZC IOC
    FX_CHECK(guest_RtlCaptureContext(ctx) == GuestResolution::continue_at_caller);
    Arm64Context captured{};
    read_guest(buffer, &captured, sizeof(captured));
    FX_CHECK_EQ(captured.Pc, 0x4000'0010u);
    FX_CHECK_EQ(captured.Sp, 0x7000'0000u);
    FX_CHECK_EQ(captured.X[5], 0x105u);
    FX_CHECK_EQ(captured.Cpsr, 0x60000000u);
    FX_CHECK_EQ(captured.Fpcr, 0x01400000u);
    FX_CHECK_EQ(captured.Fpsr, 0x93u);

    GuestContext other{};
    other.x[0] = buffer;
    other.x[1] = 0;
    FX_CHECK(guest_RtlRestoreContext(other) == GuestResolution::continue_at_pc);
    FX_CHECK_EQ(other.pc, 0x4000'0010u);
    FX_CHECK_EQ(other.x[5], 0x105u);
    FX_CHECK_EQ(other.x[31], 0x7000'0000u);
    FX_CHECK_EQ(other.fpcr, 0x01400000u);
    FX_CHECK_EQ(other.fpsr, 0x93u);
}

FX_TEST(rtl_lookup_and_virtual_unwind_guest_api) {
    t::reset_all();
    Scenario s = build(false, false);
    const std::uint64_t out_base = t::alloc_guest(8, 8);
    GuestContext ctx{};
    t::ThreadContextScope thread_scope(ctx);
    ctx.x[0] = s.f + 0x0c;
    ctx.x[1] = out_base;
    guest_RtlLookupFunctionEntry(ctx);
    FX_CHECK_EQ(ctx.x[0], s.pdata);
    FX_CHECK_EQ(t::load64(out_base), s.image.base);

    // RtlVirtualUnwind on a guest CONTEXT.
    const std::uint64_t context_address = t::alloc_guest(sizeof(Arm64Context), 16);
    Arm64Context context{};
    context.ContextFlags = kCtxFull;
    context.Pc = s.f + 0x0c;
    context.Sp = s.f_sp;
    context.Fp = s.f_sp;
    write_guest(context_address, &context, sizeof(context));
    const std::uint64_t handler_data = t::alloc_guest(8, 8);
    const std::uint64_t establisher = t::alloc_guest(8, 8);
    GuestContext vu{};
    vu.x[0] = 0;
    vu.x[1] = s.image.base;
    vu.x[2] = s.f + 0x0c;
    vu.x[3] = s.pdata;
    vu.x[4] = context_address;
    vu.x[5] = handler_data;
    vu.x[6] = establisher;
    vu.x[7] = 0;
    guest_RtlVirtualUnwind(vu);
    read_guest(context_address, &context, sizeof(context));
    FX_CHECK_EQ(context.Pc, s.c + 0x0c);
    FX_CHECK_EQ(context.Sp, s.c_sp);
    FX_CHECK_EQ(t::load64(establisher), s.c_sp);
}

FX_TEST(dynamic_table_api_roundtrip) {
    t::reset_all();
    const std::uint64_t base = t::alloc_guest(0x10000, 0x10000);
    const std::uint64_t table = base + 0x8000;
    std::uint32_t entry[2] = {0x2000, 2u | ((0x40u / 4) << 2)};
    std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(table)), entry, 8);
    GuestContext add{};
    add.x[0] = table;
    add.x[1] = 1;
    add.x[2] = base;
    guest_RtlAddFunctionTable(add);
    FX_CHECK_EQ(add.x[0], 1u);
    std::uint64_t image_base = 0;
    FX_CHECK_EQ(lookup_function_entry(base + 0x2010, &image_base), table);
    GuestContext del{};
    del.x[0] = table;
    guest_RtlDeleteFunctionTable(del);
    FX_CHECK_EQ(del.x[0], 1u);
    FX_CHECK_EQ(lookup_function_entry(base + 0x2010, &image_base), 0u);
}

FX_TEST(raise_exception_api_dispatches) {
    t::reset_all();
    Scenario s = build(true, false);
    std::vector<std::uint64_t> params;
    std::uint32_t code = 0;
    t::register_host_entry(s.handler_f, [&](GuestContext& ctx) {
        EXCEPTION_RECORD record{};
        read_guest(ctx.x[0], &record, sizeof(record));
        code = record.ExceptionCode;
        for (unsigned i = 0; i < record.NumberParameters; ++i) params.push_back(record.ExceptionInformation[i]);
        ctx.x[0] = disposition_continue_execution;
        return GuestResolution::continue_at_caller;
    });
    const std::uint64_t args = t::alloc_guest(16, 8);
    t::store64(args, 0xAA);
    t::store64(args + 8, 0xBB);
    // RaiseException called from F's body (return address in F).
    GuestContext ctx{};
    t::ThreadContextScope thread_scope(ctx);
    ctx.x[31] = s.f_sp;
    ctx.x[29] = s.f_sp;
    ctx.x[30] = s.f + 0x0c;
    ctx.pc = s.f + 0x0c;
    ctx.x[0] = 0xE06D7363;
    ctx.x[1] = 0;
    ctx.x[2] = 2;
    ctx.x[3] = args;
    guest_RaiseException(ctx);
    FX_CHECK_EQ(code, 0xE06D7363u);
    FX_REQUIRE(params.size() == 2);
    FX_CHECK_EQ(params[0], 0xAAu);
    FX_CHECK_EQ(params[1], 0xBBu);
}

FX_TEST(rtl_unwind_ex_consolidate_runs_callback_with_nonvolatiles) {
    // __CxxFrameHandler3-style: RtlUnwindEx with a STATUS_UNWIND_CONSOLIDATE
    // record; at the target the callback (__CxxCallCatchBlock) runs, reads
    // the target's non-volatile registers through ExceptionInformation[10]
    // and returns the continuation address.
    t::reset_all();
    Scenario s = build(true, false);
    t::register_host_entry(s.handler_f, [&](GuestContext& ctx) {
        ctx.x[0] = disposition_continue_search;
        return GuestResolution::continue_at_caller;
    });
    const std::uint64_t callback = s.image.reserve_bytes(16, 16);
    const std::uint64_t continuation = s.c + 0x18;
    std::uint64_t seen_x19 = 0, seen_fp = 0, seen_record = 0;
    t::register_host_entry(callback, [&](GuestContext& ctx) {
        seen_record = ctx.x[0];
        EXCEPTION_RECORD record{};
        read_guest(ctx.x[0], &record, sizeof(record));
        Arm64NonvolatileRegisters nonvol{};
        read_guest(record.ExceptionInformation[10], &nonvol, sizeof(nonvol));
        seen_x19 = nonvol.GpNvRegs[0];
        seen_fp = nonvol.GpNvRegs[10];
        ctx.x[0] = continuation;
        return GuestResolution::continue_at_caller;
    });
    EXCEPTION_RECORD consolidate{};
    consolidate.ExceptionCode = kStatusUnwindConsolidate;
    consolidate.ExceptionFlags = kFlagNoncontinuable;
    consolidate.NumberParameters = 15;
    consolidate.ExceptionInformation[0] = callback;
    consolidate.ExceptionInformation[10] = static_cast<ULONG_PTR>(-1);
    const std::uint64_t record_address = t::alloc_guest(sizeof(consolidate), 16);
    write_guest(record_address, &consolidate, sizeof(consolidate));

    GuestContext ctx{};
    t::ThreadContextScope thread_scope(ctx);
    ctx.x[31] = s.f_sp;
    ctx.x[29] = s.f_sp;
    ctx.x[30] = s.f + 0x0c;
    ctx.x[0] = s.s0;            // target frame: C
    ctx.x[1] = s.c + 0x0c;      // TargetIp (C's ControlPc)
    ctx.x[2] = record_address;
    ctx.x[3] = 0;
    guest_RtlUnwindEx(ctx);
    FX_CHECK_EQ(seen_record, record_address);
    FX_CHECK_EQ(seen_x19, s.c_x19);   // C's x19 as restored from F's frame
    FX_CHECK_EQ(seen_fp, s.c_sp);     // C's frame pointer
    FX_CHECK_EQ(ctx.pc, continuation);
    FX_CHECK_EQ(ctx.x[31], s.c_sp);
}

FX_TEST(rtl_unwind_longjmp_record_restores_jump_buffer) {
    // vcruntime longjmp: RtlUnwind(jb->Frame, jb->Lr, {STATUS_LONGJUMP, [0]=jb}, value).
    t::reset_all();
    Scenario s = build(true, false);
    int f_unwinds = 0;
    t::register_host_entry(s.handler_f, [&](GuestContext& ctx) {
        if (capture(ctx).flags & kFlagUnwinding) ++f_unwinds;
        ctx.x[0] = disposition_continue_search;
        return GuestResolution::continue_at_caller;
    });
    std::uint64_t buffer[24] = {};
    buffer[0] = s.s0;
    for (unsigned i = 0; i < 10; ++i) buffer[2 + i] = 0x7000 + i;
    buffer[12] = s.c_sp;
    buffer[13] = s.c + 0x1c;
    buffer[14] = s.c_sp;
    const std::uint64_t jb = t::alloc_guest(sizeof(buffer), 16);
    write_guest(jb, buffer, sizeof(buffer));
    EXCEPTION_RECORD longjump{};
    longjump.ExceptionCode = kStatusLongjump;
    longjump.NumberParameters = 1;
    longjump.ExceptionInformation[0] = jb;
    const std::uint64_t record_address = t::alloc_guest(sizeof(longjump), 16);
    write_guest(record_address, &longjump, sizeof(longjump));

    GuestContext ctx{};
    t::ThreadContextScope thread_scope(ctx);
    ctx.x[31] = s.f_sp;
    ctx.x[29] = s.f_sp;
    ctx.x[30] = s.f + 0x0c;
    ctx.x[0] = s.s0;
    ctx.x[1] = s.c + 0x1c;
    ctx.x[2] = record_address;
    ctx.x[3] = 5;
    guest_RtlUnwind(ctx);
    FX_CHECK_EQ(f_unwinds, 1);
    FX_CHECK_EQ(ctx.pc, s.c + 0x1c);
    FX_CHECK_EQ(ctx.x[0], 5u);
    FX_CHECK_EQ(ctx.x[19], 0x7000u);
    FX_CHECK_EQ(ctx.x[28], 0x7009u);
    FX_CHECK_EQ(ctx.x[31], s.c_sp);
}

FX_TEST(rethrow_inside_consolidation_callback_reaches_target_frame) {
    // C++ rethrow shape: exception D1 in F is caught by C through a
    // consolidated unwind; the callback (catch block) raises D3; the walk for
    // D3 must continue from the callback into C (the consolidation target),
    // whose handler catches it; the final transfer must propagate out of
    // every nested call (callback, both handler calls) to the original
    // dispatcher, which resumes the thread in C.
    t::reset_all();
    Scenario s = build(true, true);
    const std::uint64_t callback = s.image.reserve_bytes(16, 16);
    const std::uint64_t first_catch = s.c + 0x10;
    const std::uint64_t second_catch = s.c + 0x18;
    std::vector<std::uint32_t> c_first_pass_codes;
    int f_unwinds = 0, callback_calls = 0;
    std::size_t boundaries_in_callback = 0;

    t::register_host_entry(s.handler_f, [&](GuestContext& ctx) {
        if (capture(ctx).flags & kFlagUnwinding) ++f_unwinds;
        ctx.x[0] = disposition_continue_search;
        return GuestResolution::continue_at_caller;
    });
    const std::uint64_t consolidate_record = t::alloc_guest(sizeof(EXCEPTION_RECORD), 16);
    t::register_host_entry(s.handler_c, [&](GuestContext& ctx) {
        EXCEPTION_RECORD record{};
        read_guest(ctx.x[0], &record, sizeof(record));
        if (record.ExceptionFlags & kFlagUnwinding) {
            ctx.x[0] = disposition_continue_search;
            return GuestResolution::continue_at_caller;
        }
        c_first_pass_codes.push_back(record.ExceptionCode);
        const std::uint64_t establisher = ctx.x[1];
        const std::uint64_t context = ctx.x[2];
        if (record.ExceptionCode == 0xE0000010u) {
            // Catch D1 like __CxxFrameHandler: consolidated unwind to C.
            EXCEPTION_RECORD consolidate{};
            consolidate.ExceptionCode = kStatusUnwindConsolidate;
            consolidate.ExceptionFlags = kFlagNoncontinuable;
            consolidate.NumberParameters = 15;
            consolidate.ExceptionInformation[0] = callback;
            write_guest(consolidate_record, &consolidate, sizeof(consolidate));
            ctx.x[0] = establisher;
            ctx.x[1] = s.c + 0x0c;
            ctx.x[2] = consolidate_record;
            ctx.x[3] = 0;
            ctx.x[4] = context;
        } else {
            // Catch D3 (raised in the catch block): plain unwind to C.
            ctx.x[0] = establisher;
            ctx.x[1] = second_catch;
            ctx.x[2] = 0;
            ctx.x[3] = 0x33;
            ctx.x[4] = context;
        }
        ctx.x[5] = 0;
        return guest_RtlUnwindEx(ctx);
    });
    t::register_host_entry(callback, [&](GuestContext& ctx) {
        ++callback_calls;
        boundaries_in_callback = active_boundary_count();
        // The catch block rethrows: RaiseException from inside the callback.
        ctx.x[0] = 0xE0000020u;
        ctx.x[1] = 0;
        ctx.x[2] = 0;
        ctx.x[3] = 0;
        const GuestResolution resolution = guest_RaiseException(ctx);
        if (resolution == GuestResolution::continue_at_caller || ctx.pc != kReturnToHostPc) {
            ctx.x[0] = first_catch;   // not expected: the rethrow must not return here
        }
        return resolution;
    });

    GuestContext ctx = fault_in_f(s);
    t::ThreadContextScope thread_scope(ctx);
    dispatch_guest_exception(ctx, make_record(0xE0000010u, ctx.pc));

    FX_CHECK_EQ(callback_calls, 1);
    FX_CHECK_EQ(boundaries_in_callback, 2u);           // handler call + consolidation callback
    FX_REQUIRE(c_first_pass_codes.size() == 2);
    FX_CHECK_EQ(c_first_pass_codes[0], 0xE0000010u);   // D1
    FX_CHECK_EQ(c_first_pass_codes[1], 0xE0000020u);   // D3 found C through the callback boundary
    FX_CHECK_EQ(f_unwinds, 1);                         // F unwound once (by D1's unwind)
    FX_CHECK_EQ(ctx.pc, second_catch);
    FX_CHECK_EQ(ctx.x[31], s.c_sp);
    FX_CHECK_EQ(ctx.x[19], s.c_x19);
    FX_CHECK_EQ(ctx.x[0], 0x33u);
    FX_CHECK_EQ(active_boundary_count(), 0u);          // every nested call was left
}

FX_TEST(resetstkoflw_rearms_runtime_guard) {
    GuestContext ctx{};
    ctx.x[31] = 0x7000'0000;
    t::g_rearm_calls = 0;
    t::g_rearm_result = true;
    FX_CHECK(guest__resetstkoflw(ctx) == GuestResolution::continue_at_caller);
    FX_CHECK_EQ(ctx.x[0], 1u);
    FX_CHECK_EQ(t::g_rearm_calls, 1);
    FX_CHECK_EQ(t::g_rearm_sp, 0x7000'0000u);
    t::g_rearm_result = false;   // SP still inside the guard region
    guest__resetstkoflw(ctx);
    FX_CHECK_EQ(ctx.x[0], 0u);
    t::g_rearm_result = true;
}

// ---- personality fault hook (set_guest_fault_hook) ----------------------------

namespace {

struct HookLog {
    int calls = 0;
    int order = 0;               // position in g_hook_sequence
    GuestFaultInfo info{};
    std::uint64_t pc_seen = 0, sp_seen = 0, x19_seen = 0;
    std::uint32_t fpsr_seen = 0, fpcr_seen = 0;
    void* user_seen = nullptr;
    bool handle = true;          // return value
    std::uint64_t resume_pc = 0;
};
int g_hook_sequence = 0;

bool test_fault_hook(GuestContext& ctx, const GuestFaultInfo& fault, void* user) {
    auto* log = static_cast<HookLog*>(user);
    ++log->calls;
    log->order = ++g_hook_sequence;
    log->info = fault;
    log->pc_seen = ctx.pc;
    log->sp_seen = ctx.x[31];
    log->x19_seen = ctx.x[19];
    log->fpsr_seen = ctx.fpsr;
    log->fpcr_seen = ctx.fpcr;
    log->user_seen = user;
    ctx.x[0] = 0x4242;   // discarded when declining
    ctx.x[19] = 0x1919;
    ctx.fpsr = 0;        // feclearexcept(FE_ALL_EXCEPT), fesetround(FE_TOWARDZERO)
    ctx.fpcr = 0x00c00000;
    if (!log->handle) return false;
    ctx.pc = log->resume_pc;
    return true;
}

// A fault in code without .pdata (no guest module): only vectored handlers,
// the guest unhandled filter and the personality hook can take it.
struct NoPdata {
    std::uint64_t code = 0, sp = 0;
};
NoPdata no_pdata_code() {
    NoPdata n;
    n.code = t::emit_code({t::a64::nop(), t::a64::nop(), t::a64::nop(), t::a64::ret()});
    n.sp = t::alloc_guest(0x2000, 0x1000) + 0x1800;
    return n;
}
GuestContext fault_at(const NoPdata& n) {
    GuestContext ctx{};
    ctx.pc = n.code + 4;
    ctx.x[31] = n.sp;
    ctx.x[30] = 0;   // leaf at the outermost frame: the walk ends
    ctx.x[0] = 0x0A0A;
    ctx.x[19] = 0xF19;
    ctx.fpsr = 0x13;   // IXC DZC IOC raised before the fault
    return ctx;
}

GuestFault access_fault(std::uint64_t pc, std::uint64_t address, std::uint32_t type) {
    GuestFault fault{};
    fault.kind = GuestFault::Kind::access_violation;
    fault.faulting_pc = pc;
    fault.access_address = address;
    fault.access_type = type;
    return fault;
}

struct HookScope {
    HookScope(GuestFaultHook hook, void* user) { set_guest_fault_hook(hook, user); }
    ~HookScope() { set_guest_fault_hook(nullptr, nullptr); }
};

} // namespace

FX_TEST(fault_hook_resumes_fault_without_pdata) {
    t::reset_all();
    const NoPdata n = no_pdata_code();
    HookLog log;
    log.resume_pc = n.code + 8;
    HookScope hook(&test_fault_hook, &log);
    GuestContext ctx = fault_at(n);
    t::ThreadContextScope thread_scope(ctx);
    dispatch_guest_fault(ctx, access_fault(n.code + 4, 0xDEAD'0010, 1));
    FX_CHECK_EQ(log.calls, 1);
    FX_CHECK(log.user_seen == &log);
    FX_CHECK_EQ(log.info.code, kStatusAccessViolation);
    FX_CHECK_EQ(log.info.flags, 0u);
    FX_CHECK_EQ(log.info.address, 0xDEAD'0010u);
    FX_CHECK_EQ(log.info.pc, n.code + 4);
    FX_CHECK(log.info.write);
    FX_CHECK(!log.info.execute);
    FX_CHECK_EQ(log.info.parameter_count, 2u);
    FX_CHECK_EQ(log.info.parameters[0], 1u);
    FX_CHECK_EQ(log.info.parameters[1], 0xDEAD'0010u);
    // The hook saw the fault state and its changes were applied.
    FX_CHECK_EQ(log.pc_seen, n.code + 4);
    FX_CHECK_EQ(log.sp_seen, n.sp);
    FX_CHECK_EQ(log.x19_seen, 0xF19u);
    FX_CHECK_EQ(log.fpsr_seen, 0x13u);
    FX_CHECK_EQ(log.fpcr_seen, 0u);
    FX_CHECK_EQ(ctx.pc, n.code + 8);
    FX_CHECK_EQ(ctx.x[0], 0x4242u);
    FX_CHECK_EQ(ctx.x[19], 0x1919u);
    FX_CHECK_EQ(ctx.fpsr, 0u);          // the hook's FP changes are what the guest resumes with
    FX_CHECK_EQ(ctx.fpcr, 0x00c00000u);
    FX_CHECK_EQ(ctx.x[31], n.sp);
    FX_CHECK_EQ(active_boundary_count(), 0u);

    // Execute faults and non-access exceptions.
    log = HookLog{};
    log.resume_pc = n.code;
    GuestContext exec = fault_at(n);
    t::ThreadContextScope exec_scope(exec);
    dispatch_guest_fault(exec, access_fault(n.code + 4, n.code + 4, 8));
    FX_CHECK(log.info.execute);
    FX_CHECK(!log.info.write);
    GuestFault illegal{};
    illegal.kind = GuestFault::Kind::illegal_instruction;
    illegal.faulting_pc = n.code + 4;
    GuestContext ill = fault_at(n);
    t::ThreadContextScope ill_scope(ill);
    dispatch_guest_fault(ill, illegal);
    FX_CHECK_EQ(log.calls, 2);
    FX_CHECK_EQ(log.info.code, kStatusIllegalInstruction);
    FX_CHECK_EQ(log.info.address, 0u);
    FX_CHECK(!log.info.write && !log.info.execute);
    FX_CHECK_EQ(ill.pc, n.code);
}

FX_TEST(fault_hook_declining_terminates_and_discards_changes) {
    t::reset_all();
    const NoPdata n = no_pdata_code();
    HookLog log;
    log.handle = false;
    HookScope hook(&test_fault_hook, &log);
    auto previous = set_termination_hook(&TerminationCapture::hook);
    GuestContext ctx = fault_at(n);
    t::ThreadContextScope thread_scope(ctx);
    bool terminated = false;
    try {
        dispatch_guest_fault(ctx, access_fault(n.code + 4, 0x10, 0));
    } catch (const std::runtime_error&) {
        terminated = true;
    }
    set_termination_hook(previous);
    FX_CHECK(terminated);
    FX_CHECK_EQ(log.calls, 1);
    FX_CHECK(!log.info.write);
    FX_CHECK_EQ(TerminationCapture::code, kStatusAccessViolation);
    FX_CHECK_EQ(ctx.x[0], 0x0A0Au);
    FX_CHECK_EQ(ctx.x[19], 0xF19u);
    FX_CHECK_EQ(ctx.fpsr, 0x13u);
    FX_CHECK_EQ(ctx.fpcr, 0u);
}

FX_TEST(fault_hook_not_called_when_guest_handlers_take_the_exception) {
    t::reset_all();
    HookLog log;
    log.resume_pc = 0xBAD0;
    HookScope hook(&test_fault_hook, &log);
    {   // frame-based handler (.pdata) continues execution
        Scenario s = build(true, false);
        t::register_host_entry(s.handler_f, [&](GuestContext& ctx) {
            Arm64Context original{};
            read_guest(ctx.x[2], &original, sizeof(original));
            original.Pc += 4;
            write_guest(ctx.x[2], &original, sizeof(original));
            ctx.x[0] = disposition_continue_execution;
            return GuestResolution::continue_at_caller;
        });
        GuestContext ctx = fault_in_f(s);
        t::ThreadContextScope thread_scope(ctx);
        dispatch_guest_exception(ctx, make_record(kStatusAccessViolation, ctx.pc));
        FX_CHECK_EQ(ctx.pc, s.f + 0x10);
    }
    {   // vectored handler continues execution
        const NoPdata n = no_pdata_code();
        const std::uint64_t veh = t::emit_code({t::a64::ret()});
        t::register_host_entry(veh, [&](GuestContext& ctx) {
            ctx.x[0] = 0xFFFFFFFFu;   // EXCEPTION_CONTINUE_EXECUTION
            return GuestResolution::continue_at_caller;
        });
        GuestContext add{};
        add.x[0] = 1;
        add.x[1] = veh;
        guest_AddVectoredExceptionHandler(add);
        GuestContext ctx = fault_at(n);
        t::ThreadContextScope thread_scope(ctx);
        dispatch_guest_fault(ctx, access_fault(n.code + 4, 0x20, 1));
        FX_CHECK_EQ(ctx.pc, n.code + 4);
        GuestContext remove{};
        remove.x[0] = add.x[0];
        guest_RemoveVectoredExceptionHandler(remove);
    }
    FX_CHECK_EQ(log.calls, 0);
}

FX_TEST(fault_hook_runs_after_guest_unhandled_filter) {
    t::reset_all();
    const NoPdata n = no_pdata_code();
    HookLog log;
    log.resume_pc = n.code + 12;
    HookScope hook(&test_fault_hook, &log);
    const std::uint64_t filter = t::emit_code({t::a64::ret()});
    int filter_calls = 0, filter_order = 0;
    bool filter_continues = false;
    t::register_host_entry(filter, [&](GuestContext& ctx) {
        ++filter_calls;
        filter_order = ++g_hook_sequence;
        std::uint64_t pointers[2];
        read_guest(ctx.x[0], pointers, sizeof(pointers));
        Arm64Context context{};
        read_guest(pointers[1], &context, sizeof(context));
        context.Pc = 0xABCD'0000;
        write_guest(pointers[1], &context, sizeof(context));
        ctx.x[0] = filter_continues ? 0xFFFFFFFFu : 0u;   // CONTINUE_EXECUTION / CONTINUE_SEARCH
        return GuestResolution::continue_at_caller;
    });
    // The internal API used before the hook existed keeps working.
    FX_CHECK_EQ(set_unhandled_exception_filter(filter), 0u);

    GuestContext ctx = fault_at(n);
    t::ThreadContextScope thread_scope(ctx);
    dispatch_guest_exception(ctx, make_record(0xE0000042u, ctx.pc));
    FX_CHECK_EQ(filter_calls, 1);
    FX_CHECK_EQ(log.calls, 1);
    FX_CHECK(filter_order < log.order);
    FX_CHECK_EQ(log.pc_seen, n.code + 4);   // the declining filter's edit is not seen
    FX_CHECK_EQ(log.info.code, 0xE0000042u);
    FX_CHECK_EQ(log.info.parameter_count, 1u);
    FX_CHECK_EQ(log.info.parameters[0], 0x5151u);
    FX_CHECK_EQ(log.info.address, 0u);
    FX_CHECK_EQ(ctx.pc, n.code + 12);

    filter_continues = true;
    GuestContext again = fault_at(n);
    t::ThreadContextScope again_scope(again);
    dispatch_guest_exception(again, make_record(0xE0000043u, again.pc));
    FX_CHECK_EQ(filter_calls, 2);
    FX_CHECK_EQ(log.calls, 1);   // the guest filter handled it
    FX_CHECK_EQ(again.pc, 0xABCD'0000u);
    FX_CHECK_EQ(set_unhandled_exception_filter(0), filter);
}

FX_TEST(fault_hook_replace_and_remove) {
    t::reset_all();
    const NoPdata n = no_pdata_code();
    HookLog first, second;
    first.resume_pc = second.resume_pc = n.code + 8;
    set_guest_fault_hook(&test_fault_hook, &first);
    set_guest_fault_hook(&test_fault_hook, &second);   // replaces
    {
        GuestContext ctx = fault_at(n);
        t::ThreadContextScope thread_scope(ctx);
        dispatch_guest_fault(ctx, access_fault(n.code + 4, 0x30, 1));
        FX_CHECK_EQ(ctx.pc, n.code + 8);
    }
    FX_CHECK_EQ(first.calls, 0);
    FX_CHECK_EQ(second.calls, 1);
    set_guest_fault_hook(nullptr, &second);            // removes
    auto previous = set_termination_hook(&TerminationCapture::hook);
    bool terminated = false;
    try {
        GuestContext ctx = fault_at(n);
        t::ThreadContextScope thread_scope(ctx);
        dispatch_guest_fault(ctx, access_fault(n.code + 4, 0x30, 1));
    } catch (const std::runtime_error&) {
        terminated = true;
    }
    set_termination_hook(previous);
    FX_CHECK(terminated);
    FX_CHECK_EQ(second.calls, 1);
}
