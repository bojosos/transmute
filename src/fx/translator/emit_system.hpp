#pragma once

// Atomics / system family (owner: atomics/system).
//
// Handles, per docs/FX_ENGINE.md invariant 3:
//  * ordered loads/stores (LDAPR/LDAPUR = mov; STLR/STLLR/STLUR = mov that
//    sets GuestContext::release_pending; LDAR/LDLAR = mov after a fence taken
//    only while it is set; see NOTES_SYSTEM.md "Ordering"),
//  * exclusives (LDXR/STXR/LDXP/STXP all widths, CLREX) through the
//    GuestContext::excl_* reservation and lock cmpxchg / cmpxchg16b,
//  * fusion of canonical LL/SC loops (add/sub/logic RMW, exchange, MSVC
//    compare-exchange, 128-bit load/exchange) into single locked operations,
//  * LSE (CAS/CASP/SWP/LDADD/LDCLR/LDEOR/LDSET/LD{S,U}{MAX,MIN}, all widths
//    and orderings),
//  * barriers, hints, MRS/MSR (TPIDR_EL0, TPIDRRO_EL0, NZCV, CNTVCT/CNTPCT,
//    CNTFRQ, PMCCNTR, DCZID, CTR, RNDR/RNDRRS), CFINV/XAFLAG/AXFLAG,
//  * cache maintenance (DC ZVA, DC C*, IC IVAU, IC IALLU[IS]),
//  * CRC32/CRC32C, AES, PMULL(2).1Q, SHA1, SHA256, SHA512, SHA3.
// FPCR/FPSR belong to the fp family; other EL1+ system registers are declined.

#include "fx/translator/emit_context.hpp"
#include "transmute/fx/context.hpp"

#include <cstdint>
#include <span>

namespace transmute::fx {

unsigned emit_system(EmitContext& ctx, std::span<const std::uint32_t> words);
bool describe_system(std::uint32_t word, InstructionUse& use);
// {"system", emit_system, describe_system}
const FamilyEmitter& system_family();

// ---- helper slots (HelperId range 48..63) ------------------------------------
enum SystemHelperId : unsigned {
    // void(GuestContext*, u64 address): fx::invalidate_guest_code on the page.
    helper_system_ic_ivau = helper_system_first + 0,
    // void(GuestContext*): invalidate all guest code (IC IALLU / IALLUIS).
    helper_system_ic_iallu = helper_system_first + 1,
    // u64(GuestContext*): CNTVCT_EL0 when rdtsc is not usable (QPC based).
    helper_system_counter = helper_system_first + 2,
    // u64(GuestContext*, u64 acc, u64 data, u64 kind): CRC32 without PCLMUL;
    // kind = bytes | (castagnoli ? 0x100 : 0).
    helper_system_crc32 = helper_system_first + 3,
    // void(GuestContext*, u64 word): portable vector crypto (AES/SHA/PMULL/
    // SHA512/SHA3) operating on ctx->v.
    helper_system_crypto = helper_system_first + 4,
    // Not callable: pointer to the counter scale (u64 multiplier, see
    // emit_system.cpp) read by the inline CNTVCT sequence.
    helper_system_timer_data = helper_system_first + 15,
};

// Value to store in GuestContext::helpers[id] for ids 48..63 (nullptr if unused).
const void* system_helper(unsigned id);
// Fills ctx.helpers[48..63]. The runtime calls this for every context.
void install_system_helpers(GuestContext& ctx);

// Nominal CNTFRQ_EL0 reported to the guest (CNTVCT_EL0 counts at this rate).
inline constexpr std::uint64_t kSystemCounterFrequency = 19'200'000;
// CTR_EL0: 64-byte I/D lines, PIPT I-cache, IDC=DIC=0 (software must use
// DC CVAU/IC IVAU, which drive translation invalidation).
inline constexpr std::uint64_t kSystemCtrEl0 = 0x8444c004;
// DCZID_EL0: DC ZVA permitted, 64-byte block.
inline constexpr std::uint64_t kSystemDczidEl0 = 4;

// ---- testing knobs (not thread-safe; set before emitting) --------------------
// Disables LL/SC loop fusion (unfused LDXR/STXR paths are emitted instead).
void system_set_fusion(bool enabled);
// Restricts host extensions: false = no AES-NI/PCLMUL/SHA-NI/RDRAND (helpers
// are used), true = everything the host supports (default).
void system_set_host_extensions(bool enabled);
// Forces the CNTVCT_EL0 helper path instead of inline rdtsc.
void system_set_inline_counter(bool enabled);
// true = the ordering mapping before ABI 11: STLR = xchg, LDAR plain, no DMB
// elision, DSB LD/ST no code (default: TRANSMUTE_FX_SYSTEM contains "oldorder").
void system_set_legacy_ordering(bool enabled);
// false = no ADDS/SUBS LL/SC fusion, only MOVs before a consumed barrier
// (default: true unless TRANSMUTE_FX_SYSTEM contains "oldfusion").
void system_set_extended_fusion(bool enabled);

} // namespace transmute::fx
