#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace transmute::jit_detail {

enum class NativeLseOperation : std::uint8_t {
    add, clear, eor, set, smax, smin, umax, umin, swap, compare_swap
};

struct NativeLseInstruction {
    NativeLseOperation operation;
    unsigned bytes;
    unsigned source; // Rs: RMW input, or CAS expected value and returned old value.
    unsigned target; // Rt: RMW returned old value, or CAS replacement value.
    unsigned base; // Rn, with 31 denoting SP.
    bool acquire, release;
};

// All single-element B/H/W/X ordering forms. Pair and LL/SC instructions stay
// outside this decoder until their monitor/ABI integration is implemented.
[[nodiscard]] std::optional<NativeLseInstruction> decode_native_lse(
    std::uint32_t instruction) noexcept;
[[nodiscard]] bool accepts_native_lse(std::uint32_t instruction) noexcept;

// RCX=CpuState*, RDX=caller-validated, naturally aligned, readable+writable
// owned host span of at least the decoded width. The caller must pin backing
// for the whole invocation under a shared or exclusive execution lease. Shared
// use requires the same checked read/write backing and a non-executable mapping;
// publish each successful write journal before releasing the lease or observing
// checked guest state. These single-element LOCK forms need no exclusive monitor.
// RCX/RDX/R11 are preserved. Volatile RAX/R8/R9/R10 and host flags are scratch.
// R9D returns a private wrote flag: CAS match=1/mismatch=0; every other RMW=1,
// even when the replacement equals the old value. Journal only this write.
// No helpers, stack, PC/count, guest NZCV, vectors or host FP state changes.
[[nodiscard]] std::vector<std::uint8_t> emit_native_lse(
    std::uint32_t instruction);

} // namespace transmute::jit_detail
