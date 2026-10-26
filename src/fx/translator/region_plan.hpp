#pragma once

// Region formation and static analyses (owner: translator core).
//
// A region is the set of guest instructions reachable from its start through
// direct control flow (both edges of conditional branches, BL continuations),
// laid out in guest address order. Each instruction appears once.

#include "fx/translator/core_decode.hpp"
#include "fx/translator/emit_context.hpp"
#include "fx/translator/translator.hpp"

#include <cstdint>
#include <vector>

namespace transmute::fx::core {

enum class Flow : std::uint8_t {
    normal,      // falls through to pc + 4
    jump,        // B: taken edge only
    cond,        // B.cond / CBZ / TBZ: taken + fall-through
    call,        // BL / BLR: exits; continuation is an entry
    indirect,    // BR / RET: exits
    trap,        // BRK / HLT: exits
    svc,         // SVC: exits; continuation is an entry
    interpret,   // no translation: interpret exit; continuation is an entry
};

struct PlanInst {
    std::uint64_t pc{};
    Inst inst;
    Flow flow{Flow::normal};
    std::int16_t family{-1};          // family whose describe() accepted the word
    bool leader{};                    // block start (entry or internal branch target)
    bool entry{};                     // expects the flushed state
    bool loop_header{};               // target of a back edge
    bool inherit_flags{};             // single predecessor laid out before: inherits its flags state
    bool conservative_writes{};       // family usage unknown: assume all registers written
    bool try_family{};                // not a core instruction: offer it to the families
    bool flags_live_in{};
    bool flags_live_out{};
    bool reads_flags{};
    bool writes_flags{};
    bool may_fault{true};             // may touch guest memory or leave translated code
    std::int32_t taken{-1};           // internal successor index (taken edge)
    std::int32_t fall{-1};            // internal successor index (fall-through, always self + 1)
    std::int32_t continuation{-1};    // call/svc/interpret continuation index (an entry)
    std::uint32_t reads{};
    std::uint32_t writes{};
    std::uint32_t vreads{};           // vector registers (family describe())
    std::uint32_t vwrites{};
    std::uint32_t dirty_in{};         // guest registers possibly modified since the last entry
    std::uint32_t vdirty_in{};        // same for vector registers
    std::uint16_t preds{};            // internal predecessor instructions
    std::int32_t first_pred{-1};
    std::uint32_t label{0xffffffffu};
    std::int32_t entry_slot{-1};      // index into Plan::entries
    // Call inlining (see build_plan): a BL whose callee body is laid out
    // right after it (flow normal, fall = the callee's first instruction;
    // it only writes x30 = link_pc), and the callee's RET X30 copies (flow
    // normal into the continuation laid out next, else a jump to it; no
    // code). Instructions of an inlined copy carry segment != 0 (the main
    // region is segment 0); they are never entries.
    bool inline_call{};
    bool inline_ret{};
    std::uint16_t segment{};
    std::int32_t inline_body{-1};     // build_plan internal: callee body of an inline_call
    std::uint64_t link_pc{};          // inline_call / inline_ret: the return address (BL pc + 4)
    // Switch dispatch (BR through a recognised jump table, see JumpTable):
    // index into Plan::tables, else -1. The BR keeps Flow::indirect (its
    // fallback is an indirect exit); its table targets are internal edges.
    std::int32_t table{-1};
    // A BR to xT loaded by `ldr xT, [xB, xI, lsl #3]` (or `uxtw #3`) from a
    // table that is not a label table (GuestContext::table_shadow): xB and xI,
    // unchanged up to the BR, else -1. The target is compared, so they only
    // decide how fast the BR is.
    std::int8_t shadow_base{-1};
    std::int8_t shadow_index{-1};
    bool shadow_index32{};            // uxtw #3: the index is wI
};

// A BR whose target comes from a jump table (MSVC ARM64 switch lowering):
//   adr xT, table; ldr{sw,sb,sh,b,h} xO, [xT, wI, uxtw #k]; [adr xB, base];
//   add xN, xB, xO, lsl #s; br xN
// Entry k's guest target is targets[first + k]. The emitter dispatches on
// wI inside the region and checks that xN equals the entry's target, so the
// recognition (and the table bound) only decides how fast a BR is, never
// where it goes: anything else takes the ordinary indirect exit.
// LLVM's lowering (clang, Mach-O images) loads with [xT, xI{, lsl #k}], often
// with xT set up before a loop and xI reused before the BR:
//   adr xB, base; ldr{b,h,sw} wO, [xT, xI, lsl #k]; add xN, xB, xO, lsl #2; br xN
// Those tables (by_target) dispatch on the target xN itself: a dense host
// table over the aligned case pcs maps each case to its label.
struct JumpTable {
    std::uint32_t first;              // into Plan::table_pcs / table_targets
    std::uint32_t count;              // entries (bound from cmp + b.hi/b.hs, uxtb, ...)
    std::uint8_t index_reg;           // wI: the table index at the BR (UXTW); unused by_target
    bool by_target{};                 // dispatch on the target (index unknown at the BR)
};

struct PlanEntry {
    std::int32_t index;               // instruction index
};

// Compact copy of the register usage of insts[i] (Plan::uses), for the
// emitter's lookahead scans (a PlanInst is two cache lines).
struct PlanUse {
    std::uint32_t reads, writes, vreads, vwrites;
    std::uint8_t bits;                // kUse* below
};
inline constexpr std::uint8_t kUseLeader = 1;     // block start
inline constexpr std::uint8_t kUseOpaque = 2;     // interpret exit or unknown family usage (conservative_writes)
inline constexpr std::uint8_t kUseStraight = 4;   // flow normal or cond
inline constexpr std::uint8_t kUseNormal = 8;     // flow normal

struct Plan {
    std::uint64_t start{};
    std::vector<PlanInst> insts;      // sorted by pc
    std::vector<std::uint32_t> words; // insts[i].inst.word, contiguous for family lookahead
    std::vector<PlanUse> uses;        // insts[i] usage, contiguous (see PlanUse)
    std::vector<PlanEntry> entries;   // entries[0] is the region start
    // Guest code the region depends on without translating it (a call or
    // jump target's first instructions, target_flags_dead): appended to the
    // region's guest_ranges, so a write there kills the region too.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> dependency_ranges;
    std::int8_t pin_host[32]{};       // host GPR id or -1
    std::uint32_t pinned{};           // guest register mask
    std::uint32_t pin_hosts{};        // host register mask used by pins
    bool extra_pins{};                // pins in volatile r10/r11 (saved around helper calls)
    // Cross-region register convention (translator_register_convention()):
    // `globals` are its registers pinned in their convention host (live in
    // it at every entry and exit, never loaded or stored there); `evicted`
    // ones live in the context inside the region: stored from their host at
    // entries, reloaded into it at exits (another pin may use the host).
    std::uint32_t globals{};
    std::uint32_t evicted{};
    std::int8_t vpin_host[32]{};      // host xmm id (6..15) or -1
    std::uint32_t vpinned{};          // guest vector register mask
    std::uint32_t vpin_hosts{};       // host xmm mask used by vector pins
    bool reg_cache{true};             // block-local register cache for unpinned registers
    bool fcmp_fusion{true};           // FCMP hands raw (u)comis EFLAGS to its consumers
    bool rbit_fusion{true};           // RBIT feeding only CLZ -> TZCNT
    bool write_back{true};            // register cache keeps results dirty until an observation point
    bool reuse_dest{true};            // reuse a cached destination overwritten by this instruction
    bool vec_facts{true};             // vector value facts for fusions (EmitContext::vec_fact)
    bool vzero_join{true};            // p3: zero-above facts of V registers joined at block starts
    // Tier 2 (TRANSMUTE_FX_CORE=tier2, TranslateOptions::optimize): the
    // optimizing tier of hot regions. count_hot: a first-tier (lazy) region
    // counts its entries (count_loops: and loop iterations) to get there;
    // optimize: this is the tier-2 translation (vector copy aliases; with
    // t2carry also dirty entries across loop edges).
    bool count_hot{};
    bool count_loops{};               // count_hot also counts loop iterations (TRANSMUTE_FX_CORE=t2loops)
    bool optimize{};
    bool vec_alias{};
    // Tier-2 code generation (optimize; each has a TRANSMUTE_FX_CORE=t2no<x>
    // knob, see core_translator.cpp; region_alloc.cpp has the analyses):
    bool t2_checks{};                 // loop-header interrupt checks covered by another header's are dropped
    bool t2_align{};                  // loop headers fallen into from the code above are not aligned
    bool t2_alloc{};                  // two passes: block entry states from every incoming edge, dirty values cross edges
    bool t2_dead{};                   // liveness: dirty cached values no path observes again are dropped, not stored
    bool t2_lend{};                   // pins a loop never uses lend their host to the register cache inside it
    bool t2_galias{};                 // GPR copy aliases (MOV Xd, Xn emits nothing)
    bool t2_derived{};                // derived vector values after definitions (EmitContext::vec_derived)
    bool count_loop_headers{};        // count_hot: loop headers (only) count the region's tier-up counter
    bool wb_fault_flush{true};        // false: measurement only (wrong guest state at faults)
    bool fault_maps{true};            // dirty cache entries are listed in fault maps instead of flushed
    bool carry{true};                 // register cache carried across block edges
    bool carry_clean{true};           // only clean entries cross block edges
    bool belady{true};                // cache victims by next use (else LRU)
    // Code layout (TRANSMUTE_FX_CORE=oldlayout restores the previous one):
    // rare paths (unlinked exit tails, RSB-miss and switch-fallback lookups,
    // interpret and trap exits) go to the cold section after the body; call
    // continuation entry stubs are laid out where nothing falls through;
    // only small loops are aligned; backward branches use rel8 when in reach.
    bool cold_layout{true};
    bool inline_entries{true};        // cold_layout: continuation entry stubs in line (not coldentries)
    bool align_loops{};               // cold_layout: every loop header aligned (loopalign)
    bool align_hot{};                 // cold_layout, tier 2: loop headers aligned whatever the loop's size (hotalign)
    bool flag_skip{};                 // direct exits may skip their flag store (RegionExit::skip_rel32_offset)
    unsigned loop_alignment{16};      // bytes (align32: 32)
    unsigned interpret_count{};
    unsigned blocks{};
    // TranslateOptions::lazy formation stopped early (an unfollowed call
    // continuation or the first-tier budget): the region is tier 0.
    bool truncated{};
    // TRANSMUTE_FX_CORE=notier: code generation before tiering (return stubs
    // of calls whose continuation is outside the region look the return pc up
    // instead of being linkable direct exits).
    bool lookup_return_stubs{};
    // Inlined calls: BL sites whose callee body was copied into the region,
    // and the guest instructions in those copies.
    unsigned inlined_calls{};
    unsigned inlined_insts{};
    // Jump tables (PlanInst::table): entry k of table t has guest target
    // table_pcs[t.first + k], laid out at instruction table_targets[...] (-1:
    // outside the region, a linkable direct exit).
    std::vector<JumpTable> tables;
    std::vector<std::uint64_t> table_pcs;
    std::vector<std::int32_t> table_targets;

    // Calls fn(successor index) for every internal successor of insts[i]
    // (taken, fall-through, jump-table targets; duplicates possible).
    template <class Fn>
    void for_each_successor(std::int32_t i, Fn&& fn) const {
        const auto& pi = insts[static_cast<std::size_t>(i)];
        if (pi.taken >= 0) fn(pi.taken);
        if (pi.fall >= 0) fn(pi.fall);
        if (pi.table >= 0) {
            const auto& t = tables[static_cast<std::size_t>(pi.table)];
            for (std::uint32_t k = 0; k < t.count; ++k)
                if (const auto s = table_targets[t.first + k]; s >= 0) fn(s);
        }
    }

    // Index of the main-region (segment 0) instruction at `pc`, or -1.
    [[nodiscard]] std::int32_t find(std::uint64_t pc) const;
};

// Forces specific instructions to be interpreted or treated conservatively
// (used when a first translation attempt hits an emitter limitation).
struct PlanOverrides {
    std::vector<std::uint64_t> interpret;
    std::vector<std::uint64_t> conservative;
    bool no_pins{};
    bool no_extra_pins{};
    unsigned vector_pin_limit{10};    // xmm6..xmm15
    bool no_reg_cache{};
    bool no_fcmp_fusion{};
    bool exact_flags{};               // NZCV live across BL/BLR/RET (not assumed dead per AAPCS64)
    bool family_pins7{};              // no volatile r10/r11 pins in regions with family instructions
    bool no_rbit_fusion{};
    bool no_write_back{};
    bool no_reuse_dest{};             // TRANSMUTE_FX_CORE=noreusedest
    bool no_vec_facts{};              // TRANSMUTE_FX_CORE=nofacts (also after a facts retry)
    bool no_vzero_join{};             // TRANSMUTE_FX_CORE=novzjoin (zero-above facts forgotten at every label)
    bool tier2{};                     // TRANSMUTE_FX_CORE=tier2: hot regions get the optimizing tier
    bool t2_no_alias{};               // TRANSMUTE_FX_CORE=t2noalias: tier 2 without vector copy aliases
    bool t2_carry{};                  // TRANSMUTE_FX_CORE=t2carry: tier 2 carries dirty values across loop edges
    bool t2_loops{};                  // TRANSMUTE_FX_CORE=t2loops: hot loops tier up at their header
    // Tier-2 features (on with the tier; TRANSMUTE_FX_CORE=t2no<x> turns one
    // off, t2proto all of them: the prototype's code generation).
    bool t2_no_checks{};              // t2nochecks: every loop header keeps its interrupt check
    bool t2_no_align{};               // t2noalign: fall-through loop headers stay aligned
    bool t2_no_alloc{};               // t2noalloc: one pass, entry states from the first edge (tier 1 rules)
    bool t2_no_dead{};                // t2nodead: no liveness-based dead stores
    bool t2_lend{};                   // t2lend (opt-in, measured slower): pins a loop never uses lend their host to the cache there
    bool t2_no_galias{};              // t2nogalias: no GPR copy aliases
    bool t2_no_derived{};             // t2noderived: no derived vector values (TBL computes its indices)
    bool t2_entries{};                // t2entries: first-tier regions count their entries (prototype policy), not loop iterations
    bool no_galias{};                 // after a GPR aliases retry
    bool no_vec_alias{};              // after an aliases retry
    bool wb_unsafe{};                 // measurement only: no write-back flush before fault sites
    bool no_fault_maps{};             // flush dirty cache entries before every fault site
    bool no_carry{};                  // block-local register cache (flushed before branches)
    bool old_pins{};                  // pin weights count every use (not a block's first read / write)
    bool lru{};                       // register-cache victims least recently used (not by next use)
    bool carry_dirty{};               // dirty cache entries also cross block edges inside loops
    bool eager{};                     // ignore TranslateOptions::lazy (no tier-0 regions)
    // Call inlining: BL targets with a small leaf body (at most inline_max
    // instructions, see build_plan) are copied into the caller's region.
    bool no_inline{};                 // TRANSMUTE_FX_CORE=noinline
    bool inline_hot{};                // TRANSMUTE_FX_CORE=inlinehot: lazy (tier-0) regions defer inlining to tier 1
    unsigned inline_max{kDefaultInlineMax};   // TRANSMUTE_FX_INLINE=<n>
    static constexpr unsigned kDefaultInlineMax = 24;
    // Optimized (tier-2) translations of hot code inline larger leaves, up to
    // kOptimizedInlineMax instructions, and local subroutines that start
    // inside a function (assembly helpers such as dav1d's L(8tap_filter_8),
    // called per row of a loop). TRANSMUTE_FX_CORE=noloopinline: the rules
    // above everywhere.
    static constexpr unsigned kOptimizedInlineMax = 64;
    bool no_loop_inline{};
    // Switch dispatch: BR through a recognised jump table stays in the region
    // (Plan::tables). TRANSMUTE_FX_CORE=notables: every BR is an indirect exit.
    bool no_tables{};
    // TRANSMUTE_FX_CORE=tableexits: table targets are not followed (each
    // entry is a linkable direct exit to the case's own region).
    bool table_exits{};
    // TRANSMUTE_FX_CORE=oldlayout: the code layout before the hot-footprint
    // work (Plan::cold_layout off). TRANSMUTE_FX_CORE=oldcap: regions stop at
    // max_instructions even inside a .pdata function (see build_plan).
    bool old_layout{};
    bool old_cap{};
    // Finer layout knobs (codegen_layout_bits): coldentries keeps every
    // secondary entry stub in the cold section; loopalign aligns every loop.
    bool cold_entries{};
    bool align_loops{};
    bool align_hot{};                 // hotalign: tier-2 regions align loop headers of any size
    bool align32{};                   // align32: loop headers aligned to 32 bytes, not 16
    // TRANSMUTE_FX_CORE=noshadows: BRs through heap tables take the inline
    // lookup only (no table shadow, PlanInst::shadow_base).
    bool no_shadows{};
    // TRANSMUTE_FX_CORE=noflagskip: direct exits always store host-form flags
    // (no RegionExit::skip_rel32_offset).
    bool no_flag_skip{};
};

struct FamilyTable {
    const FamilyEmitter* families;
    unsigned count;
};
FamilyTable registered_families();
// TRANSMUTE_FX_CORE debugging knobs that change generated code (0 = defaults).
std::uint32_t codegen_knob_bits();
// Layout knobs beyond codegen_knob_bits (coldentries, loopalign; 0 = defaults).
std::uint32_t codegen_layout_bits();
// TRANSMUTE_FX_INLINE=<n> (call inlining size limit; PlanOverrides::kDefaultInlineMax by default).
unsigned codegen_inline_max();
// Tier-2 knobs beyond codegen_knob_bits (t2no<x>, t2proto, t2entries; 0 = defaults).
std::uint32_t codegen_tier2_bits();
// Tests only: translations on this thread use `overrides` (nullptr: the
// TRANSMUTE_FX_CORE defaults) as their starting point. Not part of any cache key.
void set_overrides_for_testing(const PlanOverrides* overrides);
// The TRANSMUTE_FX_CORE defaults (read once).
const PlanOverrides& default_plan_overrides();

// Builds the plan. Throws std::runtime_error if `guest_pc` cannot be read.
// Exact flags across a direct exit: true when the code at target_pc writes
// NZCV before reading it on every path (the exit's flags are dead), and then
// records what it read as a dependency of `plan` (owner: p1, the target scan;
// the flags liveness in build_plan asks it).
bool target_flags_dead(const CodeSource& code, std::uint64_t target_pc, Plan& plan);

void build_plan(std::uint64_t guest_pc, const CodeSource& code, const TranslateOptions& options,
                const PlanOverrides& overrides, Plan& plan);

} // namespace transmute::fx::core
