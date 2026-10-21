// GuestContext <-> ARM64_NT_CONTEXT conversions and fault translation.

#include "fx/seh/seh.hpp"
#include "fx/seh/tests/seh_test_util.hpp"
#include "fx/testing/test.hpp"

using namespace transmute::fx;
using namespace transmute::fx::seh;
namespace t = fxseh_test;

FX_TEST(context_roundtrip) {
    GuestContext guest{};
    for (unsigned i = 0; i < 31; ++i) guest.x[i] = 0x1000 + i;
    guest.x[31] = 0xDEAD'0000;  // SP
    guest.pc = 0xCAFE'1000;
    guest.nzcv = 0xA0000000;  // N and C set
    guest.fpcr = 0x00C00000;
    guest.fpsr = 0x10000000;
    for (unsigned i = 0; i < 32; ++i) {
        guest.v[i][0] = 0x2000 + i;
        guest.v[i][1] = 0x3000 + i;
    }

    Arm64Context ctx;
    guest_to_arm64_context(guest, ctx, kCtxFull);
    FX_CHECK_EQ(ctx.Sp, guest.x[31]);
    FX_CHECK_EQ(ctx.Pc, guest.pc);
    FX_CHECK_EQ(ctx.Fp, guest.x[29]);
    FX_CHECK_EQ(ctx.Lr, guest.x[30]);
    FX_CHECK_EQ(ctx.Cpsr, 0xA0000000u);
    FX_CHECK_EQ(ctx.Fpcr, guest.fpcr);
    FX_CHECK_EQ(ctx.V[5].Low, guest.v[5][0]);
    FX_CHECK_EQ(static_cast<std::uint64_t>(ctx.V[5].High), guest.v[5][1]);

    GuestContext back{};
    arm64_to_guest_context(ctx, back);
    for (unsigned i = 0; i < 32; ++i) FX_CHECK_EQ(back.x[i], guest.x[i]);
    FX_CHECK_EQ(back.pc, guest.pc);
    FX_CHECK_EQ(back.nzcv, 0xA0000000u);
    FX_CHECK_EQ(back.fpcr, guest.fpcr);
    FX_CHECK_EQ(back.fpsr, guest.fpsr);
    for (unsigned i = 0; i < 32; ++i) {
        FX_CHECK_EQ(back.v[i][0], guest.v[i][0]);
        FX_CHECK_EQ(back.v[i][1], guest.v[i][1]);
    }
}

FX_TEST(context_partial_sections) {
    GuestContext guest{};
    guest.x[0] = 0x11; guest.x[29] = 0x29; guest.x[30] = 0x30; guest.x[31] = 0x31; guest.pc = 0xABC;
    Arm64Context ctx{};
    ctx.ContextFlags = kCtxArm64 | 0x1;  // CONTROL only
    ctx.Fp = 0xF0; ctx.Lr = 0xF1; ctx.Sp = 0xF2; ctx.Pc = 0xF3; ctx.Cpsr = 0x80000000;
    ctx.X[0] = 0x999;  // INTEGER not selected; must be ignored
    arm64_to_guest_context(ctx, guest);
    FX_CHECK_EQ(guest.x[0], 0x11u);   // unchanged (INTEGER not set)
    FX_CHECK_EQ(guest.x[29], 0xF0u);  // control fields applied
    FX_CHECK_EQ(guest.x[30], 0xF1u);
    FX_CHECK_EQ(guest.x[31], 0xF2u);
    FX_CHECK_EQ(guest.pc, 0xF3u);
    FX_CHECK_EQ(guest.nzcv, 0x80000000u);
}

FX_TEST(nonvolatile_registers_filled) {
    Arm64Context ctx{};
    for (unsigned i = 19; i <= 28; ++i) ctx.X[i] = 0x100 + i;
    ctx.Fp = 0xF9;
    for (unsigned i = 8; i <= 15; ++i) ctx.V[i].D[0] = static_cast<double>(i);
    Arm64NonvolatileRegisters nv{};
    fill_nonvolatile_registers(ctx, nv);
    for (unsigned i = 0; i < 10; ++i) FX_CHECK_EQ(nv.GpNvRegs[i], 0x100u + 19 + i);
    FX_CHECK_EQ(nv.GpNvRegs[10], 0xF9u);
    FX_CHECK(nv.FpNvRegs[0] == 8.0);
    FX_CHECK(nv.FpNvRegs[7] == 15.0);
}

FX_TEST(fault_access_violation_record) {
    t::reset_all();
    GuestContext guest{};
    guest.pc = 0x4000;
    // No handlers registered and no unhandled filter -> dispatch terminates.
    // Install a termination hook that throws so we can inspect the report.
    static std::string captured;
    static std::uint32_t captured_code = 0;
    auto previous = set_termination_hook([](std::uint32_t code, const std::string& report) {
        captured = report;
        captured_code = code;
        throw std::runtime_error("terminated");
    });
    GuestFault fault{};
    fault.kind = GuestFault::Kind::access_violation;
    fault.faulting_pc = 0x4000;
    fault.access_address = 0xDEAD;
    fault.access_type = 1;  // write
    bool threw = false;
    try {
        dispatch_guest_fault(guest, fault);
    } catch (const std::exception&) {
        threw = true;
    }
    set_termination_hook(previous);
    FX_CHECK(threw);
    FX_CHECK_EQ(captured_code, 0xC0000005u);
    FX_CHECK(captured.find("write") != std::string::npos);
}
