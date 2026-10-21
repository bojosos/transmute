#pragma once

// FP state handoff at host-code boundaries (owner: runtime; docs/FX_ENGINE.md, FP).
//
// While translated code runs, the cumulative FPSR flags IOC..IXC accumulate
// in the MXCSR status bits (architectural FPSR = ctx.fpsr | flags(MXCSR),
// emit_fp.hpp) and the MXCSR control bits mirror ctx.fpcr. Host code that
// receives the GuestContext straight from translated code, without a
// dispatcher exit (register_host_callback callbacks, abi override thunks), is
// bracketed by these two sequences so that it sees ctx.fpsr and ctx.fpcr as
// the architectural registers:
//
//   to_context    ctx.fpsr |= flags(MXCSR). MXCSR is left alone (ldmxcsr is
//                 expensive, and after an inexact operation it would be needed
//                 on nearly every call): from here on ctx.fpsr is
//                 authoritative and MXCSR's flags are stale copies.
//   from_context  MXCSR = fp_mxcsr_for_state(ctx.fpcr, ctx.fpsr) -- rounding,
//                 FZ -> FTZ|DAZ, exceptions masked, the flags -- and the
//                 mirrored bits leave ctx.fpsr again (as at a region entry).
//                 Flags the host's own arithmetic raised in MXCSR are dropped;
//                 DE (no FPSR bit) is left as it is. ldmxcsr only when the
//                 value changes, which it does not in the common case (the
//                 host changed neither ctx.fpsr's flags nor ctx.fpcr).
//
// With `thread_marker` (runtime only: the abi's override thunks run without
// ThreadState access), ThreadState::fp_host is 1 between the two sequences,
// so that leaving the host code another way (exit_to_dispatcher, a C++
// exception unwinding to invoke) does not fold the stale MXCSR flags back
// into ctx.fpsr.
//
// Header-only (x64 assembler, GuestContext offsets): fx_abi emits it without
// linking fx_runtime. It must stay in step with emit_fp.cpp
// (fp_mxcsr_for_state, fp_read_fpsr) and emit_fp_arith.cpp (MSR FPCR).
// The C++ equivalents are fp_state_to_context / fp_state_from_context
// (process.hpp).

#include "fx/common/x64_assembler.hpp"
#include "transmute/fx/context.hpp"

#include <cstddef>

namespace transmute::fx::rt {

// Offset of ThreadState::fp_host for the emitters (-1: none).
inline constexpr std::int32_t kNoThreadMarker = -1;

inline void emit_fp_thread_marker(x64::Assembler& a, std::int32_t offset, std::int32_t value) {
    using x64::Gp;
    const x64::Label none = a.new_label();
    a.mov(64, Gp::rax, x64::ptr(Gp::r15, ctx_disp(offsetof(GuestContext, thread))));
    a.test(64, Gp::rax, Gp::rax);
    a.jcc_short(x64::Cond::e, none);
    a.mov(32, x64::ptr(Gp::rax, offset), value);
    a.bind(none);
}

// r15 = ctx + kContextBias. `scratch` is a 4-byte slot the code may use (for
// example the callee shadow space before a call). Clobbers rax, rcx, rdx and
// the host flags.
inline void emit_fp_state_to_context(x64::Assembler& a, const x64::Mem& scratch,
                                     std::int32_t thread_marker = kNoThreadMarker) {
    using x64::Gp;
    const x64::Mem fpsr = x64::ptr(Gp::r15, ctx_disp(offsetof(GuestContext, fpsr)));
    const x64::Label clean = a.new_label();
    a.stmxcsr(scratch);
    a.mov(32, Gp::rax, scratch);
    a.test(8, Gp::rax, 0x3d);
    a.jcc_short(x64::Cond::e, clean);
    a.mov(32, Gp::rcx, Gp::rax);
    a.shr(32, Gp::rcx, 1);
    a.and_(32, Gp::rcx, 0x1e);           // ZE OE UE PE -> DZC OFC UFC IXC
    a.and_(32, Gp::rax, 1);              // IE -> IOC (DE has no FPSR bit)
    a.or_(32, Gp::rcx, Gp::rax);
    a.alu(x64::Alu::or_, 32, fpsr, Gp::rcx);
    a.bind(clean);
    if (thread_marker != kNoThreadMarker) emit_fp_thread_marker(a, thread_marker, 1);
}

// Same registers and scratch rules as emit_fp_state_to_context.
inline void emit_fp_state_from_context(x64::Assembler& a, const x64::Mem& scratch,
                                       std::int32_t thread_marker = kNoThreadMarker) {
    using x64::Gp;
    const x64::Mem fpcr = x64::ptr(Gp::r15, ctx_disp(offsetof(GuestContext, fpcr)));
    const x64::Mem fpsr = x64::ptr(Gp::r15, ctx_disp(offsetof(GuestContext, fpsr)));
    if (thread_marker != kNoThreadMarker) emit_fp_thread_marker(a, thread_marker, 0);
    // Control: RMode (23:22) -> RC (14:13) with the two bits swapped,
    // FZ (24) -> FTZ | DAZ, all exceptions masked (fp_mxcsr_for_fpcr).
    a.mov(32, Gp::rax, fpcr);
    a.mov(32, Gp::rcx, Gp::rax);
    a.shr(32, Gp::rcx, 22);
    a.and_(32, Gp::rcx, 3);
    a.mov(32, Gp::rdx, Gp::rcx);
    a.shr(32, Gp::rdx, 1);
    a.xor_(32, Gp::rdx, Gp::rcx);
    a.and_(32, Gp::rdx, 1);                                  // b0 ^ b1
    a.lea(32, Gp::rdx, x64::ptr(Gp::rdx, Gp::rdx, 2));       // * 3
    a.xor_(32, Gp::rcx, Gp::rdx);                            // swapped
    a.shl(32, Gp::rcx, 13);
    a.shr(32, Gp::rax, 24);
    a.and_(32, Gp::rax, 1);
    a.imul(32, Gp::rax, Gp::rax, 0x8040);
    a.or_(32, Gp::rcx, Gp::rax);
    a.or_(32, Gp::rcx, 0x1f80);
    // Status: IOC -> IE, DZC OFC UFC IXC -> ZE OE UE PE; the mirrored bits
    // leave ctx.fpsr (IDC, QC and the rest stay).
    a.mov(32, Gp::rax, fpsr);
    a.mov(32, Gp::rdx, Gp::rax);
    a.and_(32, Gp::rdx, 0x1e);
    a.shl(32, Gp::rdx, 1);
    a.or_(32, Gp::rcx, Gp::rdx);
    a.mov(32, Gp::rdx, Gp::rax);
    a.and_(32, Gp::rdx, 1);
    a.or_(32, Gp::rcx, Gp::rdx);
    a.and_(32, Gp::rax, ~0x1f);
    a.mov(32, fpsr, Gp::rax);
    // DE stays as it is (no FPSR bit; keeping it saves an ldmxcsr).
    a.stmxcsr(scratch);
    a.mov(32, Gp::rax, scratch);
    a.and_(32, Gp::rax, 0x02);
    a.or_(32, Gp::rcx, Gp::rax);
    const x64::Label same = a.new_label();
    a.cmp(32, Gp::rcx, scratch);
    a.jcc_short(x64::Cond::e, same);
    a.mov(32, scratch, Gp::rcx);
    a.ldmxcsr(scratch);
    a.bind(same);
}

} // namespace transmute::fx::rt
