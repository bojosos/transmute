#pragma once

#include <cstdint>
#include <vector>

namespace transmute::jit_detail {

// DUP, modified immediates, scalar S/D FMOV/FABS/FNEG/FCSEL and GP transfers.
// These manipulate bits and do not observe or change the host FP environment.
[[nodiscard]] bool accepts_bit_moves(std::uint32_t instruction) noexcept;

// RCX=CpuState*. Only volatile XMM0..3 are scratch; all GPRs, stack, guest
// flags, FPCR/FPSR and MXCSR are preserved. PC/accounting belong to the caller.
[[nodiscard]] std::vector<std::uint8_t> emit_bit_moves(std::uint32_t instruction);

} // namespace transmute::jit_detail
