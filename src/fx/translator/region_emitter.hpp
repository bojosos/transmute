#pragma once

// Region code generator (owner: translator core): the optimized EmitContext
// used for families plus the core integer/branch/load-store emitters.
//
// Register model: plan.pin_host[] maps hot guest GPRs to host callee-saved
// registers for the whole region body (loaded at entries, dirty ones stored
// at exits and before state helpers). Other guest registers live in
// GuestContext and are addressed as [r15 + x_disp(n)]. Registers of the
// cross-region convention (translator_register_convention) are pins in their
// convention host that are live at entries and exits (plan.globals), or
// evicted (plan.evicted: context inside the region, see entry_pins/exit_pins).
//
// Flags model: FlagsState says where the current NZCV lives: in ctx->nzcv
// or in host EFLAGS with FlagsProducer meaning. Host flags are materialized
// (lahf/seto, no pushfq) only when they escape: exits, helpers, EFLAGS
// clobbers while live, and block edges into blocks that expect the context.

#include "fx/common/x64_assembler.hpp"
#include "fx/translator/core_isel.hpp"   // instruction-selection analyses (additive)
#include "fx/translator/emit_context.hpp"
#include "fx/translator/region_plan.hpp"
#include "fx/translator/translator.hpp"

#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

namespace transmute::fx::core {

// host_fcmp: EFLAGS (ZF PF CF) of (u)comis(Rn, Rm) left by a fused FCMP;
// host_fcmp_swapped: of (u)comis(Rm, Rn).
enum class FlagsState : std::uint8_t { ctx, host_sub, host_add, host_logic, host_fcmp, host_fcmp_swapped };
constexpr bool is_host(FlagsState s) noexcept { return s != FlagsState::ctx; }
constexpr bool is_fcmp(FlagsState s) noexcept {
    return s == FlagsState::host_fcmp || s == FlagsState::host_fcmp_swapped;
}

// Thrown by the emitter when an instruction cannot be emitted in this
// configuration; translate_region retries with an override.
struct RetryTranslation {
    enum class Resource : std::uint8_t { other, gpr, vec, writeback, facts, aliases };
    std::uint64_t pc;
    bool conservative;   // false: interpret it; true: treat family usage as unknown
    Resource resource = Resource::other;   // scratch exhaustion: which pool
};

// Diagnostics (relaxed counters).
struct CoreCounters {
    std::atomic<std::uint64_t> regions;
    std::atomic<std::uint64_t> retries;
    std::atomic<std::uint64_t> inlined_calls;   // BL sites inlined in emitted regions
    std::atomic<std::uint64_t> tables;          // BRs dispatched through a host jump table (Plan::tables)
};
CoreCounters& core_counters();
// True if the neon family's region hooks (constant pool) are linked in.
bool neon_region_hooks_linked();

// Diagnostics (fx_translator_image_audit, tools/fx_loop_count.py): while a
// thread's sink is set, every RegionEmitter::run() on it records, for each
// emitted guest instruction, its pc, the offset of its block label (leaders;
// else = begin) and the offset where its own code begins. Cleared per attempt.
struct InstructionMapEntry {
    std::uint64_t pc;
    std::uint32_t label;
    std::uint32_t begin;
    bool leader;
};
std::vector<InstructionMapEntry>*& instruction_map_sink();

class RegionEmitter final : public EmitContext {
public:
    RegionEmitter(const Plan& plan, const TranslateOptions& options, TranslatedRegion& out);
    void run();
    // Tier 2 (Plan::t2_alloc; region_alloc.cpp): the region is emitted twice.
    // Pass 1 records the register cache at every internal edge into a block
    // (before stores); build_presets() then chooses each merge point's and
    // loop header's entry state from all of its incoming edges, weighted by
    // loop depth, and pass 2 emits with those states (dirty values cross
    // edges whose target keeps them). 0 = single pass.
    void set_alloc_pass(unsigned pass) { alloc_pass_ = pass; }
    void build_presets();

    // ---- EmitContext -------------------------------------------------------
    x64::Assembler& as() override { return a_; }
    [[nodiscard]] std::uint64_t pc() const override { return pc_; }
    x64::Gp read_gpr(unsigned index, bool sp) override;
    x64::Gp write_gpr(unsigned index, bool sp) override;
    x64::Gp copy_gpr(unsigned index, bool sp) override;
    x64::Gp temp_gpr() override;
    x64::Xmm read_vec(unsigned index) override;
    x64::Xmm write_vec(unsigned index) override;
    x64::Xmm temp_vec() override;
    void clobber_host_flags() override;
    void set_flags(FlagsProducer producer) override;
    void set_nzcv(x64::Gp value) override;
    x64::Gp get_nzcv() override;
    x64::Cond condition(unsigned arm_condition) override;
    void mark_may_fault() override;
    x64::Gp call_helper(unsigned id, std::initializer_list<HelperArg> args, HelperEffects effects) override;
    x64::Gp guest_address(std::uint64_t address) override;
    x64::Gp temp_gpr_prefer(x64::Gp preferred) override;
    void load_gpr(x64::Gp dst, unsigned index, bool sp) override;
    void store_gpr(unsigned index, x64::Gp src, bool sp) override;
    VecSrc vsrc(unsigned index) override;
    GprSrc gsrc(unsigned index, bool sp) override;
    bool fcmp_swap() override;
    bool set_fcmp_flags(bool swapped) override;
    [[nodiscard]] bool vec_write_deferred(unsigned index) const override {
        return index < 32 && !vpinned(index) && !reusable_vec(index);
    }
    // ---- vector upper-bit knowledge (fp/neon; additive) ----
    [[nodiscard]] std::uint32_t vec_zero_above(unsigned width) const override {
        return width == 32 ? vzero32_ : width == 64 ? vzero64_ : width >= 128 ? ~0u : 0u;
    }
    void note_vec_zero_above(unsigned index, unsigned width) override;
    // ---- guest address operands (neon; additive): EmitContext::address ----
    x64::Mem address(const GuestAddress& g) override;
    // ---- current context slots (neon/fp; additive): EmitContext::vec_slot ----
    bool vec_slot(unsigned index, x64::Mem& slot) override {
        if (index >= 32 || vpinned(index)) return false;
        const auto h = vc_.host[index];
        if (h >= 0 && ((vc_.dirty >> h) & 1u)) return false;   // the register is newer than the slot
        if ((valias_dirty_ >> index) & 1u) return false;        // tier 2: a dirty alias
        if (vslot_alias_[index] >= 0) return false;             // tier 2: in another slot (see vsrc)
        slot = x64::ptr(x64::Gp::r15, v_disp(index));
        return true;
    }
    // ---- vector value facts (neon fusion; additive): EmitContext::vec_fact ----
    VecFact vec_fact(unsigned index, const VecValue& value) override;
    bool vec_lane_masks(unsigned index) override;
    bool vec_derived(unsigned index, unsigned kind, x64::Mem& slot) override;   // tier 2

private:
    // fp/neon (additive): forget which V registers are known zero above bit
    // 32/64. Called at labels, helper calls and exits.
    void forget_vec_zero() {
        vzero32_ = vzero64_ = 0;
        vzwritten_ = ~0u;   // p3: as if every V register were written (vzero_record)
    }
    using Gp = x64::Gp;
    using Mem = x64::Mem;
    using Cond = x64::Cond;

    // A guest operand: pinned host register, context slot, or zero (XZR).
    struct Val {
        enum class K : std::uint8_t { reg, mem, zero } k{K::zero};
        Gp r{Gp::none};
        std::uint8_t g{};
        [[nodiscard]] bool is_reg() const { return k == K::reg; }
        [[nodiscard]] bool is_zero() const { return k == K::zero; }
        [[nodiscard]] bool is_mem() const { return k == K::mem; }
    };

    struct Stub {
        // Cold-section code (Plan::cold_layout): exit_tail is the unlinked
        // tail of an in-line direct exit (`field` = label of its jmp opcode);
        // lookup the stub_indirect lookup of a target in rcx (state flushed);
        // indirect_exit stores the pins (`dirty`) first; reason_exit stores
        // the pins and exits with `reason` (pc, `arg`).
        enum class K : std::uint8_t { direct_exit, edge, interrupt, entry, return_trampoline, entry_jump, tier_up,
                                      loop_tier_up, exit_tail, lookup, indirect_exit, reason_exit,
                                      region_tier_up } k;   // region_tier_up: a loop's counter ran out (tier 2)
        x64::Label label;
        FlagsState flags;
        std::uint64_t dirty;   // pinned GPRs (low 32 bits) and vectors (high 32 bits)
        std::uint64_t pc;
        std::int32_t target;
        // Register cache at the branch (s_.cache_snaps index, -1 = none): exits
        // and interrupt stubs store its dirty entries first; edge stubs
        // reconcile it to the target block's entry state.
        std::int32_t snap{-1};
        // Reached in the boundary state (region-start checks before the
        // entry's pin loads): evicted convention registers are still in
        // their hosts, so the exit must not reload them.
        bool boundary{};
        x64::Label field{};             // exit_tail
        // Tier 2: pins lent to the register cache where the stub's code
        // starts (exits reload lent convention registers; edge stubs go from
        // this to the target's lent set). 0 in the boundary state.
        std::uint64_t lent{};
        ExitReason reason{};            // reason_exit
        bool has_arg{};
        std::uint64_t arg{};
    };

    // ---- driver --------------------------------------------------------------
    void emit_instruction(std::int32_t index);
    void begin_instruction(std::int32_t index);
    void end_instruction();
    void start_block(std::int32_t index);
    unsigned try_families(std::int32_t index);
    unsigned emit_core(const PlanInst& pi);   // returns guest instructions consumed
    void emit_fallthrough(std::int32_t index, unsigned consumed);
    void emit_stubs();
    x64::Label label_of(std::int32_t index);

    // ---- scratch registers ---------------------------------------------------
    Gp alloc();
    [[nodiscard]] std::uint64_t evict_cost(unsigned g, bool dirty, bool vec, std::uint32_t stamp) const;
    bool belady_{true};   // victims by next use (TRANSMUTE_FX_CORE=lru: least recently used)
    // evict_cost lookahead per register ([0, 32) GPRs, [32, 64) vectors) of
    // instruction next_use_index_ (next_use_known_: computed ones).
    mutable std::int32_t next_use_index_{-1};
    mutable std::uint64_t next_use_known_{};
    mutable std::uint8_t next_use_reload_[64]{};
    mutable std::uint8_t next_use_distance_[64]{};
    void take(Gp r);
    // take(r) for an implicit use of guest value `v` in r (the target of BR,
    // BLR): when r already caches v's guest, the value stays in r (stored
    // first if dirty) instead of being evicted and reloaded from the context.
    void take_value(Gp r, const Val& v);
    [[nodiscard]] bool is_free(Gp r) const { return (free_gpr_ >> x64::id(r)) & 1u; }
    x64::Xmm alloc_vec();
    void reset_scratch();

    // ---- block-local write-through register cache -----------------------------
    // An unpinned guest register whose value (== its context slot) is held in
    // a scratch register. Entries survive instructions while the register is
    // not handed out writable (alloc/take evict); they die at block leaders,
    // helper calls and direct context writes. Writes stay write-through:
    // pending destinations are stored at end_instruction and then become
    // entries. Reads that hit "borrow" the register (read-only, not free) for
    // the rest of the instruction.
    struct RegCache {
        std::int8_t host[32];      // guest -> host register id, or -1
        std::int8_t guest[16];     // host register id -> guest, or -1
        std::uint32_t used[16];    // LRU stamps
        std::uint32_t mask;        // hosts holding an entry
        std::uint32_t dirty;       // hosts whose value is not yet stored to the context (write-back)
    };
    void cache_clear();
    // Write-back: destinations stay dirty in their register until a point where
    // guest state may be observed (fault site, helper, exit, block end) or the
    // register is reused; flush_dirty() stores them all.
    void flush_dirty(bool edge = false);   // edge: code on one edge (no internal-path check)
    void flush_dirty_except(std::uint32_t keep_g, std::uint32_t keep_v, std::uint64_t dead);   // tier 2
    struct CacheSnapshot;
    void keep_matching(const CacheSnapshot& e, std::uint32_t& keep_g, std::uint32_t& keep_v) const;
    void carried_dirty(std::int32_t index, std::uint32_t& keep_g, std::uint32_t& keep_v) const;
    [[nodiscard]] bool leaves_loop(std::int32_t index, std::int32_t target) const;
    // A conditional branch whose internal successors each have it as their
    // only predecessor (its fall-through continues the block; a taken target
    // takes over the whole cache, dirty entries included) and whose exits
    // spill their dirty snapshot: the dirty cache stays in registers across
    // it instead of being stored first.
    [[nodiscard]] bool dirty_crosses_branch(std::int32_t index) const;
    [[nodiscard]] bool dirty_into_merge(std::int32_t target) const;
    void evict_gpr(unsigned h);   // drop host h's entry, storing it first if dirty
    void evict_vec(unsigned h);
    void cache_drop_host(RegCache& c, unsigned h);
    void cache_drop_guest(RegCache& c, unsigned g);
    static void drop_entry(RegCache& c, unsigned h);   // no code, no fault-site bookkeeping
    static void reset_cache(RegCache& c);
    void cache_set(RegCache& c, unsigned g, unsigned h);
    // True while this instruction has emitted no internal control flow: only
    // then may a load create an entry (the load runs on every path).
    [[nodiscard]] bool may_cache() const { return cache_on_ && a_.control_flow_count() == cf_mark_; }
    Gp cached_gpr(unsigned g);              // hit: host register (peek, not borrowed)
    Gp borrow_gpr(unsigned g);              // hit: borrowed host register
    Gp read_unpinned(unsigned g);           // borrowed, loaded (and cached) on a miss
    x64::Xmm borrow_vec(unsigned v);        // hit: borrowed xmm, else xmm(none-like 0xff)
    // A nonfaulting instruction may overwrite its cached destination in
    // place. Faulting instructions retain the previous value for recovery.
    [[nodiscard]] bool reusable_vec(unsigned v) const {
        return plan_.reuse_dest && cur_ && !cur_->may_fault && !cur_->conservative_writes &&
               cur_->flow == Flow::normal && may_cache() && vc_.host[v] >= 0 &&
               valias_of_[static_cast<unsigned>(vc_.host[v])] == 0;   // tier 2: aliases keep the value
    }
    // ---- tier 2: vector copy aliases (block-local value identities) -----------
    // MOV Vd.16B, Vn.16B emits no code: Vd becomes another name of the host
    // that holds Vn's value (a cache entry's owner is vc_.guest[h]; aliases
    // are extra guests with the same value). An alias is dirty while its
    // context slot is older; vslot_[h] records which slots hold h's value,
    // so a copy back (the MOV rotations compilers leave in loops) is clean.
    // Aliases never cross a block boundary, helper call or exit: they are
    // stored (if dirty) and dropped first (alias_resolve). Fault maps list
    // the dirty ones. TRANSMUTE_FX_CORE=t2noalias disables them.
    std::int8_t valias_host_[32];
    // Slot aliases: MOV from a register that is only in its context slot.
    // vslot_alias_[a] = s: V`a`'s value is slot s's content (a's own slot is
    // older). They need no register at all, and a copy back (s's value
    // returning to s) costs nothing. Fault maps cannot describe them, so they
    // are copied into their own slots (slot_aliases_all) before instructions
    // that may fault, helpers, exits and block ends, and before slot s is
    // overwritten (slot_aliases_of).
    std::int8_t vslot_alias_[32];
    unsigned slot_alias_count_{};
    void slot_alias_materialize(unsigned guest);
    void slot_aliases_of(unsigned source);
    void slot_aliases_all();
    std::uint32_t valias_of_[16]{};     // host -> alias guests
    std::uint32_t valias_dirty_{};      // alias guests whose slot is stale
    std::uint32_t vslot_[16]{};         // host -> guests whose context slot holds that value (block-local)
    bool aliasing_{};
    unsigned alias_count_{};
    bool emit_vec_copy(const PlanInst& pi);          // true: the MOV was handled
    void alias_add(unsigned guest, unsigned host);
    void alias_drop_guest(unsigned guest);          // the guest is overwritten (no store)
    void alias_store_dirty(unsigned host);           // store dirty aliases of host (they become clean)
    void alias_drop_host(unsigned host);             // store dirty aliases, then forget them
    void alias_owner_leaves(unsigned host);          // the owner gets a new value: an alias owns the host
    void alias_resolve();                            // every alias: stored if dirty, dropped
    void alias_forget_knowledge();                   // vslot_ (no code)
    void slot_stored(unsigned guest, unsigned host); // the guest's slot now holds host's value
    void alias_guard() const;                        // aliases must be resolved here (else retry)
    // ---- tier 2: GPR copy aliases (block-local) ----------------------------------
    // MOV Xd, Xn (ORR Xd, XZR, Xn) between unpinned registers emits nothing
    // when Xn is in a cache register: Xd becomes another name of that host
    // (dirty: its context slot is older; fault maps list it). Aliases are
    // resolved (copied into a free scratch register as Xd's own entry, else
    // stored) before control flow, block ends, family and interpreted
    // instructions, and any instruction that writes Xd or Xn; evicting the
    // host stores them. TRANSMUTE_FX_CORE=t2nogalias disables them.
    std::int8_t galias_host_[32];
    std::uint32_t galias_of_[16]{};     // host -> alias guests
    std::uint32_t galias_dirty_{};      // alias guests whose slot is stale
    unsigned galias_count_{};
    bool galiasing_{};
    bool emit_gpr_copy(const PlanInst& pi);
    void galias_resolve();                           // every alias becomes an entry of its own or is stored
    void galias_promote(unsigned g);                 // alias g owns its host's entry, the owner becomes the alias
    void galias_drop_host(unsigned host);            // store dirty aliases of host, forget them
    [[nodiscard]] std::uint32_t galias_touched() const;   // alias guests and the owners of their hosts
    [[nodiscard]] bool vec_read_soon(unsigned v) const;   // lookahead in the block

    // ---- guest operands ------------------------------------------------------
    // Pinned here: tier 2 lends the host of a pin a loop never uses to the
    // register cache inside that loop (lent_; the value is in the context).
    [[nodiscard]] bool pinned(unsigned g) const { return plan_.pin_host[g] >= 0 && !((lent_ >> g) & 1u); }
    [[nodiscard]] Gp pin(unsigned g) const { return x64::gp(static_cast<unsigned>(plan_.pin_host[g])); }
    static Mem gmem(unsigned g) { return x64::ptr(Gp::r15, x_disp(g)); }
    static Mem ctx_mem(std::size_t offset) { return x64::ptr(Gp::r15, ctx_disp(offset)); }
    Val val(unsigned index, bool sp = false) const;
    Gp in_reg(const Val& v);
    Gp in_reg(unsigned index, bool sp = false) { return in_reg(val(index, sp)); }
    Gp copy_of(const Val& v);
    void load(unsigned bits, Gp d, const Val& v);
    void alu(x64::Alu op, unsigned bits, Gp d, const Val& v);
    void zero(Gp d);
    Gp dst(unsigned index, bool sp = false);
    Gp dst_rmw(unsigned index, bool sp = false);
    void note_write(unsigned g);
    void binop(x64::Alu op, unsigned bits, Gp d, const Val& n, const Val& m, bool commutative);
    [[nodiscard]] std::uint64_t current_dirty() const;
    // Every pin from the context (after a helper that may write guest state).
    void load_pins();
    // Region entries (start, entry stubs): evicted convention registers go to
    // the context, then the other pins are loaded (convention pins are live).
    void entry_pins(std::int32_t index);   // at the entry of instruction `index`
    // Pinned GPRs the straight-line code from entry `index` overwrites before
    // anything can observe them (read, fault, exit, block end): not loaded there.
    [[nodiscard]] std::uint32_t pins_dead_at_entry(std::int32_t index) const;
    [[nodiscard]] std::uint32_t vpins_dead_at_entry(std::int32_t index) const;   // p3: vector pins
    // Dirty pins to the context, convention pins included (before helpers).
    void flush_pins(std::uint64_t dirty);
    // Exits (to translated code or the runtime's stubs): dirty pins except the
    // convention pins to the context, then, with `restore`, the evicted
    // convention registers back into their hosts. Paths still in the entry
    // state (region-start stubs, return trampolines) pass restore = false.
    void exit_pins(std::uint64_t dirty, bool restore = true);
    [[nodiscard]] bool global(unsigned g) const { return (plan_.globals >> g) & 1u; }
    [[nodiscard]] bool vpinned(unsigned v) const { return plan_.vpin_host[v] >= 0 && !((lent_ >> (32 + v)) & 1u); }
    [[nodiscard]] x64::Xmm vpin(unsigned v) const { return x64::xmm(static_cast<unsigned>(plan_.vpin_host[v])); }

    // ---- flags -----------------------------------------------------------------
    [[nodiscard]] bool keep_flags() const;
    bool save_flags_around();
    [[nodiscard]] std::int32_t ccmp_bool_reader() const;
    [[nodiscard]] Gp reusable_gpr(unsigned index) const;
    void prepare_clobber();
    void materialize();
    void materialize_state(FlagsState state, bool save_rax);
    [[nodiscard]] FlagsLocation fault_flags() const;
    Cond host_condition(unsigned cond);
    [[nodiscard]] bool needs_edge_materialize(std::int32_t target) const;
    // ARM condition over fused FCMP flags: one x64 condition, or `c` combined
    // with the parity flag (AND not-parity / OR parity).
    struct FcmpCond {
        enum class K : std::uint8_t { single, and_np, or_p } k;
        Cond c;
    };
    [[nodiscard]] FcmpCond fcmp_cond(unsigned cond) const;
    void materialize_fcmp(bool swapped, bool save_rax);
    void cond_branch_fcmp(const FcmpCond& fc, std::int32_t taken, std::uint64_t taken_pc, std::int32_t fall,
                          std::uint64_t fall_pc);
    [[nodiscard]] int next_flags_condition() const;   // ARM cond of the next flags reader, or -1

    // ---- faults ----------------------------------------------------------------
    void fault_close();
    // Fault map of the dirty register-cache entries (1-based index into
    // out_.fault_maps, 0 = none); deduplicated against the previous map.
    std::uint16_t dirty_map();
    // The dirty set changed while a fault site is open: close it and open a
    // new one (same pc and flags) with the current map.
    void fault_reopen();

    // ---- exits -----------------------------------------------------------------
    std::size_t fixup_imm64(Gp r, std::uint64_t value);
    // mov r, constant. A 10-byte movabs records a literal_imm64 fixup so the
    // persistent cache knows the 8 bytes are a constant, not an address.
    void mov_literal(Gp r, std::uint64_t value);
    void emit_direct_exit(std::uint64_t target, std::uint64_t dirty, FlagsState flags, bool restore = true);
    void emit_exit_with_reason(std::uint64_t pc, ExitReason reason, std::uint64_t arg, bool has_arg);
    void emit_interpret_exit(std::int32_t index);
    // Pins to the context (evicted convention registers back), then an exit
    // with `reason`: in line, or a jmp to the cold section (Plan::cold_layout).
    void emit_reason_exit(std::uint64_t dirty, std::uint64_t pc, ExitReason reason, std::uint64_t arg, bool has_arg);
    // Branches to a label: rel8 when it is bound (backward) and in reach
    // (Plan::cold_layout), else rel32.
    void jcc_label(Cond c, x64::Label l);
    void jmp_label(x64::Label l);
    // 128-bit moves between a vector register and its context slot (pins,
    // register cache, spills): movups with Plan::cold_layout, one byte
    // shorter than movdqu (no REX/prefix difference otherwise); the bits
    // moved are the same and loads and stores have no domain penalty.
    void vmove(x64::Xmm dst, const x64::Mem& src) {
        if (plan_.cold_layout) a_.movups(dst, src);
        else a_.movdqu(dst, src);
    }
    void vmove(const x64::Mem& dst, x64::Xmm src) {
        if (plan_.cold_layout) a_.movups(dst, src);
        else a_.movdqu(dst, src);
    }
    // Layout: true while emit_stubs() writes the cold section. no_fall_at_ is
    // the code size right after an instruction whose code never falls
    // through (calls, returns, traps, interpret exits): a continuation entry
    // stub laid out there costs no jump (start_block).
    bool cold_{};
    std::size_t no_fall_at_{~std::size_t{0}};
    x64::Label add_stub(Stub::K kind, FlagsState flags, std::uint64_t dirty, std::uint64_t pc, std::int32_t target,
                        std::int32_t snap = -1, bool boundary = false);
    x64::Label taken_label(std::int32_t taken, FlagsState state);
    std::int32_t dirty_snap();   // snapshot of the cache if it has dirty entries, else -1
    void edge_inline(std::int32_t target);
    void jump_to(std::int32_t target, std::uint64_t target_pc);
    void cond_branch(Cond c, std::int32_t taken, std::uint64_t taken_pc, std::int32_t fall, std::uint64_t fall_pc);
    void rsb_push(std::uint64_t return_pc, std::int32_t continuation, Gp ret_reg = Gp::rdx);

    // ---- core emitters (emit_integer.cpp) --------------------------------------
    void e_add_sub_imm(const Inst& d);
    void e_logic_imm(const Inst& d);
    unsigned e_move_wide(const PlanInst& pi);
    unsigned e_adr(const PlanInst& pi);
    void e_bitfield(const Inst& d);
    void e_extr(const Inst& d);
    void e_add_sub_shift(const Inst& d);
    void e_add_sub_ext(const Inst& d);
    void e_adc(const Inst& d);
    void e_logic_shift(const Inst& d);
    void e_ccmp(const Inst& d);
    void e_csel(const Inst& d);
    void e_dp1(const Inst& d);
    // RBIT Xd, Xn feeding only CLZ ?, Xd (count trailing zeros): index of the
    // CLZ, which then emits TZCNT of Xn, or -1. See emit_integer.cpp.
    [[nodiscard]] std::int32_t rbit_clz_target(const Inst& d) const;
    std::int32_t tz_clz_{-1};
    std::uint8_t tz_src_{};
    void e_dp2(const Inst& d);
    void e_dp3(const Inst& d);
    Val shifted(unsigned rm, unsigned type, unsigned amount, unsigned bits);
    Val extended(unsigned rm, unsigned option, unsigned bits);
    Val shl_val(const Val& v, unsigned amount, unsigned bits);
    void compare(unsigned bits, const Val& n, const Val& m);
    void alu_imm(x64::Alu op, unsigned bits, Gp d, std::uint64_t imm);
    void shift_imm(x64::Shift op, unsigned bits, Gp d, Gp s, unsigned amount);
    void add_value(unsigned bits, Gp d, const Val& n, const Val& m, unsigned scale_log2);
    void sub_value(unsigned bits, Gp d, const Val& n, const Val& m, bool m_is_temp = false);
    void field_extract(unsigned bits, Gp d, Gp n, unsigned lsb, unsigned len, bool sign);
    void field_insert_zero(unsigned bits, Gp d, Gp n, unsigned pos, unsigned len, bool sign);

    // ---- loads/stores (emit_load_store.cpp) -------------------------------------
    void e_load_store(const Inst& d);
    void e_load_store_pair(const Inst& d);
    void e_load_literal(const Inst& d);
    void access(const Inst& d, Gp value, const Mem& m, unsigned size, MemOp op, bool sf);
    static bool disp8(const Mem& m);
    Gp zero_reg();
    // ---- address folding / extension knowledge (core_isel.hpp; additive) ----
    // A cached unpinned guest value as a borrowed register operand (else v).
    Val resolve(const Val& v);
    // [base + extend(rm) << amount + imm] of an AddrMode::reg access.
    Mem reg_address(const Inst& d, Gp base);
    // ---- folded SIMD&FP addresses (neon; additive; emit_integer.cpp) ----
    // [base + extend(Xrm) << shift + disp] (shift 0..4); UXTW/SXTW facts of
    // Xrm as before instruction `fact_index`.
    Mem index_address(Gp base, unsigned rm, unsigned option, unsigned shift, std::int32_t disp,
                      std::int32_t fact_index);
    // ADD/SUB Xd whose result only feeds the base of the next family access
    // (FamilyEmitter::address_base) and is dead after it: the access is
    // emitted inside this instruction with Xd = Xn + (ext(Xm) << amount) +
    // imm folded into EmitContext::address(); its fault sites report the
    // ADD's pc with Xd unchanged (as for the integer fold). Returns the guest
    // instructions consumed (0: not folded, nothing emitted).
    unsigned fold_family_access(const Inst& d);
    struct AddrFold {
        bool active{};
        std::uint8_t rd{}, rn{}, rm{32}, option{3}, amount{};
        std::int32_t imm{};
        std::int32_t index{-1};   // the ADD's instruction index
    };
    AddrFold afold_{};
    std::uint64_t fault_pc_{};    // guest pc of fault sites (the ADD's in a folded pair)
    Gp fold_value();              // the folded Xd value in a temp (an access reading Xd otherwise)
    [[nodiscard]] bool folded(unsigned index) const { return afold_.active && index == afold_.rd; }

    // ---- branches (emit_branch.cpp) ---------------------------------------------
    void e_branch(const PlanInst& pi);
    void e_branch_cond(const PlanInst& pi);
    void e_compare_branch(const PlanInst& pi);
    void e_test_branch(const PlanInst& pi);
    void e_call(const PlanInst& pi);
    void e_indirect(const PlanInst& pi);
    void emit_indirect_jump();   // target in rcx, state flushed
    void e_trap(const PlanInst& pi);
    // ---- switch dispatch (Plan::tables; emit_branch.cpp) ----
    void e_table_branch(const PlanInst& pi);
    // Label for the multiway edge from the current (empty-cache, flags in
    // ctx) state into leader `target`: the block, a pending edge, or an edge
    // stub that loads the target's entry state (no inline edge code).
    x64::Label multiway_label(std::int32_t target);
    // Host jump tables emitted after the stubs: entry k = {int32 guest target
    // - region start, int32 host label - table}; host offsets are patched
    // after finalize.
    struct HostTable {
        x64::Label label;
        std::int32_t table;              // Plan::tables index
        std::uint32_t offset{};          // code offset of the data
        std::vector<x64::Label> entries;
        std::vector<std::int32_t> guest_rel;
    };
    std::vector<HostTable> host_tables_;
    void emit_host_tables();
    void patch_host_tables();

    // ---- vector value facts (EmitContext::vec_fact; neon fusion) ----------------
    // Before emission, prepare_facts() asks the families what their fusions
    // want to know (FamilyFacts::want) and collects each register's reaching
    // definitions (backwards over the region's edges; a region entry, an
    // interpret exit or a conservative instruction on the way defeats it).
    // Constant or compare definitions alone prove a fact (exact). Otherwise
    // every definition writes one 16-bit flag per (register, value) in the
    // context spill area, right after it: a check of the loaded value
    // (FamilyFacts::check) for a 128-bit load, a constant store for a MOVI.
    // The flag then describes the definition that executed last, which is
    // the one the register holds at the fusion.
    struct FactUse {
        std::int32_t index;               // instruction asking (words[0] of the fusion)
        std::uint8_t vec;
        std::uint8_t kind;                // 0 = value (exact), 1 = value (flag), 2 = lane masks (exact), 3 = derived (tier 2)
        std::uint16_t flag;               // kind 1: index into fact_flags
        VecValue value;
    };
    struct FactFlag {                     // one per (register, value)
        std::uint8_t vec;
        bool handed_out;                  // a fusion relied on it
        VecValue value;
    };
    struct FactCheck {                    // a flag write after a definition
        std::int32_t def;
        std::uint16_t flag;
        std::uint8_t kind;                // 0 = check the loaded value, 1 = store "equal", 2 = store "different", 3 = derive
        bool emitted;
        const FamilyFacts* facts;
    };
    void prepare_facts();
    void emit_fact_checks(std::int32_t index, unsigned consumed);
    // Reaching definitions of V`vec` at insts[index] (at most kMaxDefs), false if unknown.
    static constexpr unsigned kMaxDefs = 16;
    bool reaching_defs(std::int32_t index, unsigned vec, std::int32_t* defs, unsigned& count);
    static x64::Mem fact_flag(unsigned flag);
    bool facts_on_{};
    bool facts_helper_{};                 // a state-writing helper call was emitted (flags are not trusted then)

    const Plan& plan_;
    const TranslateOptions& options_;
    TranslatedRegion& out_;
    x64::Assembler a_;

    // per-instruction state
    std::int32_t index_{};
    const PlanInst* cur_{};
    std::uint64_t pc_{};
    std::uint32_t free_gpr_{};
    std::uint32_t free_xmm_{};
    // Tier 2 pin lending (region_alloc.cpp): pins whose host the register
    // cache uses at the current point (GPRs low, vectors high) and those hosts.
    std::uint64_t lent_{};
    std::uint32_t lent_hosts_{}, lent_vhosts_{};
    void set_lent(std::uint64_t lent);
    [[nodiscard]] std::uint64_t lent_at(std::int32_t index) const {
        return index >= 0 && static_cast<std::size_t>(index) < s_.lent.size() ? s_.lent[static_cast<std::size_t>(index)] : 0;
    }
    void lend_enter(std::uint64_t from, std::uint64_t to);   // newly lent pins to the context (before the cache reconciles)
    void lend_leave(std::uint64_t from, std::uint64_t to);   // returned pins back into their hosts (after)
    // Hosts the register cache may use at instruction `index` (GPRs low, xmm high).
    [[nodiscard]] std::uint64_t cache_hosts_at(std::int32_t index) const;
    static void restrict_hosts(RegCache& c, std::uint32_t hosts);
    std::uint32_t written_{};
    std::uint32_t vwritten_{};
    std::uint32_t allowed_writes_{};
    bool family_mode_{};
    bool cur_reads_flags_{};
    bool cur_writes_flags_{};
    bool live_out_{};
    struct PendingGpr { std::uint8_t guest; Gp host; };
    struct PendingVec { std::uint8_t guest; x64::Xmm host; };
    PendingGpr pending_gpr_[8]{};
    unsigned pending_gpr_count_{};
    PendingVec pending_vec_[8]{};
    unsigned pending_vec_count_{};
    bool fault_open_{};
    std::uint32_t fault_begin_{};
    FlagsLocation fault_location_{};
    std::uint16_t fault_map_{};
    bool fault_maps_{true};       // dirty cache entries are described at fault sites (no flush)
    // register cache (block-local)
    RegCache gc_{};
    RegCache vc_{};
    std::uint32_t cache_clock_{};
    std::uint32_t cf_mark_{};
    bool cache_on_{true};
    bool write_back_{true};
    bool fault_flush_{true};      // false only for the wbunsafe measurement knob

    // region state
    FlagsState flags_{FlagsState::ctx};
    // fp/neon (additive): guest V registers whose bits above 32 / 64 are known
    // zero on the current straight-line path (see EmitContext::vec_zero_above).
    std::uint32_t vzero32_{};
    std::uint32_t vzero64_{};
    // p3 (Plan::vzero_join): the facts survive labels. After every emitted
    // instruction the facts are recorded with the V registers its block wrote
    // so far (vzero_record). A block starts with the AND of its predecessors'
    // facts (vzero_block_start): of those emitted earlier in this pass, or, in
    // tier 2's pass 2, the fixpoint over all of them from pass 1's records
    // (vzero_solve: loop-invariant values keep their facts across back
    // edges). Region entries start with none, as does a block with an
    // unrecorded predecessor (one inside a fused span).
    void vzero_prepare();
    void vzero_solve();
    void vzero_record(std::int32_t last);
    void vzero_block_start(std::int32_t index);
    void vzero_finish();
    std::uint32_t vzwritten_{};   // V registers written since the block start (or forgotten)
    std::int32_t vzblock_{-1};    // the current block's leader (-1: region start)
    bool vzsolved_{};             // pass 2: s_.vz_in32/64 hold the block starts' facts
    // Per-thread reusable storage (no per-region allocations).
    struct CacheSnapshot {
        RegCache gpr;
        RegCache vec;
    };
    struct LoopInfo {
        std::uint32_t writes, vwrites;     // anywhere in the loop
        std::uint32_t exposed, vexposed;   // read before written (layout order)
        std::uint16_t weight[32], vweight[32];   // instructions reading each register
    };
    struct Scratch {
        std::vector<FlagsState> inherited;
        std::vector<x64::Label> labels;
        std::vector<x64::Label> entry_labels;
        std::vector<Stub> stubs;
        std::vector<std::uint8_t> entry_inline;   // entry slot -> its stub was laid out in the body
        // Entry state of each leader (index -> snapshot slot, -1 = undecided)
        // and other snapshots (stubs).
        std::vector<std::int32_t> cache_slot;
        std::vector<CacheSnapshot> cache_snaps;
        std::vector<std::int32_t> loop_end;   // loop header -> last back-edge source (else -1)
        std::vector<std::int32_t> loop_of;    // innermost loop header whose layout range holds the index (else -1)
        std::vector<std::int32_t> loop_slot;  // loop header -> loops index (computed on demand)
        std::vector<LoopInfo> loops;
        // Forward edges into merge points not decided yet (per target, linked).
        struct PendingEdge {
            x64::Label label;
            std::int32_t snap;
            std::int32_t from;
            FlagsState flags;
            bool materialize;
            std::int32_t next;
        };
        std::vector<PendingEdge> pend;
        std::vector<std::int32_t> pend_head;
        // vector value facts (prepare_facts)
        std::vector<FactUse> fact_uses;       // sorted by index
        std::vector<FactFlag> fact_flags;
        std::vector<FactCheck> fact_checks;
        struct Derived {                      // tier 2: a derived value slot (vec_derived)
            std::uint8_t vec, kind;
            bool handed_out;
        };
        std::vector<Derived> derived;
        std::vector<std::int32_t> pred_head, pred_next, pred_from;   // predecessor lists (all internal edges)
        std::vector<std::uint32_t> visit;     // reaching_def visit stamps
        std::uint32_t visit_stamp{};
        // tier 2 (region_alloc.cpp)
        std::vector<std::uint64_t> olive_in, olive_out, rlive_in;
        std::vector<std::uint8_t> depth;      // loop nesting depth
        std::vector<std::uint8_t> no_check;   // loop header whose interrupt check another header's covers
        std::vector<std::uint64_t> lent;      // pins lent to the register cache at each instruction
        struct EdgeRec {
            std::int32_t from, to, snap;
        };
        std::vector<EdgeRec> edge_recs;       // pass 1 (cleared by run)
        std::vector<CacheSnapshot> presets;   // pass 2 entry states (built between the passes)
        std::vector<std::int32_t> preset_of;  // leader -> presets index, -1
        // p3: V zero-above facts after each instruction (this pass; pass 1's
        // kept for pass 2), predecessor lists, entry blocks (vzero_*).
        std::vector<std::uint32_t> vz32, vz64, vz1_32, vz1_64, vz_w, vz1_w, vz_in32, vz_in64;
        std::vector<std::int32_t> vz_blk, vz1_blk;
        std::vector<std::uint8_t> vz_rec, vz1_rec, vz_entry;
        std::vector<std::int32_t> vz_pred_head, vz_pred_next, vz_pred_from;
    };
    // ---- cross-block register cache --------------------------------------------
    // Every leader has an entry state (s_.cache_slot -> s_.cache_snaps): the
    // cache entries (guest -> scratch register, dirty bits) its code assumes.
    // The first edge into a leader decides it (a single predecessor hands over
    // its whole cache; merge points keep entries read soon, loop headers the
    // loop's registers, and their preheader edge may load more); every other
    // edge reconciles its state to it (stores, register moves, loads). Dirty
    // entries thus stay in registers across blocks; exits, helpers and stubs
    // store them, fault sites describe them in fault maps. Without carry_
    // (TRANSMUTE_FX_CORE=nocarry) dirty entries are stored before every
    // branch and only single-predecessor blocks inherit the cache.
    std::int32_t push_snap();                     // snapshot of the current cache
    void spill_snap(std::int32_t snap);           // stores a snapshot's dirty entries (stubs)
    void edge_cache(std::int32_t target, bool both_paths);
    // `dead` (GPRs low, vectors high): guest registers no path from the edge
    // observes before overwriting them (tier 2, dead_at): their dirty
    // entries are dropped instead of stored.
    static bool reconcile_needs_code(const CacheSnapshot& from, const CacheSnapshot& to, std::uint64_t dead = 0);
    void decide_entry(std::int32_t target, bool with_current);
    void merge_entry(std::int32_t target, bool with_current, CacheSnapshot& e);
    [[nodiscard]] bool is_pending_target(std::int32_t target) const;
    x64::Label pending_edge(std::int32_t target, std::int32_t from, FlagsState state);
    void reconcile(const CacheSnapshot& e, bool keep_rest, std::uint64_t dead = 0);
    void reconcile_class(RegCache& c, const RegCache& t, bool vec, bool keep_rest, std::uint32_t dead);
    void load_entry_state(std::int32_t index);   // entry stubs: the block's cache entries from the context
    const LoopInfo& loop_info(std::int32_t header);
    void jump_edge(std::int32_t target, std::int32_t from, bool fall_through);
    bool carry_{true};
    bool carry_clean_{};   // carried entries are always clean (dirty ones stored before every branch)
    std::int32_t bool_reader_{-1};   // e_ccmp fusion: this reader's condition() is NE
    bool bool_in_slot_{};             // e_ccmp fusion: the answer is parked in the context (park_bool)
    void park_bool();
    bool flags_saved_{};   // save_flags_around: host flags saved in ah/al until end_instruction
    bool branch_dirty_{};
    bool merge_dirty_{};   // opt-in: dirty entries cross forward edges into merge points (dirty_into_merge)  // dirty entries cross branches into single-predecessor blocks (dirty_crosses_branch)

    // ---- tier 2: region-wide analyses (region_alloc.cpp) ------------------------
    // Liveness over the region's edges (s_.olive_in/out, s_.rlive_in; GPRs in
    // the low 32 bits, vectors in the high 32): "observed" = read, or at an
    // observation point (anything that may fault or leave the region, a kept
    // interrupt check, an ADD that a fault of the next access reports)
    // before being overwritten on some path; "read" = read by a guest
    // instruction or at an exit. Loop depth, dropped interrupt checks.
    void compute_tier2();
    // True if the current value of the guest register may be observed from
    // the current point (always true without Plan::t2_dead).
    [[nodiscard]] bool observed(unsigned g, bool vec) const;
    [[nodiscard]] std::uint64_t dead_at(std::int32_t target) const;   // ~olive_in[target] (0 without t2_dead)
    void record_edges(std::int32_t from, std::int32_t only = -1);     // pass 1: the cache at edges from `from`
    unsigned alloc_pass_{};
    // Tier-up from loops (Plan::count_loop_headers): every loop header of a
    // counted first-tier region does `dec dword [rip + slot]; jz`, one counter
    // per region in a cache line of its own at the end of the code (no code
    // shares it); when it runs out the region exits with tier_up_loop.
    bool loop_counting_{};
    std::vector<std::uint32_t> loop_counter_fields_;
    bool dead_on_{};
    bool in_instruction_{};
    std::int32_t after_last_{};   // last word of the instruction just emitted (olive_out between instructions)
    static Scratch& scratch();
    Scratch& s_;
    std::vector<FlagsState>& inherited_;
    std::vector<x64::Label>& labels_;
    std::vector<x64::Label>& entry_labels_;
    std::vector<Stub>& stubs_;
    std::int32_t skip_until_{};
};

} // namespace transmute::fx::core
