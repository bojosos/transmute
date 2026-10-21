#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace transmute::jit_detail {

enum class NativeFpPolicy : std::uint8_t { native_nan_representation };
// Finite arithmetic retains guest rounding and sticky exception semantics.
// NaN sign, payload and DN representation use the host's direct FP result.
inline constexpr auto native_fp_policy = NativeFpPolicy::native_nan_representation;

// Embedded in the region's private invocation context, never in CpuState.
struct NativeFpScratch {
    std::uint32_t saved_mxcsr{};
    std::uint32_t work_mxcsr{};
    std::array<std::uint64_t, 2> result{};
    std::uint32_t committed_mxcsr{};
    std::uint32_t session_active{};
};

enum class NativeFpEmissionMode : std::uint8_t { isolated, region_session };

struct GuardedFpFragment {
    std::vector<std::uint8_t> bytes;
    // Offsets of disp32 fields in conditional branches. The region emitter
    // patches these to an exit reporting this instruction's PC, without
    // advancing its completed count. Earlier instructions have committed.
    std::vector<std::size_t> bailout_displacements;
};

// Fused forms require CPU FMA/AVX, OSXSAVE and OS-enabled SSE/YMM state.
[[nodiscard]] bool native_fma_available() noexcept;
// Explicit feature input keeps instruction eligibility portable and testable.
[[nodiscard]] bool accepts_guarded_fp(std::uint32_t instruction,
                                      bool fma_available) noexcept;
// Includes scalar/vector S/D indexed FMUL/FMLA/FMLS with every valid source
// element and GP SCVTF/UCVTF W/X->S/D, unscaled or with valid fixed-point
// scales. Uses host FMA availability; guards reject difficult FP arithmetic.
[[nodiscard]] bool accepts_guarded_fp(std::uint32_t instruction) noexcept;

// RCX=CpuState*, R11=region context. scratch_offset locates NativeFpScratch
// inside that context. Clobbers volatile RAX/RDX/R8, XMM0..3 and host flags.
// FMA fragments also clear the volatile upper halves of YMM registers.
// No stack, helpers, return or PC/accounting changes. Isolated fragments restore
// host MXCSR on every path and merge sticky FPSR on success. Session fragments
// retain guest MXCSR between instructions and defer FPSR to the region exit.
// A failed instruction never commits Rd or its exception flags. Every session
// exit must append emit_fp_session_finish before returning to host code.
[[nodiscard]] GuardedFpFragment emit_guarded_fp(
    std::uint32_t instruction, std::uint32_t scratch_offset,
    NativeFpEmissionMode mode = NativeFpEmissionMode::isolated);

// Merge only flags from successful session instructions, restore exact host
// MXCSR and mark the session inactive. Safe when no FP instruction executed.
// Same RCX/R11 ABI, clobbers volatile RAX/RDX and host flags only.
[[nodiscard]] std::vector<std::uint8_t> emit_fp_session_finish(
    std::uint32_t scratch_offset);

} // namespace transmute::jit_detail
