// Conversions between GuestContext and ARM64_NT_CONTEXT.

#include "fx/seh/seh.hpp"

#include <cstring>

namespace transmute::fx::seh {
namespace {

// GuestContext.nzcv holds the ARM flags in bits 31..28 (N,Z,C,V). Cpsr in the
// ARM64 context record uses the same bit positions, so the NZCV field maps
// directly; the remaining Cpsr bits (DAIF, EL, SPSel) are not modelled and
// stay zero.
constexpr std::uint32_t kNzcvMask = 0xf0000000u;

} // namespace

// guest.fpsr/fpcr are architectural at every caller: exception dispatch runs
// after a dispatcher exit, and RtlCaptureContext/longjmp/RaiseException arrive
// through abi override thunks (or host callbacks), which fold the MXCSR flags
// into ctx.fpsr first (src/fx/runtime/fp_boundary.hpp). Contexts written back
// (RtlRestoreContext, longjmp, a resuming handler or fault hook) reach MXCSR
// at the thunk's or callback's return, or at the next region entry.
void guest_to_arm64_context(const GuestContext& guest, Arm64Context& out, DWORD flags) {
    std::memset(&out, 0, sizeof(out));
    out.ContextFlags = flags;
    out.Cpsr = static_cast<std::uint32_t>(nzcv_value(guest)) & kNzcvMask;   // also a region exit's raw form
    for (unsigned i = 0; i < 31; ++i) out.X[i] = guest.x[i];
    out.Sp = guest.x[31];
    out.Pc = guest.pc;
    for (unsigned i = 0; i < 32; ++i) {
        out.V[i].Low = guest.v[i][0];
        out.V[i].High = static_cast<LONGLONG>(guest.v[i][1]);
    }
    out.Fpcr = guest.fpcr;
    out.Fpsr = guest.fpsr;
}

void arm64_to_guest_context(const Arm64Context& in, GuestContext& guest) {
    // The section discriminators are the low bits (CONTROL=1, INTEGER=2,
    // FLOATING_POINT=4); the 0x400000 ARM64 marker is shared, so it must not be
    // treated as a section selector.
    const bool is_arm64 = (in.ContextFlags & kCtxArm64) != 0;
    const DWORD low = in.ContextFlags & 0x7u;
    const bool all = !is_arm64 || low == 0;
    if (all || (low & 0x2u)) {
        for (unsigned i = 0; i < 29; ++i) guest.x[i] = in.X[i];
    }
    if (all || (low & 0x1u)) {
        guest.x[29] = in.Fp;
        guest.x[30] = in.Lr;
        guest.x[31] = in.Sp;
        guest.pc = in.Pc;
        guest.nzcv = in.Cpsr & kNzcvMask;
    }
    if (all || (low & 0x4u)) {
        for (unsigned i = 0; i < 32; ++i) {
            guest.v[i][0] = in.V[i].Low;
            guest.v[i][1] = static_cast<std::uint64_t>(in.V[i].High);
        }
        guest.fpcr = in.Fpcr;
        guest.fpsr = in.Fpsr;
    }
}

void fill_nonvolatile_registers(const Arm64Context& context, Arm64NonvolatileRegisters& out) {
    // GpNvRegs[0..10] = x19..x28, Fp(x29); FpNvRegs[0..7] = d8..d15.
    for (unsigned i = 0; i < 10; ++i) out.GpNvRegs[i] = context.X[19 + i];
    out.GpNvRegs[10] = context.Fp;
    for (unsigned i = 0; i < 8; ++i) out.FpNvRegs[i] = context.V[8 + i].D[0];
}

} // namespace transmute::fx::seh
