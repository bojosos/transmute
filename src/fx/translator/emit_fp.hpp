#pragma once

// Floating-point family (owner: fp).
//
// Lowers A64 scalar and vector floating point (S/D, and H when the host has
// F16C) to inline SSE4.1 / AVX / FMA3 sequences, plus MRS/MSR of FPCR and
// FPSR. See NOTES_FP.md for semantics decisions.
//
// FP state convention while guest code runs (docs/FX_ENGINE.md):
//  * MXCSR = fp_mxcsr_for_state(ctx.fpcr, ctx.fpsr): all exceptions masked,
//    FPCR.FZ -> FTZ|DAZ, FPCR.RMode -> MXCSR.RC. Only MSR FPCR (and thread
//    entry) rewrites the control bits.
//  * The architectural FPSR is ctx.fpsr | fp_fpsr_flags_from_mxcsr(MXCSR):
//    the cumulative exception flags accumulate lazily in the MXCSR status
//    bits. MSR FPSR writes both ctx.fpsr and the MXCSR status bits.
//  * Before C++ code inspects or changes FP state (interpreter fallback,
//    exception dispatch, GetThreadContext), call fp_sync_fpsr(ctx); before
//    resuming translated code after changing ctx.fpcr/ctx.fpsr, call
//    fp_load_mxcsr(ctx).

#include "fx/translator/emit_context.hpp"
#include "transmute/fx/context.hpp"

#include <cstdint>
#include <span>

namespace transmute::fx {

// ---- family entry points ------------------------------------------------------
// FamilyEmitFn / FamilyDescribeFn for the fp family.
unsigned fp_emit(EmitContext& ctx, std::span<const std::uint32_t> words);
bool fp_describe(std::uint32_t word, InstructionUse& use);
// {"fp", fp_emit, fp_describe}. register_fp_emitters() (translator.hpp)
// registers this through register_family_emitter().
const FamilyEmitter& fp_family();

// ---- FPCR / FPSR / MXCSR ----------------------------------------------------------
// FPSR cumulative exception bits: IOC(0) DZC(1) OFC(2) UFC(3) IXC(4) IDC(7).
inline constexpr std::uint32_t kFpsrCumulativeMask = 0x9fu;
// The subset mirrored in MXCSR status bits (IDC has no x64 equivalent).
inline constexpr std::uint32_t kFpsrMxcsrMask = 0x1fu;
inline constexpr std::uint32_t kMxcsrStatusMask = 0x3fu;

// MXCSR control word for FPCR (status bits clear): exceptions masked,
// RMode 00 RN -> RC 00, 01 RP -> RC 10, 10 RM -> RC 01, 11 RZ -> RC 11,
// FZ -> FTZ | DAZ. Used by the runtime at thread entry.
std::uint32_t fp_mxcsr_for_fpcr(std::uint32_t fpcr);
// fp_mxcsr_for_fpcr(fpcr) plus the status bits mirroring FPSR's cumulative
// flags (IOC->IE, DZC->ZE, OFC->OE, UFC->UE, IXC->PE).
std::uint32_t fp_mxcsr_for_state(std::uint32_t fpcr, std::uint32_t fpsr);
// FPSR cumulative bits encoded by MXCSR status bits (IE->IOC, ZE->DZC,
// OE->OFC, UE->UFC, PE->IXC). DE is ignored: x64 sets it only when DAZ is
// clear, i.e. exactly when A64 never sets IDC.
std::uint32_t fp_fpsr_flags_from_mxcsr(std::uint32_t mxcsr);
// MXCSR status bits for FPSR's cumulative flags.
std::uint32_t fp_mxcsr_flags_from_fpsr(std::uint32_t fpsr);
// Architectural FPSR (what MRS FPSR returns) given ctx.fpsr and a live MXCSR.
std::uint32_t fp_read_fpsr(std::uint32_t context_fpsr, std::uint32_t mxcsr);
// Architectural FPSR after writing `value` (MSR FPSR): the new ctx.fpsr and
// the new MXCSR (control bits of `mxcsr` kept, status bits from `value`).
void fp_write_fpsr(std::uint32_t value, std::uint32_t mxcsr, std::uint32_t& context_fpsr,
                   std::uint32_t& new_mxcsr);

// Live-thread helpers (operate on the calling thread's MXCSR).
// ctx.fpsr |= flags accumulated in MXCSR; returns the architectural FPSR.
std::uint32_t fp_sync_fpsr(GuestContext& ctx) noexcept;
// MXCSR = fp_mxcsr_for_state(ctx.fpcr, ctx.fpsr).
void fp_load_mxcsr(const GuestContext& ctx) noexcept;

// ---- host feature selection (tests) ---------------------------------------------
struct FpFeatures {
    bool avx = false;    // VEX encodings (3-operand forms)
    bool fma = false;    // FMA3; without it fused operations are declined
    bool f16c = false;   // half precision; without it H forms are declined
    bool avx2 = false;
};
// Features the emitters use (host features unless overridden).
FpFeatures fp_features();
FpFeatures fp_host_features();
// Overrides the features used by subsequent emits (nullptr = host). Tests
// use this to exercise the SSE4.1-only paths on an AVX host. Not thread-safe.
void fp_set_features_for_testing(const FpFeatures* features);
// Overrides the TRANSMUTE_FX_FP code-generation knobs (bit set, see
// emit_fp_internal.hpp; -1 = the environment). Tests use this to check old
// and new lowerings in one process. Not thread-safe.
void fp_set_knobs_for_testing(int bits);

} // namespace transmute::fx
