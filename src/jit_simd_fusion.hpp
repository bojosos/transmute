#pragma once

#include <cstdint>
#include <vector>

namespace transmute::jit_detail {

[[nodiscard]] bool native_avx2_available() noexcept;
[[nodiscard]] bool accepts_fused_integer_simd(
    std::uint32_t first, std::uint32_t second, bool avx2_available) noexcept;
[[nodiscard]] bool accepts_fused_integer_simd(
    std::uint32_t first, std::uint32_t second) noexcept;

// Combines two independent Q=1 operations over adjacent vector registers.
// RCX is CpuState; RCX/R11, GPRs, flags and MXCSR are preserved. YMM0..2
// are volatile scratch. No guest PC or completed-count changes are emitted.
// The caller must enforce a two-instruction budget and retain a separate
// scalar entry for the second instruction. VZEROUPPER finishes the fragment.
[[nodiscard]] std::vector<std::uint8_t> emit_fused_integer_simd(
    std::uint32_t first, std::uint32_t second);

} // namespace transmute::jit_detail
