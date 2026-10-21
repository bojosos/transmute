#pragma once

#include <cstdint>
#include <vector>

namespace transmute::jit_detail {

// Exact AdvSIMD integer operations and architectural FP/SIMD bit transfers.
[[nodiscard]] bool accepts_integer_simd(std::uint32_t instruction) noexcept;

// Inline Windows x64 fragment. RCX points to CpuState; only volatile XMM0..3
// are clobbered. No calls, host GPR changes, stack access, return or PC update.
// Architectural sources are snapshotted before destination writes. Narrow V
// results clear their upper bits. Unsupported/reserved encodings throw.
[[nodiscard]] std::vector<std::uint8_t> emit_integer_simd(
    std::uint32_t instruction);

} // namespace transmute::jit_detail
