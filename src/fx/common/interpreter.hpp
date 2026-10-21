#pragma once

// Reference interpreter on direct host memory (owner: integration).
// Used as the differential oracle in tests and as the runtime's
// one-instruction fallback for instructions the translator does not lower.

#include "transmute/fx/context.hpp"

#include <cstdint>

namespace transmute {
struct CpuState;
}

namespace transmute::fx {

void context_to_cpu_state(const GuestContext& ctx, CpuState& state);
void cpu_state_to_context(const CpuState& state, GuestContext& ctx);

// Executes the instruction `word` at ctx.pc with the reference interpreter,
// reading and writing guest (== host) memory directly. Updates registers,
// flags, FP state and pc. Throws std::runtime_error for instructions the
// reference interpreter does not implement; ctx is unchanged in that case.
// Returns true if the interpreter requested a guest exit.
//
// Exclusives (LDXR/LDXP/STXR/STXP and acquire/release forms, CLREX), CAS/CASP
// and the LSE atomics (LDADD..LDUMIN, SWP, ST* aliases) run with host
// interlocked operations on the guest address, so they are atomic with
// respect to other host threads. The exclusive monitor is
// GuestContext::excl_* with the translated path's semantics (STXR =
// cmpxchg against the value LDXR read), so interpreted and translated code
// can share a lock word. A guest memory fault leaves ctx unchanged.
bool interpret_instruction(GuestContext& ctx, std::uint32_t word);

// Fetches the word at ctx.pc (direct memory) and interprets it.
bool interpret_one(GuestContext& ctx);

} // namespace transmute::fx
