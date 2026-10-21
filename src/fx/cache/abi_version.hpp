#pragma once

// Translator ABI version (defined by cache; BUMPED BY THE TRANSLATOR OWNER).
//
// Increment whenever previously translated code must no longer be reused:
// a change to emitted code conventions (GuestContext usage, helper ids,
// stubs, RSB protocol, exit tails), to TranslatedRegion semantics (entries,
// exits, fixups, fault sites, pins), or any code-generation bug fix. Every
// persistent cache file is keyed by this value; stale files are ignored and
// eventually garbage-collected.

#include <cstdint>

namespace transmute::fx {

// 10: address folding, extension facts (core_isel.hpp)
// 11: memory ordering (STLR = mov + GuestContext::release_pending, LDAR fences
//     iff pending, DMB elision, DSB LD/ST fence) and ADDS/SUBS LL/SC fusion
// 12: tiers (tier-0 entry counters, ExitReason::tier_up), RSB continuation
//     stubs as linkable direct exits, synthetic bounds for code without .pdata
// 13: call inlining (small leaf callees copied into the caller's region; the
//     region's guest ranges cover the callee)
// 14: SIMD&FP address folding (EmitContext::address, FamilyEmitter::
//     address_base), by-element broadcasts from current context slots
//     (EmitContext::vec_slot), single-table TBL without a temp
// 15: switch dispatch in the region (BR through a recognised jump table:
//     host jump table + target check), regions with tables up to 4x larger
// 16: cross-region register convention (translator_register_convention: SP,
//     x19-x22, FP, LR live in host callee-saved registers across region
//     boundaries; stub_indirect_live, stub_host_return, host-entry adapters)
// 17: SIMD&FP cost audit (NOTES_NEON.md / NOTES_FP.md): pooled FMOV
//     immediates and FNMUL/FABD/FACGE/FACGT masks, UCVTF via FMA, SCVTF S
//     in the vector domain, USHL/SSHL vpsignd / AVX2 D path, 64-bit vector
//     results without zeroing when sources are known zero above bit 64
// 18: guarded SIMD mask reduction fusion and reuse of nonfaulting cached
//     vector destinations; do not reload code emitted before these changes.
// 19: mask reductions use vector value facts (MOVI/compare/checked-load
//     definitions) and the USHR #31 form; optimizing tier (tier 2)
// 20: hot-footprint layout: unlinked exit tails, RSB-miss and switch-fallback
//     lookups, interpret and trap exits in the cold section; continuation
//     entry stubs in line; rel8 back edges; only small loops aligned; regions
//     cover whole .pdata functions up to 16384 instructions
// 21: tier 2 on by default: first-tier regions with loops count iterations at
//     their loop headers (ExitReason::tier_up_loop, one counter per region in
//     its own cache line), regions without loops are final; tier-2 code:
//     covered interrupt checks dropped, loop-header entry states from two
//     passes with loop-carried dirty values, liveness-based dead stores, GPR
//     copy aliases, TBL indices precomputed after their definitions
// 22: preserve current branch operands when pruning dead stores before
//     materializing NZCV, so CBZ/TBZ cannot reload an unstored stale value.
// 23: PMCCNTR_EL0 reads the host TSC instead of taking an illegal-instruction exit.
inline constexpr std::uint32_t kTranslatorAbiVersion = 23;

} // namespace transmute::fx
