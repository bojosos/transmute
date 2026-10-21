#pragma once

// Instruction-selection analyses of the translator core (owner: translator
// core; used by emit_integer.cpp and emit_load_store.cpp).
//
// All facts are guest-level and exact: they hold for the guest register
// values themselves, whatever host register or context slot holds them.
// "Block" means the straight-line run of instructions since the last
// leader (a conditional branch inside it falls through to a non-leader, so
// everything earlier in the run has executed on every path that reaches
// the instruction).

#include "fx/translator/region_plan.hpp"

#include <cstdint>

namespace transmute::fx::core {

// TRANSMUTE_FX_CORE knobs restoring the previous lowering (read once; folded
// into codegen_knob_bits() << 16 and thus the translator fingerprint):
//   nofold   - no address folding (ADD/SUB + LDR/STR/LDP/STP)
//   nozext   - no zero/sign-extension elimination from known producers
//   nosrcreg - cached guest registers are not used as direct sources of
//              three-operand forms (lea, movsx, rorx); copies as before
//   nomovk   - MOVK of a known constant is not a single mov
//   oldisel  - all of the above
enum IselKnob : std::uint32_t {
    isel_no_fold = 1u,
    isel_no_zext = 2u,
    isel_no_srcreg = 4u,
    isel_no_movk = 8u,
};
std::uint32_t isel_knob_bits();
inline bool isel_on(std::uint32_t knob) { return (isel_knob_bits() & knob) == 0; }

// Further TRANSMUTE_FX_CORE knobs restoring a previous lowering (read once):
//   nobranchdirty - dirty cached registers are stored before every
//                   conditional branch outside loops, even when both
//                   successors take the cache over (dirty_crosses_branch)
//   nodeadpinloads - entries load every pin, also those the code overwrites
//                   before anything observes them (pins_dead_at_entry)
//   noflagsave    - flags that outlive an EFLAGS-clobbering instruction are
//                   always converted to ctx->nzcv (save_flags_around)
//   noccmpfusion  - CCMP is branchy even when its only reader follows it
//                   (e_ccmp: setcc/test fusion with B.cond and CSEL)
//   nogprreuse    - a GPR result never reuses the host that cached the
//                   destination's old value (reusable_gpr)
//   nobrdeadflags - NZCV stays live at every BR. By default (abi_flags) it
//                   is dead at a BR that does not look intra-function (no
//                   recognised table, no ADR/ADRP in its block), as at
//                   BL/BLR/RET (region_plan.cpp flags liveness)
// and opt-in ones (on when named):
//   mergedirty    - dirty cached registers also cross forward jumps and
//                   fall-throughs into merge points outside loops
//                   (dirty_into_merge)
// All are folded into the translator fingerprint (region_plan.cpp).
enum CodegenKnob : std::uint32_t {
    knob_no_branch_dirty = 1u,
    knob_no_br_flags_dead = 2u,
    knob_no_dead_pin_loads = 4u,
    knob_no_flag_save = 8u,
    knob_no_ccmp_fusion = 16u,
    knob_no_gpr_reuse = 32u,
    knob_merge_dirty = 64u,
};
std::uint32_t codegen_p2_knob_bits();
// Tests: use `bits` instead of the environment (~0u: the environment again).
void set_p2_knobs_for_testing(std::uint32_t bits);
inline bool codegen_on(std::uint32_t knob) { return (codegen_p2_knob_bits() & knob) == 0; }

// Facts about the value of Xg before instruction `index` (bit set = holds).
enum ExtFact : unsigned {
    ext_zero32 = 1u,   // bits 63..32 are zero (Xg == UXTW(Wg))
    ext_sign32 = 2u,   // Xg == SXTW(Wg)
};
// Index of the last instruction of the block before `index` that writes Xg
// (31 = SP), or -1 (none, unknown writer or too far back).
std::int32_t last_writer(const Plan& plan, std::int32_t index, unsigned g);
unsigned ext_facts(const Plan& plan, std::int32_t index, unsigned g);
// True if Xg holds a constant (MOVZ/MOVN/MOVK chain, ORR with XZR) before
// `index`, written in the block.
bool known_constant(const Plan& plan, std::int32_t index, unsigned g, std::uint64_t& value);
// True if Xg is overwritten after instruction `index`, within the block,
// before any read of it and before any instruction that may fault, branch
// or leave the region: its value after `index` is never observed.
bool dead_after(const Plan& plan, std::int32_t index, unsigned g);
// ADD/SUB Xd, ... at `index` whose only consumer is the adjacent integer
// load/store using Xd as its base: the access with the address computation
// folded into its addressing mode (AddrMode::offset with the combined
// displacement, or AddrMode::reg with rm/shift(option)/amount and imm as the
// displacement). Xd is dead after the access or rewritten by the load
// itself, so the folded pair never writes it. A fault of the folded access
// reports the ADD's pc with the state before it (Xd unchanged), which is
// exactly the state of re-executing both instructions.
bool fold_address(const Plan& plan, std::int32_t index, Inst& access);

} // namespace transmute::fx::core
