#include "fx/translator/region_emitter.hpp"

#include "fx/common/cpu_features.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

// NEON per-region constant pool (emit_neon.hpp). Weak references: when
// fx_emit_neon is not linked these resolve to no-op defaults, so the core
// does not depend on the neon library.
namespace transmute::fx {
void neon_begin_region(x64::Assembler& as);
void neon_end_region(x64::Assembler& as);
void neon_region_hook_absent(x64::Assembler&) {}
} // namespace transmute::fx
#pragma comment(linker, "/alternatename:?neon_begin_region@fx@transmute@@YAXAEAVAssembler@x64@12@@Z=?neon_region_hook_absent@fx@transmute@@YAXAEAVAssembler@x64@12@@Z")
#pragma comment(linker, "/alternatename:?neon_end_region@fx@transmute@@YAXAEAVAssembler@x64@12@@Z=?neon_region_hook_absent@fx@transmute@@YAXAEAVAssembler@x64@12@@Z")

namespace transmute::fx::core {

bool neon_region_hooks_linked() {
    return reinterpret_cast<const void*>(&neon_begin_region) != reinterpret_cast<const void*>(&neon_region_hook_absent);
}

std::vector<InstructionMapEntry>*& instruction_map_sink() {
    thread_local std::vector<InstructionMapEntry>* sink = nullptr;
    return sink;
}

using x64::Alu;
using x64::Cond;
using x64::Gp;
using x64::Label;
using x64::Mem;
using x64::ptr;

namespace {

constexpr Gp kScratch[] = {Gp::r8, Gp::r9, Gp::r10, Gp::r11, Gp::rcx, Gp::rdx, Gp::rax};
constexpr std::uint32_t kScratchMask = (1u << 8) | (1u << 9) | (1u << 10) | (1u << 11) | (1u << 1) | (1u << 2) | 1u;
constexpr unsigned kRaxSaveSlot = 63;
constexpr unsigned kFcmpConstSlot = 62;
constexpr unsigned kHelperArgSlot = 56;
constexpr unsigned kLoopCounterSlot = 55;   // tier 2: per-thread loop iteration counter (counted code)
constexpr unsigned kBoolSlot = 61;          // e_ccmp fusion: the parked answer (park_bool)
constexpr std::int32_t kEvictLookahead = 64;    // instructions scanned for a cache entry's next use

constexpr Cond kSubCond[14] = {Cond::e, Cond::ne, Cond::ae, Cond::b, Cond::s, Cond::ns, Cond::o,
                               Cond::no, Cond::a, Cond::be, Cond::ge, Cond::l, Cond::g, Cond::le};
// add/logic producer: C = CF. HI/LS have no direct mapping (handled separately).
constexpr Cond kAddCond[14] = {Cond::e, Cond::ne, Cond::b, Cond::ae, Cond::s, Cond::ns, Cond::o,
                               Cond::no, Cond::a, Cond::be, Cond::ge, Cond::l, Cond::g, Cond::le};

std::uint32_t cond_mask(unsigned cond) {
    std::uint32_t mask = 0;
    for (unsigned v = 0; v < 16; ++v)
        if (condition_holds(cond, v << 28)) mask |= 1u << v;
    return mask;
}

constexpr std::int32_t kNzcvDisp = ctx_disp(offsetof(GuestContext, nzcv));
constexpr std::int32_t kPcDisp = ctx_disp(offsetof(GuestContext, pc));
// Loops whose layout range (header to last back-edge source) holds fewer
// guest instructions keep their 16-byte header alignment (Plan::cold_layout).
constexpr std::int32_t kSmallLoop = 24;

} // namespace

RegionEmitter::Scratch& RegionEmitter::scratch() {
    thread_local Scratch s;
    return s;
}

RegionEmitter::RegionEmitter(const Plan& plan, const TranslateOptions& options, TranslatedRegion& out)
    : plan_(plan), options_(options), out_(out), a_(plan.insts.size() * 24 + 256), s_(scratch()),
      inherited_(s_.inherited), labels_(s_.labels), entry_labels_(s_.entry_labels), stubs_(s_.stubs) {
    stubs_.clear();
    const auto n = plan.insts.size();
    a_.reserve_labels(n + plan.entries.size() + n / 2 + 16, n + n / 2 + 16);
    out_.faults.reserve(n);
    out_.fixups.reserve(n / 4 + 4);
    out_.exits.reserve(n / 4 + 4);
    out_.entries.reserve(plan.entries.size());
    out_.pins.reserve(8);
    out_.guest_ranges.reserve(4);
}

// ---- scratch registers ------------------------------------------------------

void RegionEmitter::reset_scratch() {
    free_gpr_ = (kScratchMask & ~plan_.pin_hosts) | lent_hosts_;   // tier 2: lent pin hosts too
    free_xmm_ = (0xffffu & ~plan_.vpin_hosts) | lent_vhosts_;
}

// Cost of evicting the cache entry of guest `g` now (lower = better victim):
// bits 48+: 2 for a store (dirty) + 2 for a reload in this block (read
// before written), or 1 if only read after the block (layout lookahead);
// then the distance to that read (further = better) and the LRU stamp.
std::uint64_t RegionEmitter::evict_cost(unsigned g, bool dirty, bool vec, std::uint32_t stamp) const {
    if (dirty && !observed(g, vec)) dirty = false;   // tier 2: dropped without a store (evict_gpr/evict_vec)
    if (!belady_) return (dirty ? 1ull << 48 : 0) | stamp;
    const auto bit = 1u << g;
    unsigned reload = 0, distance = 255;
    if (cur_ && ((vec ? cur_->vreads : cur_->reads) & bit)) {   // this instruction may still read it
        reload = 2;
        distance = 0;
    } else {
        // The lookahead depends only on the instruction and the register:
        // computed once per (instruction, register) (several allocations of
        // an instruction compare the same candidates).
        if (next_use_index_ != index_) {
            next_use_index_ = index_;
            next_use_known_ = 0;
        }
        const auto k = g + (vec ? 32u : 0u);
        if ((next_use_known_ >> k) & 1u) {
            reload = next_use_reload_[k];
            distance = next_use_distance_[k];
        } else {
            const auto n = static_cast<std::int32_t>(plan_.uses.size());
            const PlanUse* uses = plan_.uses.data();
            bool in_block = true;
            for (std::int32_t j = index_ + 1; j < n && j <= index_ + kEvictLookahead; ++j) {
                const auto& u = uses[j];
                if (u.bits & kUseLeader) in_block = false;
                if (u.bits & kUseOpaque) break;
                if ((vec ? u.vreads : u.reads) & bit) {
                    reload = in_block ? 2 : 1;
                    distance = static_cast<unsigned>(j - index_);
                    break;
                }
                if ((vec ? u.vwrites : u.writes) & bit) break;   // overwritten first: dead here
                if (!(u.bits & kUseStraight)) break;
            }
            next_use_known_ |= std::uint64_t{1} << k;
            next_use_reload_[k] = static_cast<std::uint8_t>(reload);
            next_use_distance_[k] = static_cast<std::uint8_t>(distance);
        }
    }
    const std::uint64_t cost = (dirty ? 2u : 0u) + reload;
    return (cost << 48) | (static_cast<std::uint64_t>(255 - std::min(distance, 255u)) << 32) | stamp;
}

// Free registers without a cache entry first (pool order), then the cached
// one cheapest to give up (evict_cost; LRU among clean, then dirty, without
// belady_), stored first if dirty.
Gp RegionEmitter::alloc() {
    Gp victim = Gp::none;
    std::uint64_t oldest = UINT64_MAX;
    const auto consider = [&](Gp r) {
        if (!is_free(r)) return false;
        const auto h = x64::id(r);
        if (!((gc_.mask >> h) & 1u)) {
            free_gpr_ &= ~(1u << h);
            victim = r;
            return true;
        }
        std::uint64_t cost =
            evict_cost(static_cast<unsigned>(gc_.guest[h]), ((gc_.dirty >> h) & 1u) != 0, false, gc_.used[h]);
        if (galias_of_[h])   // tier 2: its copy aliases lose the host too (a store each if dirty)
            cost += std::uint64_t{2u * static_cast<unsigned>(std::popcount(galias_of_[h] & galias_dirty_)) +
                                  static_cast<unsigned>(std::popcount(galias_of_[h]))} << 48;
        if (cost < oldest) {
            oldest = cost;
            victim = r;
        }
        return false;
    };
    for (const auto r : kScratch)
        if (consider(r)) return r;
    for (auto m = lent_hosts_; m; m &= m - 1)   // tier 2: hosts of pins lent inside this loop
        if (consider(x64::gp(static_cast<unsigned>(std::countr_zero(m))))) return victim;
    if (victim == Gp::none) throw RetryTranslation{pc_, false, RetryTranslation::Resource::gpr};
    evict_gpr(x64::id(victim));
    free_gpr_ &= ~(1u << x64::id(victim));
    return victim;
}

void RegionEmitter::take(Gp r) {
    if (r == Gp::rax && flags_saved_) materialize();   // save_flags_around, then an implicit rax user
                                                           // (SMULH/UMULH): the flags go to ctx->nzcv after all
    if (!is_free(r)) throw std::logic_error("RegionEmitter::take: register in use");
    evict_gpr(x64::id(r));
    free_gpr_ &= ~(1u << x64::id(r));
}

void RegionEmitter::take_value(Gp r, const Val& v) {
    const auto h = x64::id(r);
    if (v.is_mem() && gc_.host[v.g] == static_cast<std::int8_t>(h) && is_free(r) && galias_of_[h] == 0) {
        if ((gc_.dirty >> h) & 1u) a_.mov(64, gmem(v.g), r);   // the context gets the value too
        cache_drop_host(gc_, h);
        free_gpr_ &= ~(1u << h);
        return;
    }
    take(r);
    if (v.is_zero()) a_.mov_imm(r, 0);
    else load(64, r, v);
}

x64::Xmm RegionEmitter::alloc_vec() {
    if (free_xmm_ == 0) throw RetryTranslation{pc_, false, RetryTranslation::Resource::vec};
    const auto uncached = free_xmm_ & ~vc_.mask;
    unsigned n;
    if (uncached != 0) {
        n = static_cast<unsigned>(std::countr_zero(uncached));
    } else {
        std::uint64_t oldest = UINT64_MAX;
        n = 0;
        for (auto m = free_xmm_; m; m &= m - 1) {
            const auto h = static_cast<unsigned>(std::countr_zero(m));
            std::uint64_t cost =
                evict_cost(static_cast<unsigned>(vc_.guest[h]), ((vc_.dirty >> h) & 1u) != 0, true, vc_.used[h]);
            // tier 2: the host's aliases lose their value too (a store each if dirty).
            if (valias_of_[h]) cost += std::uint64_t{2u * static_cast<unsigned>(std::popcount(valias_of_[h] & valias_dirty_)) +
                                                     static_cast<unsigned>(std::popcount(valias_of_[h]))} << 48;
            if (cost < oldest) {
                oldest = cost;
                n = h;
            }
        }
        evict_vec(n);
    }
    free_xmm_ &= ~(1u << n);
    vslot_[n] = 0;   // a new value
    return x64::xmm(n);
}

// ---- register cache -------------------------------------------------------------

void RegionEmitter::reset_cache(RegCache& c) {
    std::memset(c.host, -1, sizeof c.host);
    std::memset(c.guest, -1, sizeof c.guest);
    c.mask = 0;
    c.dirty = 0;
}

void RegionEmitter::cache_clear() {
    // Every helper call is reached with the cache flushed.
    if (gc_.dirty | vc_.dirty) throw RetryTranslation{pc_, false, RetryTranslation::Resource::writeback};
    alias_resolve();   // tier 2: flush_dirty stored them already
    std::memset(galias_host_, -1, sizeof galias_host_);
    std::memset(galias_of_, 0, sizeof galias_of_);
    galias_count_ = 0;
    galias_dirty_ = 0;
    reset_cache(gc_);
    reset_cache(vc_);
    alias_forget_knowledge();
}

void RegionEmitter::drop_entry(RegCache& c, unsigned h) {
    if (!((c.mask >> h) & 1u)) return;
    c.host[static_cast<unsigned>(c.guest[h])] = -1;
    c.guest[h] = -1;
    c.mask &= ~(1u << h);
    c.dirty &= ~(1u << h);
}

void RegionEmitter::cache_drop_host(RegCache& c, unsigned h) {
    const bool was_dirty = ((c.mask & c.dirty) >> h) & 1u;
    drop_entry(c, h);
    if (was_dirty) fault_reopen();   // the open fault site's map named h
}

void RegionEmitter::cache_drop_guest(RegCache& c, unsigned g) {
    const auto h = c.host[g];
    if (h < 0) return;
    cache_drop_host(c, static_cast<unsigned>(h));
}

void RegionEmitter::cache_set(RegCache& c, unsigned g, unsigned h) {
    if (!cache_on_) return;
    cache_drop_guest(c, g);   // a dirty older value of g is dead: overwritten
    cache_drop_host(c, h);
    c.host[g] = static_cast<std::int8_t>(h);
    c.guest[h] = static_cast<std::int8_t>(g);
    c.used[h] = ++cache_clock_;
    c.mask |= 1u << h;
}

void RegionEmitter::evict_gpr(unsigned h) {
    if (galias_of_[h]) galias_drop_host(h);   // tier 2: its copy aliases lose the host too
    // Tier 2: a dirty value nothing observes again (overwritten first on
    // every path) is dropped without a store.
    if (((gc_.dirty >> h) & 1u) && !observed(static_cast<unsigned>(gc_.guest[h]), false)) gc_.dirty &= ~(1u << h);
    if ((gc_.dirty >> h) & 1u) {
        if (a_.control_flow_count() != cf_mark_)   // the store would run on one path only
            throw RetryTranslation{pc_, false, RetryTranslation::Resource::writeback};
        a_.mov(64, gmem(static_cast<unsigned>(gc_.guest[h])), x64::gp(h));
    }
    cache_drop_host(gc_, h);
}

void RegionEmitter::evict_vec(unsigned h) {
    if (valias_of_[h]) alias_drop_host(h);   // tier 2: its aliases lose the host too
    if (((vc_.dirty >> h) & 1u) && !observed(static_cast<unsigned>(vc_.guest[h]), true)) vc_.dirty &= ~(1u << h);
    if ((vc_.dirty >> h) & 1u) {
        if (a_.control_flow_count() != cf_mark_)
            throw RetryTranslation{pc_, false, RetryTranslation::Resource::writeback};
        slot_aliases_of(static_cast<unsigned>(vc_.guest[h]));   // tier 2: they read the old slot
        vmove(ptr(Gp::r15, v_disp(static_cast<unsigned>(vc_.guest[h]))), x64::xmm(h));
        slot_stored(static_cast<unsigned>(vc_.guest[h]), h);
    }
    cache_drop_host(vc_, h);
}

void RegionEmitter::flush_dirty(bool edge) {
    if (!(gc_.dirty | vc_.dirty | valias_dirty_ | galias_dirty_) && slot_alias_count_ == 0) return;
    if (!edge && a_.control_flow_count() != cf_mark_)   // inside an instruction's internal paths: the stores
        throw RetryTranslation{pc_, false, RetryTranslation::Resource::writeback};   // would not cover all
    slot_aliases_all();   // tier 2: before any slot they read is overwritten
    for (auto m = gc_.dirty; m; m &= m - 1) {
        const auto h = static_cast<unsigned>(std::countr_zero(m));
        a_.mov(64, gmem(static_cast<unsigned>(gc_.guest[h])), x64::gp(h));
    }
    for (auto m = vc_.dirty; m; m &= m - 1) {
        const auto h = static_cast<unsigned>(std::countr_zero(m));
        vmove(ptr(Gp::r15, v_disp(static_cast<unsigned>(vc_.guest[h]))), x64::xmm(h));
        slot_stored(static_cast<unsigned>(vc_.guest[h]), h);
    }
    for (unsigned h = 0; h < 16 && valias_dirty_; ++h)   // tier 2: dirty aliases (they stay, clean)
        if (valias_of_[h] & valias_dirty_) alias_store_dirty(h);
    for (auto m = galias_dirty_; m; m &= m - 1) {   // tier 2: dirty GPR copy aliases (they stay, clean)
        const auto g = static_cast<unsigned>(std::countr_zero(m));
        a_.mov(64, gmem(g), x64::gp(static_cast<unsigned>(galias_host_[g])));
    }
    galias_dirty_ = 0;
    gc_.dirty = 0;
    vc_.dirty = 0;
    fault_reopen();
}

// Tier 2: adds to keep_g/keep_v the hosts whose dirty entry `e` keeps dirty
// in the same host.
void RegionEmitter::keep_matching(const CacheSnapshot& e, std::uint32_t& keep_g, std::uint32_t& keep_v) const {
    for (auto m = gc_.dirty; m; m &= m - 1) {
        const auto h = static_cast<unsigned>(std::countr_zero(m));
        if (e.gpr.host[static_cast<unsigned>(gc_.guest[h])] == static_cast<std::int8_t>(h) && ((e.gpr.dirty >> h) & 1u))
            keep_g |= 1u << h;
    }
    for (auto m = vc_.dirty; m; m &= m - 1) {
        const auto h = static_cast<unsigned>(std::countr_zero(m));
        if (e.vec.host[static_cast<unsigned>(vc_.guest[h])] == static_cast<std::int8_t>(h) && ((e.vec.dirty >> h) & 1u))
            keep_v |= 1u << h;
    }
}

// Tier 2: the loop-carried dirty values at instruction `index`: those the
// entry state of its innermost loop's header (a pass-2 preset) keeps dirty
// in the same host. They stay in registers inside the loop and are stored
// on the way out of it.
void RegionEmitter::carried_dirty(std::int32_t index, std::uint32_t& keep_g, std::uint32_t& keep_v) const {
    if (alloc_pass_ != 2 || index < 0) return;
    const auto h = s_.loop_of[static_cast<std::size_t>(index)];
    if (h < 0 || static_cast<std::size_t>(h) >= s_.preset_of.size()) return;
    const auto slot = s_.cache_slot[static_cast<std::size_t>(h)];
    if (s_.preset_of[static_cast<std::size_t>(h)] < 0 || slot < 0) return;
    keep_matching(s_.cache_snaps[static_cast<std::size_t>(slot)], keep_g, keep_v);
}

bool RegionEmitter::dirty_crosses_branch(std::int32_t index) const {
    if (!branch_dirty_ || !carry_ || !write_back_ || !fault_maps_) return false;
    const auto& pi = plan_.insts[static_cast<std::size_t>(index)];
    if (pi.flow != Flow::cond || pi.table >= 0) return false;
    const auto k = pi.inst.kind;
    if (k != Kind::branch_cond && k != Kind::compare_branch && k != Kind::test_branch) return false;
    const auto single = [&](std::int32_t t) {
        if (t < 0) return true;   // an exit: its stub stores the dirty snapshot
        const auto& s = plan_.insts[static_cast<std::size_t>(t)];
        if (lent_at(t) != lent_) return false;
        if (!s.leader) return t == index + 1;   // the fall-through continues the block with the cache as it is
        return (t > index && s.inherit_flags && s.first_pred == index) || dirty_into_merge(t);
    };
    return single(pi.taken) && single(pi.fall);
}

// Opt-in TRANSMUTE_FX_CORE=mergedirty: a forward edge into a merge point
// outside loops whose entry state is not decided yet carries its dirty
// entries too. The merge keeps those every predecessor holds in the same
// host (dirty if dirty in any); the edges store the rest (edge stubs for
// jumps, inline on the fall-through), instead of every predecessor storing
// everything first.
bool RegionEmitter::dirty_into_merge(std::int32_t t) const {
    if (!merge_dirty_ || !carry_ || !write_back_ || !fault_maps_ || alloc_pass_ == 2 || t <= index_) return false;
    const auto& s = plan_.insts[static_cast<std::size_t>(t)];
    return s.leader && !s.inherit_flags && !s.entry && !s.loop_header && s_.loop_of[static_cast<std::size_t>(t)] < 0 &&
           s_.loop_of[static_cast<std::size_t>(index_)] < 0 && lent_at(t) == lent_ && is_pending_target(t);
}

// True if `target` (-1: outside the region) is outside the innermost loop of `index`.
bool RegionEmitter::leaves_loop(std::int32_t index, std::int32_t target) const {
    const auto h = s_.loop_of[static_cast<std::size_t>(index)];
    if (h < 0) return false;
    return target < h || target > s_.loop_end[static_cast<std::size_t>(h)];
}

// Tier 2: flush_dirty() except the dirty entries of hosts in keep_g/keep_v;
// entries of guests in `dead` (GPRs low, vectors high) become clean unstored.
void RegionEmitter::flush_dirty_except(std::uint32_t keep_g, std::uint32_t keep_v, std::uint64_t dead) {
    // Before a branch, its operands are still needed even if both successors
    // overwrite them. Materializing NZCV can evict RAX before CBZ/TBZ reads
    // its operand, so its context slot must still contain the current value.
    if (in_instruction_ && cur_) dead &= ~(cur_->reads | (static_cast<std::uint64_t>(cur_->vreads) << 32));
    if (!(gc_.dirty | vc_.dirty | valias_dirty_) && slot_alias_count_ == 0) return;
    if (a_.control_flow_count() != cf_mark_) throw RetryTranslation{pc_, false, RetryTranslation::Resource::writeback};
    slot_aliases_all();
    for (auto m = gc_.dirty & ~keep_g; m; m &= m - 1) {
        const auto h = static_cast<unsigned>(std::countr_zero(m));
        const auto g = static_cast<unsigned>(gc_.guest[h]);
        if (!((dead >> g) & 1u)) a_.mov(64, gmem(g), x64::gp(h));
    }
    for (auto m = vc_.dirty & ~keep_v; m; m &= m - 1) {
        const auto h = static_cast<unsigned>(std::countr_zero(m));
        const auto g = static_cast<unsigned>(vc_.guest[h]);
        if ((dead >> (32 + g)) & 1u) continue;
        vmove(ptr(Gp::r15, v_disp(g)), x64::xmm(h));
        slot_stored(g, h);
    }
    for (unsigned h = 0; h < 16 && valias_dirty_; ++h)
        if (valias_of_[h] & valias_dirty_) alias_store_dirty(h);
    gc_.dirty &= keep_g;
    vc_.dirty &= keep_v;
    fault_reopen();
}

Gp RegionEmitter::cached_gpr(unsigned g) {
    auto h = gc_.host[g];
    if (h < 0) h = galias_host_[g];   // tier 2: a copy alias of another guest's host
    if (h < 0) return Gp::none;
    gc_.used[h] = ++cache_clock_;
    return x64::gp(static_cast<unsigned>(h));
}

Gp RegionEmitter::borrow_gpr(unsigned g) {
    const auto r = cached_gpr(g);
    if (r != Gp::none) free_gpr_ &= ~(1u << x64::id(r));
    return r;
}

Gp RegionEmitter::read_unpinned(unsigned g) {
    if (const auto r = borrow_gpr(g); r != Gp::none) return r;
    const bool cacheable = may_cache();
    const auto t = alloc();
    a_.mov(64, t, gmem(g));
    if (cacheable) cache_set(gc_, g, x64::id(t));
    return t;
}

x64::Xmm RegionEmitter::borrow_vec(unsigned v) {
    auto h = vc_.host[v];
    if (h < 0) h = valias_host_[v];   // tier 2: an alias of another guest's host
    if (h < 0) return static_cast<x64::Xmm>(0xff);
    vc_.used[h] = ++cache_clock_;
    free_xmm_ &= ~(1u << static_cast<unsigned>(h));
    return x64::xmm(static_cast<unsigned>(h));
}

// True if guest vector `v` is read again soon in this block before being
// written (worth keeping in a register rather than folding a memory operand).
bool RegionEmitter::vec_read_soon(unsigned v) const {
    const auto n = static_cast<std::int32_t>(plan_.uses.size());
    const auto bit = 1u << v;
    for (std::int32_t j = index_ + 1; j < n && j <= index_ + 12; ++j) {
        const auto& u = plan_.uses[static_cast<std::size_t>(j)];
        if (u.bits & (kUseLeader | kUseOpaque)) return false;
        if (u.vreads & bit) return true;
        if (u.vwrites & bit) return false;
        if (!(u.bits & kUseNormal)) return false;
    }
    return false;
}

// ---- guest operands -----------------------------------------------------------

RegionEmitter::Val RegionEmitter::val(unsigned index, bool sp) const {
    Val v;
    if (index == 31 && !sp) return v;   // XZR
    v.g = static_cast<std::uint8_t>(index);
    if (pinned(index)) {
        v.k = Val::K::reg;
        v.r = pin(index);
    } else {
        v.k = Val::K::mem;
    }
    return v;
}

void RegionEmitter::zero(Gp d) {
    if (keep_flags() || family_mode_) a_.mov_imm(d, 0);
    else a_.xor_(32, d, d);
}

void RegionEmitter::load(unsigned bits, Gp d, const Val& v) {
    switch (v.k) {
    case Val::K::zero: zero(d); break;
    case Val::K::mem:
        if (const auto h = cached_gpr(v.g); h != Gp::none) {
            if (h != d || bits != 64) a_.mov(bits == 64 ? 64 : 32, d, h);
        } else {
            a_.mov(bits == 64 ? 64 : 32, d, gmem(v.g));
        }
        break;
    case Val::K::reg:
        if (v.r != d || bits != 64) a_.mov(bits == 64 ? 64 : 32, d, v.r);
        break;
    }
}

Gp RegionEmitter::in_reg(const Val& v) {
    if (v.is_reg()) return v.r;
    if (v.is_mem()) return read_unpinned(v.g);
    const auto t = alloc();
    load(64, t, v);
    return t;
}

Gp RegionEmitter::copy_of(const Val& v) {
    const auto t = alloc();
    load(64, t, v);
    return t;
}

void RegionEmitter::alu(Alu op, unsigned bits, Gp d, const Val& v) {
    switch (v.k) {
    case Val::K::zero: a_.alu(op, bits, d, 0); break;
    case Val::K::mem:
        if (const auto h = cached_gpr(v.g); h != Gp::none) a_.alu(op, bits, d, h);
        else a_.alu(op, bits, d, gmem(v.g));
        break;
    case Val::K::reg: a_.alu(op, bits, d, v.r); break;
    }
}

void RegionEmitter::note_write(unsigned g) { written_ |= 1u << g; }

Gp RegionEmitter::dst(unsigned index, bool sp) {
    if (index == 31 && !sp) return alloc();   // XZR: discarded
    note_write(index);
    if (pinned(index)) return pin(index);
    if (pending_gpr_count_ >= std::size(pending_gpr_)) throw RetryTranslation{pc_, false};
    if (const auto h = reusable_gpr(index); h != Gp::none) {   // the guest's own cached host
        free_gpr_ &= ~(1u << x64::id(h));
        pending_gpr_[pending_gpr_count_++] = {static_cast<std::uint8_t>(index), h};
        return h;
    }
    const auto t = alloc();
    pending_gpr_[pending_gpr_count_++] = {static_cast<std::uint8_t>(index), t};
    return t;
}

// A core instruction that overwrites guest `index` without reading it, and
// cannot fault or branch, writes the result into the host register that
// caches the old value (r8-r11 only: rax, rcx and rdx have implicit users
// that take() them later). The guest then stays in one host across
// instructions, so the predecessors of a merge point agree more often (no
// reload there). TRANSMUTE_FX_CORE=nogprreuse: a fresh register each time.
Gp RegionEmitter::reusable_gpr(unsigned index) const {
    if (!codegen_on(knob_no_gpr_reuse) || family_mode_ || !cur_ || !may_cache()) return Gp::none;
    if (((cur_->reads >> index) & 1u) || cur_->may_fault || cur_->conservative_writes || cur_->flow != Flow::normal ||
        cur_->inst.kind == Kind::unknown)
        return Gp::none;
    const auto h = gc_.host[index];
    if (h < 8 || h > 11) return Gp::none;
    const auto r = x64::gp(static_cast<unsigned>(h));
    if (!is_free(r) || galias_of_[static_cast<unsigned>(h)] != 0) return Gp::none;
    return r;
}

Gp RegionEmitter::dst_rmw(unsigned index, bool sp) {
    const auto v = val(index, sp);
    const auto d = dst(index, sp);
    if (!v.is_reg()) load(64, d, v);
    return d;
}

void RegionEmitter::binop(Alu op, unsigned bits, Gp d, const Val& n, const Val& m, bool commutative) {
    if (n.is_reg() && n.r == d) {
        alu(op, bits, d, m);
        return;
    }
    if (m.is_reg() && m.r == d) {
        if (commutative) {
            alu(op, bits, d, n);
            return;
        }
        const auto t = alloc();
        load(bits, t, n);
        alu(op, bits, t, m);
        a_.mov(bits, d, t);
        return;
    }
    load(bits, d, n);
    alu(op, bits, d, m);
}

std::uint64_t RegionEmitter::current_dirty() const {
    // Convention pins are always newer than their context slot (live since
    // the region was entered). Tier 2: lent pins are in the context.
    const std::uint64_t gpr = (((cur_ ? cur_->dirty_in : 0u) | written_) & plan_.pinned) | plan_.globals;
    const std::uint64_t vec = ((cur_ ? cur_->vdirty_in : 0u) | vwritten_) & plan_.vpinned;
    return (gpr | (vec << 32)) & ~lent_;
}

void RegionEmitter::load_pins() {
    for (unsigned g = 0; g < 32; ++g)
        if (pinned(g)) a_.mov(64, pin(g), gmem(g));
    for (unsigned v = 0; v < 32; ++v)
        if (vpinned(v)) vmove(vpin(v), ptr(Gp::r15, v_disp(v)));
}

std::uint32_t RegionEmitter::pins_dead_at_entry(std::int32_t index) const {
    if (!codegen_on(knob_no_dead_pin_loads) || index < 0) return 0;
    const auto n = static_cast<std::int32_t>(plan_.insts.size());
    if (plan_.insts[static_cast<std::size_t>(index)].loop_header) return 0;   // its interrupt stub stores dirty pins
    std::uint32_t want = plan_.pinned & ~plan_.globals & ~static_cast<std::uint32_t>(lent_);
    std::uint32_t dead = 0;
    for (std::int32_t j = index; j < n && j < index + 16 && want != 0; ++j) {
        const auto& pj = plan_.insts[static_cast<std::size_t>(j)];
        if (j != index && pj.leader) break;
        if (pj.family >= 0 || pj.try_family || pj.inst.kind == Kind::unknown || pj.conservative_writes ||
            pj.may_fault || pj.flow != Flow::normal || pj.inline_call || pj.inline_ret)
            break;
        // An ADD/SUB the next access may fold reports a fault there with its own state before it.
        if ((pj.inst.kind == Kind::add_sub_imm || pj.inst.kind == Kind::add_sub_shift || pj.inst.kind == Kind::add_sub_ext) &&
            j + 1 < n && plan_.insts[static_cast<std::size_t>(j) + 1].may_fault)
            break;
        want &= ~pj.reads;
        dead |= pj.writes & want;
        want &= ~pj.writes;
    }
    return dead;
}

void RegionEmitter::entry_pins(std::int32_t index) {
    const auto& conv = translator_register_convention();
    // Tier 2: convention registers lent at this entry go to the context like evicted ones.
    for (auto m = plan_.evicted | (plan_.globals & static_cast<std::uint32_t>(lent_)); m; m &= m - 1) {
        const auto g = static_cast<unsigned>(std::countr_zero(m));
        a_.mov(64, gmem(g), x64::gp(static_cast<unsigned>(conv.host[g])));
    }
    const auto dead = pins_dead_at_entry(index);
    for (unsigned g = 0; g < 32; ++g)
        if (pinned(g) && !global(g) && !((dead >> g) & 1u)) a_.mov(64, pin(g), gmem(g));
    const auto vdead = vpins_dead_at_entry(index);
    for (unsigned v = 0; v < 32; ++v)
        if (vpinned(v) && !((vdead >> v) & 1u)) vmove(vpin(v), ptr(Gp::r15, v_disp(v)));
}

// p3: vector pins that the code at entry `index` overwrites before reading
// them, with no fault, exit, block end or loop header in between (a fault or
// an exit there would take the unloaded pin for the register's value): their
// entry loads are dead. Same scan and knob (nodeadpinloads) as the GPR pins;
// family instructions count through their described vector reads and writes
// (partial writes such as INS read the register too).
std::uint32_t RegionEmitter::vpins_dead_at_entry(std::int32_t index) const {
    if (!codegen_on(knob_no_dead_pin_loads) || index < 0) return 0;
    const auto n = static_cast<std::int32_t>(plan_.insts.size());
    if (plan_.insts[static_cast<std::size_t>(index)].loop_header) return 0;   // its interrupt stub stores dirty pins
    std::uint32_t want = plan_.vpinned & ~static_cast<std::uint32_t>(lent_ >> 32);
    std::uint32_t dead = 0;
    for (std::int32_t j = index; j < n && j < index + 16 && want != 0; ++j) {
        const auto& pj = plan_.insts[static_cast<std::size_t>(j)];
        if (j != index && pj.leader) break;
        if ((pj.family < 0 && (pj.try_family || pj.inst.kind == Kind::unknown)) || pj.conservative_writes ||
            pj.may_fault || pj.flow != Flow::normal || pj.inline_call || pj.inline_ret)
            break;
        // An ADD/SUB the next access may fold reports a fault there with its own state before it.
        if ((pj.inst.kind == Kind::add_sub_imm || pj.inst.kind == Kind::add_sub_shift || pj.inst.kind == Kind::add_sub_ext) &&
            j + 1 < n && plan_.insts[static_cast<std::size_t>(j) + 1].may_fault)
            break;
        want &= ~pj.vreads;
        dead |= pj.vwrites & want;
        want &= ~pj.vwrites;
    }
    return dead;
}

void RegionEmitter::exit_pins(std::uint64_t dirty, bool restore) {
    flush_pins(dirty & ~static_cast<std::uint64_t>(plan_.globals));
    if (!restore) return;
    const auto& conv = translator_register_convention();
    // Tier 2: lent convention registers are in the context, like evicted ones.
    for (auto m = plan_.evicted | (plan_.globals & static_cast<std::uint32_t>(lent_)); m; m &= m - 1) {
        const auto g = static_cast<unsigned>(std::countr_zero(m));
        a_.mov(64, x64::gp(static_cast<unsigned>(conv.host[g])), gmem(g));
    }
}

void RegionEmitter::flush_pins(std::uint64_t dirty) {
    dirty &= ~lent_;   // tier 2: lent pins are in the context (their hosts hold cache entries)
    auto gpr = static_cast<std::uint32_t>(dirty) & plan_.pinned;
    while (gpr) {
        const auto g = static_cast<unsigned>(std::countr_zero(gpr));
        gpr &= gpr - 1;
        a_.mov(64, gmem(g), pin(g));
    }
    auto vec = static_cast<std::uint32_t>(dirty >> 32) & plan_.vpinned;
    while (vec) {
        const auto v = static_cast<unsigned>(std::countr_zero(vec));
        vec &= vec - 1;
        vmove(ptr(Gp::r15, v_disp(v)), vpin(v));
    }
}

// ---- flags ------------------------------------------------------------------------

bool RegionEmitter::keep_flags() const {
    return is_host(flags_) && !flags_saved_ && (cur_reads_flags_ || (live_out_ && !cur_writes_flags_));
}

void RegionEmitter::prepare_clobber() {
    if (!is_host(flags_) || flags_saved_) return;
    if (bool_reader_ >= 0) {   // a fused CCMP's answer: kept in the context until its reader
        park_bool();
        return;
    }
    if (!(cur_reads_flags_ || (live_out_ && !cur_writes_flags_))) {
        flags_ = FlagsState::ctx;   // dead
        return;
    }
    if (save_flags_around()) return;
    // Flags kept only for an exit (nothing in the region reads them before
    // the next write; under exact flags, mostly the next call or return):
    // the raw form, as at the exit itself.
    materialize(!cur_reads_flags_ && !flags_read_after());
}

bool RegionEmitter::flags_read_after() const {
    bool read = false;
    if (index_ >= 0)
        plan_.for_each_successor(index_, [&](std::int32_t t) { read = read || s_.flags_read[static_cast<std::size_t>(t)] != 0; });
    return read;
}

// The flags outlive an instruction that must clobber EFLAGS (EOR/ORR/AND,
// shifts without BMI2, ...) and are read again within a few instructions:
// keep them in host form by saving AH/OF in rax around it (lahf; seto al
// ... add al, 0x7f; sahf in end_instruction) instead of converting them to
// ctx->nzcv and testing them there. Not for instructions that read or write
// the flags, may fault (FlagsLocation has no saved form), branch, or are
// family code; not with rax in use.
bool RegionEmitter::save_flags_around() {
    if (!codegen_on(knob_no_flag_save) || !in_instruction_ || family_mode_ || !cur_ || is_fcmp(flags_)) return false;
    if (cur_reads_flags_ || cur_writes_flags_ || cur_->may_fault || cur_->flow != Flow::normal || cur_->conservative_writes)
        return false;
    if (!is_free(Gp::rax)) return false;
    // The next flags reader in this block within three instructions.
    const auto n = static_cast<std::int32_t>(plan_.insts.size());
    bool near = false;
    for (std::int32_t j = index_ + 1; j < n && j <= index_ + 3; ++j) {
        const auto& pj = plan_.insts[static_cast<std::size_t>(j)];
        if (pj.leader) break;
        if (pj.reads_flags) { near = true; break; }
        if (pj.writes_flags || pj.flow != Flow::normal) break;
    }
    if (!near) return false;
    evict_gpr(x64::id(Gp::rax));
    take(Gp::rax);
    a_.lahf();
    a_.setcc(Cond::o, Gp::rax);
    flags_saved_ = true;
    return true;
}

// A fused CCMP's answer (ZF clear: the reader is taken) is about to be
// clobbered before its reader: setnz into kBoolSlot; the reader tests it
// there (condition()). NZCV itself is dead after the reader and nothing in
// between reads it or may fault, so the context's NZCV is left stale.
void RegionEmitter::park_bool() {
    // The scratch is released right away: the instruction parking the answer
    // may take() a fixed register next (SMULH: rax, rdx; shifts: rcx).
    const auto t = alloc();
    a_.setcc(Cond::ne, t);
    a_.mov(8, ptr(Gp::r15, spill_disp(kBoolSlot)), t);
    free_gpr_ |= 1u << x64::id(t);
    bool_in_slot_ = true;
    flags_ = FlagsState::ctx;
}

void RegionEmitter::materialize(bool raw) {
    if (!is_host(flags_)) return;
    if (bool_reader_ >= 0) {
        park_bool();
        return;
    }
    if (flags_saved_) {   // save_flags_around: EFLAGS back first, rax no longer held
        a_.alu(Alu::add, 8, Gp::rax, 0x7f);
        a_.sahf();
        flags_saved_ = false;
        free_gpr_ |= 1u << x64::id(Gp::rax);
    }
    const bool save = !is_free(Gp::rax);
    if (!save) evict_gpr(x64::id(Gp::rax));   // rax is clobbered
    materialize_state(flags_, save, raw);
    flags_ = FlagsState::ctx;
}

// EFLAGS -> ARM NZCV in ctx->nzcv without pushfq:
//   lahf: AH = SF ZF - AF - PF 1 CF; seto al
//   eax & 0xC101 = SF<<15 | ZF<<14 | CF<<8 | OF
//   * (1<<16 | 1<<21 | 1<<28) moves them to bits 31, 30, 29, 28 without carries.
//
// Region exits store only the first half (raw form, TRANSMUTE_FX_CORE=
// norawflags: the full conversion): AX = AH:AL after lahf/seto with the ARM
// carry, in the low 16 bits of ctx->nzcv (AH bit 1 is always 1, and the ARM
// layout has a zero low half, so the form tags itself). The next reader
// finishes the conversion: entries whose code reads NZCV
// (normalize_entry_flags) and the runtime when the region returns
// (nzcv_normalize). Most targets write the flags before they read any.
void RegionEmitter::materialize_state(FlagsState state, bool save_rax, bool raw) {
    if (!is_host(state)) return;
    if (is_fcmp(state)) {
        materialize_fcmp(state == FlagsState::host_fcmp_swapped, save_rax);
        return;
    }
    if (save_rax) a_.mov(64, ptr(Gp::r15, spill_disp(kRaxSaveSlot)), Gp::rax);
    if (state == FlagsState::host_sub) a_.cmc();
    a_.lahf();
    a_.setcc(Cond::o, Gp::rax);
    if (raw && codegen_on(knob_no_raw_flags)) {
        a_.mov(16, ptr(Gp::r15, kNzcvDisp), Gp::rax);
        if (save_rax) a_.mov(64, Gp::rax, ptr(Gp::r15, spill_disp(kRaxSaveSlot)));
        return;
    }
    a_.and_(32, Gp::rax, 0xc101);
    a_.imul(32, Gp::rax, Gp::rax, 0x10210000);
    a_.and_(32, Gp::rax, static_cast<std::int32_t>(0xf0000000u));
    a_.mov(64, ptr(Gp::r15, kNzcvDisp), Gp::rax);
    if (save_rax) a_.mov(64, Gp::rax, ptr(Gp::r15, spill_disp(kRaxSaveSlot)));
}

// An entry whose code may read NZCV before writing it (s_.flags_read: flags
// that only pass through to an exit stay raw) converts a raw form
// (materialize_state) to the ARM layout: cmp byte [nzcv + 1], 0 (AH, never
// 0 in the raw form); je over the second half of the conversion. Entries
// expect the flushed state: rax and EFLAGS are free.
void RegionEmitter::normalize_entry_flags(std::int32_t index) {
    if (index < 0 || !s_.flags_read[static_cast<std::size_t>(index)] || !codegen_on(knob_no_raw_flags)) return;
    const auto done = a_.new_label();
    a_.cmp(8, ptr(Gp::r15, kNzcvDisp + 1), 0);
    a_.jcc(Cond::e, done);
    a_.movzx(32, Gp::rax, ptr(Gp::r15, kNzcvDisp), 16);
    a_.and_(32, Gp::rax, 0xc101);
    a_.imul(32, Gp::rax, Gp::rax, 0x10210000);
    a_.and_(32, Gp::rax, static_cast<std::int32_t>(0xf0000000u));
    a_.mov(64, ptr(Gp::r15, kNzcvDisp), Gp::rax);
    a_.bind(done);
}

// Fused FCMP EFLAGS -> NZCV (GT 0010, LT 1000, EQ 0110, UN 0011) using only
// rax: (u)comis(a, b) sets CF for a < b or UN, ZF for a == b or UN, PF for
// UN; the cmov sources are staged in a spill slot (mov m32, imm keeps EFLAGS).
void RegionEmitter::materialize_fcmp(bool swapped, bool save_rax) {
    const auto k = ptr(Gp::r15, spill_disp(kFcmpConstSlot));
    if (save_rax) a_.mov(64, ptr(Gp::r15, spill_disp(kRaxSaveSlot)), Gp::rax);
    a_.mov_imm(Gp::rax, swapped ? 0x80000000u : 0x20000000u);   // a > b
    a_.mov(32, k, static_cast<std::int32_t>(swapped ? 0x20000000u : 0x80000000u));
    a_.cmov(Cond::b, 32, Gp::rax, k);                            // a < b (or UN)
    a_.mov(32, k, 0x60000000);
    a_.cmov(Cond::e, 32, Gp::rax, k);                            // a == b (or UN)
    a_.mov(32, k, 0x30000000);
    a_.cmov(Cond::p, 32, Gp::rax, k);                            // UN
    a_.mov(64, ptr(Gp::r15, kNzcvDisp), Gp::rax);
    if (save_rax) a_.mov(64, Gp::rax, ptr(Gp::r15, spill_disp(kRaxSaveSlot)));
}

// (u)comis(a, b) with a = Rn (or Rm when swapped). Native x64 conditions:
// a: a>b, ae: a>=b, b: a<b|UN, be: a<=b|UN, p: UN; EQ/NE and the other
// orientation combine with the parity flag.
RegionEmitter::FcmpCond RegionEmitter::fcmp_cond(unsigned cond) const {
    using K = FcmpCond::K;
    if (cond == 0) return {K::and_np, Cond::e};   // EQ
    if (cond == 1) return {K::or_p, Cond::ne};    // NE
    if (cond == 6) return {K::single, Cond::p};   // VS
    if (cond == 7) return {K::single, Cond::np};  // VC
    if (flags_ == FlagsState::host_fcmp) {
        switch (cond) {
        case 2: case 5: return {K::or_p, Cond::ae};    // CS/PL: GT EQ UN
        case 3: case 4: return {K::and_np, Cond::b};   // CC/MI: LT
        case 8: return {K::or_p, Cond::a};             // HI: GT UN
        case 9: return {K::and_np, Cond::be};          // LS: LT EQ
        case 10: return {K::single, Cond::ae};         // GE
        case 11: return {K::single, Cond::b};          // LT (or UN)
        case 12: return {K::single, Cond::a};          // GT
        default: return {K::single, Cond::be};         // LE (or UN)
        }
    }
    switch (cond) {   // swapped: a = Rm, b = Rn
    case 2: case 5: return {K::single, Cond::be};      // CS/PL: GT EQ UN
    case 3: case 4: return {K::single, Cond::a};       // CC/MI: LT
    case 8: return {K::single, Cond::b};               // HI: GT UN
    case 9: return {K::single, Cond::ae};              // LS: LT EQ
    case 10: return {K::and_np, Cond::be};             // GE: GT EQ
    case 11: return {K::or_p, Cond::a};                // LT: LT UN
    case 12: return {K::and_np, Cond::b};              // GT
    default: return {K::or_p, Cond::ae};               // LE: LT EQ UN
    }
}

FlagsLocation RegionEmitter::fault_flags() const {
    switch (flags_) {
    case FlagsState::host_sub: return FlagsLocation::host_sub;
    case FlagsState::host_add: return FlagsLocation::host_add;
    case FlagsState::host_logic: return FlagsLocation::host_logic;
    default: return FlagsLocation::context;
    }
}

Cond RegionEmitter::host_condition(unsigned cond) {
    switch (flags_) {
    case FlagsState::host_sub: return kSubCond[cond];
    case FlagsState::host_logic:
        if (cond == 8) return Cond::b;    // HI: C=0 -> false (CF = 0)
        if (cond == 9) return Cond::ae;   // LS: true
        return kAddCond[cond];
    case FlagsState::host_add:
        if (cond == 8 || cond == 9) {
            a_.cmc();   // CF = !C: the EFLAGS now have sub-producer meaning
            flags_ = FlagsState::host_sub;
            return kSubCond[cond];
        }
        return kAddCond[cond];
    case FlagsState::host_fcmp:
    case FlagsState::host_fcmp_swapped: {
        const auto fc = fcmp_cond(cond);
        if (fc.k == FcmpCond::K::single) return fc.c;
        if (cur_reads_flags_ ? (live_out_ && !cur_writes_flags_) : true) {
            materialize();   // the flags outlive this consumer: keep them in ctx
            break;
        }
        // Dead after this consumer: fold "c AND not parity" / "c OR parity"
        // into ZF (byte setcc, flag-preserving zeroing first).
        const auto t1 = alloc(), t2 = alloc();
        a_.setcc(fc.c, t1);
        a_.setcc(fc.k == FcmpCond::K::and_np ? Cond::np : Cond::p, t2);
        if (fc.k == FcmpCond::K::and_np) a_.test(8, t1, t2);
        else a_.alu(Alu::or_, 8, t1, t2);
        flags_ = FlagsState::ctx;   // dead
        return Cond::ne;
    }
    default: break;
    }
    // Flags in ctx->nzcv.
    if (cond < 8) {
        static constexpr std::int32_t bit[4] = {0x40000000, 0x20000000, static_cast<std::int32_t>(0x80000000u), 0x10000000};
        a_.test(32, ptr(Gp::r15, kNzcvDisp), bit[cond >> 1]);
        return (cond & 1) ? Cond::e : Cond::ne;
    }
    static const std::uint32_t masks[16] = {
        cond_mask(0), cond_mask(1), cond_mask(2), cond_mask(3), cond_mask(4), cond_mask(5),
        cond_mask(6), cond_mask(7), cond_mask(8), cond_mask(9), cond_mask(10), cond_mask(11),
        cond_mask(12), cond_mask(13), cond_mask(14), cond_mask(15),
    };
    const auto t1 = alloc();
    const auto t2 = alloc();
    a_.mov(32, t1, ptr(Gp::r15, kNzcvDisp));
    a_.shr(32, t1, 28);
    a_.mov_imm(t2, masks[cond]);
    a_.bt(32, t2, t1);
    return Cond::b;
}

// Flags live into `target` only on their way to an exit: the edge stores the
// raw form (materialize_state), which nothing in the region reads.
bool RegionEmitter::edge_raw(std::int32_t target) const {
    return !s_.flags_read[static_cast<std::size_t>(target)] && codegen_on(knob_no_raw_flags);
}

bool RegionEmitter::needs_edge_materialize(std::int32_t target) const {
    const auto& t = plan_.insts[target];
    return is_host(flags_) && t.flags_live_in && !t.inherit_flags;
}

// ---- faults ---------------------------------------------------------------------------

void RegionEmitter::mark_may_fault() {
    if (slot_alias_count_ != 0)   // tier 2: materialized before instructions that may fault
        throw RetryTranslation{pc_, false, RetryTranslation::Resource::aliases};
    fault_close();
    // FlagsLocation has no FCMP form: a fault must find NZCV in the context.
    if (is_fcmp(flags_)) prepare_clobber();
    // Write-back: the fault handler rebuilds guest state from the pins, the
    // site's fault map (dirty cached registers: host register -> guest) and
    // the context. Without fault maps every dirty register is stored first.
    fault_map_ = 0;
    if (fault_flush_) {
        if (fault_maps_) fault_map_ = dirty_map();
        else flush_dirty();
    }
    fault_open_ = true;
    fault_begin_ = static_cast<std::uint32_t>(a_.size());
    fault_location_ = fault_flags();
}

std::uint16_t RegionEmitter::dirty_map() {
    if (slot_alias_count_ != 0)   // tier 2: fault maps cannot name another slot
        throw RetryTranslation{pc_, false, RetryTranslation::Resource::aliases};
    // Tier 2: lent pins are in the context, not in their host (overrides the region pin map).
    const auto lent = lent_ & (static_cast<std::uint64_t>(plan_.pinned) | (static_cast<std::uint64_t>(plan_.vpinned) << 32));
    if (!(gc_.dirty | vc_.dirty | valias_dirty_ | galias_dirty_) && lent == 0) return 0;
    PinnedRegister regs[128];
    unsigned n = 0;
    for (auto m = lent; m; m &= m - 1) {
        const auto b = static_cast<unsigned>(std::countr_zero(m));
        regs[n++] = {static_cast<std::uint8_t>(b & 31u), kFaultMapContext, b >= 32};
    }
    for (auto m = gc_.dirty; m; m &= m - 1) {
        const auto h = static_cast<unsigned>(std::countr_zero(m));
        regs[n++] = {static_cast<std::uint8_t>(gc_.guest[h]), static_cast<std::uint8_t>(h), false};
    }
    for (auto m = vc_.dirty; m; m &= m - 1) {
        const auto h = static_cast<unsigned>(std::countr_zero(m));
        regs[n++] = {static_cast<std::uint8_t>(vc_.guest[h]), static_cast<std::uint8_t>(h), true};
    }
    for (auto m = valias_dirty_; m; m &= m - 1) {   // tier 2: dirty aliases share their host
        const auto g = static_cast<unsigned>(std::countr_zero(m));
        regs[n++] = {static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(valias_host_[g]), true};
    }
    for (auto m = galias_dirty_; m; m &= m - 1) {   // tier 2: dirty GPR copy aliases
        const auto g = static_cast<unsigned>(std::countr_zero(m));
        regs[n++] = {static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(galias_host_[g]), false};
    }
    auto& maps = out_.fault_maps;
    auto& all = out_.fault_map_regs;
    if (!maps.empty() && maps.back().count == n) {
        const auto* last = all.data() + maps.back().begin;
        bool same = true;
        for (unsigned i = 0; i < n && same; ++i)
            same = last[i].guest == regs[i].guest && last[i].host == regs[i].host && last[i].vector == regs[i].vector;
        if (same) return static_cast<std::uint16_t>(maps.size());
    }
    if (maps.size() >= 0xffffu) throw RetryTranslation{pc_, false, RetryTranslation::Resource::writeback};
    maps.push_back({static_cast<std::uint32_t>(all.size()), n});
    all.insert(all.end(), regs, regs + n);
    return static_cast<std::uint16_t>(maps.size());
}

void RegionEmitter::fault_reopen() {
    if (!fault_open_) return;
    fault_close();
    fault_map_ = fault_flush_ && fault_maps_ ? dirty_map() : 0;
    fault_open_ = true;
    fault_begin_ = static_cast<std::uint32_t>(a_.size());
}

void RegionEmitter::fault_close() {
    if (!fault_open_) return;
    fault_open_ = false;
    const auto end = static_cast<std::uint32_t>(a_.size());
    if (end == fault_begin_) return;
    auto& faults = out_.faults;
    // fault_pc_: pc_, or the producer's pc in a folded ADD + SIMD&FP access (neon; additive).
    if (!faults.empty() && faults.back().host_end == fault_begin_ && faults.back().guest_pc == fault_pc_ &&
        faults.back().flags == fault_location_ && faults.back().map == fault_map_) {
        faults.back().host_end = end;
        return;
    }
    faults.push_back({fault_begin_, end, fault_pc_, fault_location_, 0, fault_map_});
}

// ---- exits -----------------------------------------------------------------------------

std::size_t RegionEmitter::fixup_imm64(Gp r, std::uint64_t value) {
    const auto at = a_.mov_imm64(r, value);
    out_.fixups.push_back({static_cast<std::uint32_t>(at), FixupKind::guest_address_imm64, value});
    return at;
}

void RegionEmitter::mov_literal(Gp r, std::uint64_t value) {
    if (value <= 0xffffffffULL || static_cast<std::int64_t>(value) == static_cast<std::int64_t>(static_cast<std::int32_t>(value))) {
        a_.mov_imm(r, value);   // 32-bit forms: no 8-byte immediate
        return;
    }
    const auto at = a_.mov_imm64(r, value);
    out_.fixups.push_back({static_cast<std::uint32_t>(at), FixupKind::literal_imm64, value});
}

void RegionEmitter::emit_direct_exit(std::uint64_t target, std::uint64_t dirty, FlagsState flags, bool restore) {
    emit_exit_flags(flags);
    exit_pins(dirty, restore);
    a_.align_rel32_after(1);
    if (plan_.cold_layout && !cold_) {
        // In the body only the patchable jmp: its unlinked tail (dead once
        // linked) is cold. The runtime records the jmp's initial rel32 at
        // install and restores it on unlink, wherever the tail is.
        Stub s{Stub::K::exit_tail, a_.new_label(), FlagsState::ctx, 0, target, -1};
        s.field = a_.bound_label();   // the opcode; the rel32 field follows it
        const auto field_offset = static_cast<std::uint32_t>(a_.jmp(s.label));
        stubs_.push_back(s);
        out_.exits.push_back({field_offset, target});
        no_fall_at_ = a_.size();
        return;
    }
    a_.db(0xe9);
    const auto field = a_.bound_label();
    const auto field_offset = static_cast<std::uint32_t>(a_.size());
    a_.dd(0);   // initially jumps to the unlinked tail that follows
    fixup_imm64(Gp::rax, target);
    a_.mov(64, ptr(Gp::r15, kPcDisp), Gp::rax);
    a_.lea(64, Gp::rax, x64::rip(field));
    a_.jmp(ctx_mem(offsetof(GuestContext, stub_link)));
    out_.exits.push_back({field_offset, target});
}

void RegionEmitter::emit_reason_exit(std::uint64_t dirty, std::uint64_t pc, ExitReason reason, std::uint64_t arg,
                                     bool has_arg) {
    if (plan_.cold_layout && !cold_) {
        Stub s{Stub::K::reason_exit, a_.new_label(), FlagsState::ctx, dirty, pc, -1};
        s.reason = reason;
        s.arg = arg;
        s.has_arg = has_arg;
        a_.jmp(s.label);
        stubs_.push_back(s);
        return;
    }
    exit_pins(dirty);
    emit_exit_with_reason(pc, reason, arg, has_arg);
}

void RegionEmitter::jcc_label(Cond c, Label l) {
    if (plan_.cold_layout && a_.is_bound(l) &&
        static_cast<std::int64_t>(a_.offset_of(l)) - static_cast<std::int64_t>(a_.size() + 2) >= -128) {
        a_.jcc_short(c, l);
        return;
    }
    a_.jcc(c, l);
}

void RegionEmitter::jmp_label(Label l) {
    if (plan_.cold_layout && a_.is_bound(l) &&
        static_cast<std::int64_t>(a_.offset_of(l)) - static_cast<std::int64_t>(a_.size() + 2) >= -128) {
        a_.jmp_short(l);
        return;
    }
    a_.jmp(l);
}

void RegionEmitter::emit_exit_with_reason(std::uint64_t pc, ExitReason reason, std::uint64_t arg, bool has_arg) {
    fixup_imm64(Gp::rax, pc);
    a_.mov(64, ptr(Gp::r15, kPcDisp), Gp::rax);
    a_.mov(32, ctx_mem(offsetof(GuestContext, exit_reason)), static_cast<std::int32_t>(reason));
    if (has_arg) a_.mov(64, ctx_mem(offsetof(GuestContext, exit_arg)), static_cast<std::int32_t>(arg));
    a_.jmp(ctx_mem(offsetof(GuestContext, stub_dispatch_exit)));
}

void RegionEmitter::emit_interpret_exit(std::int32_t index) {
    (void)index;
    alias_resolve();     // tier 2
    forget_vec_zero();   // fp/neon
    flush_dirty();
    materialize_state(flags_, false);
    flags_ = FlagsState::ctx;
    emit_reason_exit(current_dirty(), pc_, ExitReason::interpret, 0, false);
    ++out_.stats.interpreted_instructions;
}

Label RegionEmitter::add_stub(Stub::K kind, FlagsState flags, std::uint64_t dirty, std::uint64_t pc, std::int32_t target,
                              std::int32_t snap, bool boundary) {
    const auto label = a_.new_label();
    stubs_.push_back({kind, label, flags, dirty, pc, target, snap, boundary});
    // Tier 2: the pins lent where the stub's code starts (none in the boundary state).
    if (!boundary && kind != Stub::K::return_trampoline && kind != Stub::K::tier_up) stubs_.back().lent = lent_;
    return label;
}

Label RegionEmitter::label_of(std::int32_t index) {
    auto& l = labels_[index];
    if (!l.valid()) l = a_.new_label();
    return l;
}

void RegionEmitter::edge_inline(std::int32_t target) {
    const auto& t = plan_.insts[target];
    // Tier 2: entering or leaving a loop whose unused pins lend their hosts.
    const auto from = lent_, to = lent_at(target);
    if (from != to) lend_enter(from, to);
    if (!t.leader) {
        if (from != to) {
            // Cache entries in hosts that become pins again: stored (unless dead), dropped.
            const auto keep = cache_hosts_at(target);
            const auto dead = dead_at(target);
            for (auto m = gc_.mask & ~static_cast<std::uint32_t>(keep); m; m &= m - 1) {
                const auto h = static_cast<unsigned>(std::countr_zero(m));
                const auto g = static_cast<unsigned>(gc_.guest[h]);
                if (((gc_.dirty >> h) & 1u) && !((dead >> g) & 1u)) a_.mov(64, gmem(g), x64::gp(h));
                drop_entry(gc_, h);
            }
            for (auto m = vc_.mask & ~static_cast<std::uint32_t>(keep >> 32); m; m &= m - 1) {
                const auto h = static_cast<unsigned>(std::countr_zero(m));
                const auto g = static_cast<unsigned>(vc_.guest[h]);
                if (((vc_.dirty >> h) & 1u) && !((dead >> (32 + g)) & 1u)) vmove(ptr(Gp::r15, v_disp(g)), x64::xmm(h));
                drop_entry(vc_, h);
            }
            lend_leave(from, to);
            set_lent(to);
        }
        return;
    }
    edge_cache(target, false);
    if (t.inherit_flags) {
        inherited_[target] = flags_;
    } else if (needs_edge_materialize(target)) {
        materialize(edge_raw(target));   // rax is never in a merge point's entry state
    }
    if (from != to) {
        lend_leave(from, to);
        set_lent(to);
    }
}

void RegionEmitter::jump_to(std::int32_t target, std::uint64_t target_pc) {
    if (target < 0) {
        flush_dirty(true);
        emit_direct_exit(target_pc, current_dirty(), flags_);
        return;
    }
    jump_edge(target, index_, target == index_ + 1);
}

// Unconditional transfer from `from` (-1 = region entry) into `target`,
// falling through when `fall_through`.
void RegionEmitter::jump_edge(std::int32_t target, std::int32_t from, bool fall_through) {
    if (!fall_through && is_pending_target(target)) {
        a_.jmp(pending_edge(target, from, flags_));
        return;
    }
    edge_inline(target);
    if (!fall_through) jmp_label(label_of(target));
}

std::int32_t RegionEmitter::dirty_snap() {
    return (gc_.dirty | vc_.dirty) ? push_snap() : -1;
}

// Label for a jcc into `taken` with flags state `state`. Forward edges into
// merge points are pending (resolved at the block); back edges reconcile the
// cache before the jcc (both paths run it: the not-taken path usually leaves
// the loop once).
Label RegionEmitter::taken_label(std::int32_t taken, FlagsState state) {
    const auto& t = plan_.insts[taken];
    if (is_pending_target(taken)) return pending_edge(taken, index_, state);
    // Tier 2, pass 2: a forward edge into a block whose entry state is
    // decided reconciles in an edge stub (cold), so the not-taken path keeps
    // its registers and dirty values.
    if (lent_ != lent_at(taken)) {   // tier 2: pin lending changes on this edge: an edge stub
        if (s_.cache_slot[static_cast<std::size_t>(taken)] < 0) decide_entry(taken, true);
        if (t.inherit_flags) inherited_[taken] = state;   // the stub's moves keep EFLAGS
        return add_stub(Stub::K::edge, needs_edge_materialize(taken) ? state : FlagsState::ctx, 0, 0, taken, push_snap());
    }
    if (alloc_pass_ == 2 && t.leader && (taken > index_ || leaves_loop(index_, taken)) &&
        s_.cache_slot[static_cast<std::size_t>(taken)] >= 0) {
        const auto& e = s_.cache_snaps[static_cast<std::size_t>(s_.cache_slot[static_cast<std::size_t>(taken)])];
        const bool mat = needs_edge_materialize(taken);
        if (mat || reconcile_needs_code({gc_, vc_}, e, dead_at(taken)))
            return add_stub(Stub::K::edge, mat ? state : FlagsState::ctx, 0, 0, taken, push_snap());
        if (t.inherit_flags) inherited_[taken] = state;
        return label_of(taken);
    }
    if (t.leader) edge_cache(taken, true);
    if (needs_edge_materialize(taken)) return add_stub(Stub::K::edge, state, 0, 0, taken);
    if (t.inherit_flags) inherited_[taken] = state;
    return label_of(taken);
}

void RegionEmitter::cond_branch(Cond c, std::int32_t taken, std::uint64_t taken_pc, std::int32_t fall,
                                std::uint64_t fall_pc) {
    const auto state = flags_;
    if (taken >= 0) jcc_label(c, taken_label(taken, state));
    else a_.jcc(c, add_stub(Stub::K::direct_exit, state, current_dirty(), taken_pc, -1, dirty_snap()));
    if (fall >= 0) {
        // Tier 2: loop-carried dirty values leave the loop stored.
        if (alloc_pass_ == 2 && !plan_.insts[static_cast<std::size_t>(fall)].leader && leaves_loop(index_, fall))
            flush_dirty(true);
        edge_inline(fall);
        if (fall != index_ + 1) jmp_label(label_of(fall));
    } else {
        flush_dirty(true);
        emit_direct_exit(fall_pc, current_dirty(), flags_);
    }
}

void RegionEmitter::rsb_push(std::uint64_t return_pc, std::int32_t continuation, Gp ret_reg) {
    (void)return_pc;
    // ret_reg = return pc on entry. Clobbers rax, r8, rdx and EFLAGS.
    const auto host = continuation >= 0
                          ? entry_labels_[static_cast<std::size_t>(plan_.insts[continuation].entry_slot)]
                          : add_stub(Stub::K::return_trampoline, FlagsState::ctx, 0, return_pc, -1);
    const auto top = ctx_mem(offsetof(GuestContext, rsb_top));
    const auto rsb = ctx_disp(offsetof(GuestContext, rsb));
    a_.mov(32, Gp::rax, top);
    a_.lea(32, Gp::r8, ptr(Gp::rax, 1));
    a_.and_(32, Gp::r8, static_cast<std::int32_t>(kRsbEntries - 1));
    a_.mov(32, top, Gp::r8);
    a_.and_(32, Gp::rax, static_cast<std::int32_t>(kRsbEntries - 1));
    a_.shl(32, Gp::rax, 4);
    a_.mov(64, ptr(Gp::r15, Gp::rax, 1, rsb), ret_reg);
    a_.lea(64, Gp::rdx, x64::rip(host));
    a_.mov(64, ptr(Gp::r15, Gp::rax, 1, rsb + 8), Gp::rdx);
}

// ---- cross-block register cache --------------------------------------------------------

std::int32_t RegionEmitter::push_snap() {
    alias_guard();   // tier 2: snapshots never hold aliases
    s_.cache_snaps.push_back({gc_, vc_});
    return static_cast<std::int32_t>(s_.cache_snaps.size() - 1);
}

void RegionEmitter::spill_snap(std::int32_t snap) {
    if (snap < 0) return;
    const auto c = s_.cache_snaps[static_cast<std::size_t>(snap)];
    for (auto m = c.gpr.dirty & c.gpr.mask; m; m &= m - 1) {
        const auto h = static_cast<unsigned>(std::countr_zero(m));
        a_.mov(64, gmem(static_cast<unsigned>(c.gpr.guest[h])), x64::gp(h));
    }
    for (auto m = c.vec.dirty & c.vec.mask; m; m &= m - 1) {
        const auto h = static_cast<unsigned>(std::countr_zero(m));
        vmove(ptr(Gp::r15, v_disp(static_cast<unsigned>(c.vec.guest[h]))), x64::xmm(h));
    }
}

void RegionEmitter::load_entry_state(std::int32_t index) {
    const auto slot = s_.cache_slot[static_cast<std::size_t>(index)];
    if (slot < 0) return;
    const auto& e = s_.cache_snaps[static_cast<std::size_t>(slot)];
    for (auto m = e.gpr.mask; m; m &= m - 1) {
        const auto h = static_cast<unsigned>(std::countr_zero(m));
        a_.mov(64, x64::gp(h), gmem(static_cast<unsigned>(e.gpr.guest[h])));
    }
    for (auto m = e.vec.mask; m; m &= m - 1) {
        const auto h = static_cast<unsigned>(std::countr_zero(m));
        vmove(x64::xmm(h), ptr(Gp::r15, v_disp(static_cast<unsigned>(e.vec.guest[h]))));
    }
}

// Before a control transfer from the current state into `target`. With
// both_paths the code runs before a jcc: the not-taken path continues with
// the reconciled state plus the untouched (now clean) other entries.
void RegionEmitter::edge_cache(std::int32_t target, bool both_paths) {
    if (!plan_.insts[target].leader) return;
    if (s_.cache_slot[static_cast<std::size_t>(target)] < 0) decide_entry(target, true);
    const auto e = s_.cache_snaps[static_cast<std::size_t>(s_.cache_slot[static_cast<std::size_t>(target)])];
    // Tier 2: dirty values neither path observes again need no store (with
    // both_paths the code also runs on the not-taken path, the fall-through).
    auto dead = dead_at(target);
    if (both_paths) dead &= cur_ && cur_->flow == Flow::cond ? dead_at(cur_->fall) : 0;
    reconcile(e, both_paths, dead);
}

bool RegionEmitter::reconcile_needs_code(const CacheSnapshot& from, const CacheSnapshot& to, std::uint64_t dead) {
    const auto differs = [](const RegCache& c, const RegCache& t, std::uint32_t d) {
        for (auto m = c.dirty & c.mask; m; m &= m - 1) {
            const auto h = static_cast<unsigned>(std::countr_zero(m));
            const auto g = static_cast<unsigned>(c.guest[h]);
            const auto th = t.host[g];
            if ((th < 0 || !((t.dirty >> th) & 1u)) && !((d >> g) & 1u)) return true;   // store
        }
        for (auto m = t.mask; m; m &= m - 1) {
            const auto th = static_cast<unsigned>(std::countr_zero(m));
            if (c.host[static_cast<unsigned>(t.guest[th])] != static_cast<std::int8_t>(th)) return true;   // move or load
        }
        return false;
    };
    return differs(from.gpr, to.gpr, static_cast<std::uint32_t>(dead)) ||
           differs(from.vec, to.vec, static_cast<std::uint32_t>(dead >> 32));
}

const RegionEmitter::LoopInfo& RegionEmitter::loop_info(std::int32_t header) {
    auto& slot = s_.loop_slot[static_cast<std::size_t>(header)];
    if (slot >= 0) return s_.loops[static_cast<std::size_t>(slot)];
    LoopInfo li{};
    std::uint32_t wr = 0, vwr = 0;
    const auto end = s_.loop_end[static_cast<std::size_t>(header)];
    for (std::int32_t j = header; j <= end; ++j) {
        const auto& pj = plan_.insts[j];
        if (pj.flow == Flow::interpret || pj.conservative_writes) continue;
        li.exposed |= pj.reads & ~wr;
        li.vexposed |= pj.vreads & ~vwr;
        wr |= pj.writes;
        vwr |= pj.vwrites;
        for (auto m = pj.reads; m; m &= m - 1) {
            auto& w = li.weight[std::countr_zero(m)];
            if (w < 0xffff) ++w;
        }
        for (auto m = pj.vreads; m; m &= m - 1) {
            auto& w = li.vweight[std::countr_zero(m)];
            if (w < 0xffff) ++w;
        }
    }
    li.writes = wr;
    li.vwrites = vwr;
    slot = static_cast<std::int32_t>(s_.loops.size());
    s_.loops.push_back(li);
    return s_.loops.back();
}

// A forward edge into a merge point whose entry state is not decided yet:
// it records its cache state and jumps to a label that start_block() binds
// to the block itself (the state already fits) or to an edge stub.
bool RegionEmitter::is_pending_target(std::int32_t target) const {
    const auto& t = plan_.insts[target];
    return carry_ && t.leader && !t.inherit_flags && target > index_ &&
           s_.cache_slot[static_cast<std::size_t>(target)] < 0;
}

Label RegionEmitter::pending_edge(std::int32_t target, std::int32_t from, FlagsState state) {
    const auto label = a_.new_label();
    const auto snap = push_snap();
    const bool materialize_flags = needs_edge_materialize(target);
    s_.pend.push_back({label, snap, from, state, materialize_flags, s_.pend_head[static_cast<std::size_t>(target)]});
    s_.pend_head[static_cast<std::size_t>(target)] = static_cast<std::int32_t>(s_.pend.size() - 1);
    return label;
}

// The entry state of a merge point: the entries all its relevant forward
// predecessors hold in the same register (dirty if dirty in any), so those
// edges need no code. Relevant: inside the innermost loop when the block
// is in a loop body (edges entering the loop in the middle come from colder
// code and reconcile in a stub), and never the region-start edge when
// others exist (it can load anything). Loop headers keep only registers
// their first block reads before writing (the back edges reload evicted
// ones; those loads replace the header's own), dirty if the loop writes
// them. rax never stays (flag materialization on edges clobbers it).
void RegionEmitter::merge_entry(std::int32_t target, bool with_current, CacheSnapshot& e) {
    const auto loop = s_.loop_of[static_cast<std::size_t>(target)];
    const bool body = loop >= 0 && loop != target;
    const auto lend = loop >= 0 ? s_.loop_end[static_cast<std::size_t>(loop)] : -1;
    std::int32_t preds = with_current ? 1 : 0;
    for (auto p = s_.pend_head[static_cast<std::size_t>(target)]; p >= 0; p = s_.pend[static_cast<std::size_t>(p)].next) ++preds;
    const auto relevant_raw = [&](std::int32_t from) {
        if (from < 0) return preds == 1;
        return !body || (from >= loop && from <= lend);
    };
    bool any = with_current && relevant_raw(index_);
    for (auto p = s_.pend_head[static_cast<std::size_t>(target)]; p >= 0; p = s_.pend[static_cast<std::size_t>(p)].next)
        any |= relevant_raw(s_.pend[static_cast<std::size_t>(p)].from);
    const auto relevant = [&](std::int32_t from) { return !any || relevant_raw(from); };
    const auto intersect = [](RegCache& a, const RegCache& b) {
        for (auto m = a.mask; m; m &= m - 1) {
            const auto h = static_cast<unsigned>(std::countr_zero(m));
            if (b.host[static_cast<unsigned>(a.guest[h])] != static_cast<std::int8_t>(h)) drop_entry(a, h);
            else if ((b.dirty >> h) & 1u) a.dirty |= 1u << h;
        }
    };
    bool first = true;
    const auto meet = [&](const CacheSnapshot& s) {
        if (first) {
            e = s;
            first = false;
        } else {
            intersect(e.gpr, s.gpr);
            intersect(e.vec, s.vec);
        }
    };
    if (with_current && relevant(index_)) meet({gc_, vc_});
    for (auto p = s_.pend_head[static_cast<std::size_t>(target)]; p >= 0; p = s_.pend[static_cast<std::size_t>(p)].next) {
        const auto& pe = s_.pend[static_cast<std::size_t>(p)];
        if (relevant(pe.from)) meet(s_.cache_snaps[static_cast<std::size_t>(pe.snap)]);
    }
    if (first) {
        reset_cache(e.gpr);
        reset_cache(e.vec);
    }
    drop_entry(e.gpr, 0);   // rax
    e.gpr.dirty &= e.gpr.mask;
    e.vec.dirty &= e.vec.mask;
    if (loop == target) {
        std::uint32_t use = 0, vuse = 0, wr = 0, vwr = 0;
        const auto n = static_cast<std::int32_t>(plan_.insts.size());
        for (std::int32_t j = target; j < n && j < target + 32; ++j) {
            const auto& pj = plan_.insts[j];
            if (pj.flow == Flow::interpret || pj.conservative_writes) break;
            use |= pj.reads & ~wr;
            vuse |= pj.vreads & ~vwr;
            wr |= pj.writes;
            vwr |= pj.vwrites;
            if (pj.flow != Flow::normal || (j > target && plan_.insts[j + 1 < n ? j + 1 : j].leader)) break;
        }
        const auto& li = loop_info(target);
        const auto keep = [](RegCache& c, std::uint32_t used, std::uint32_t loop_writes) {
            for (auto m = c.mask; m; m &= m - 1) {
                const auto h = static_cast<unsigned>(std::countr_zero(m));
                const auto g = static_cast<unsigned>(c.guest[h]);
                if (!((used >> g) & 1u)) drop_entry(c, h);
                else if ((loop_writes >> g) & 1u) c.dirty |= 1u << h;
            }
        };
        keep(e.gpr, use, li.writes);
        keep(e.vec, vuse, li.vwrites);
    }
}

// Decides the entry state of leader `target`: a single predecessor hands
// over its whole cache (with_current: the current state is that edge's);
// merge points see merge_entry(); without carry_ they start empty.
void RegionEmitter::decide_entry(std::int32_t target, bool with_current) {
    CacheSnapshot e{gc_, vc_};
    const auto& t = plan_.insts[target];
    if (!t.inherit_flags || !with_current) {
        if (!carry_) {
            reset_cache(e.gpr);
            reset_cache(e.vec);
        } else {
            merge_entry(target, with_current, e);
        }
    }
    if (!s_.lent.empty()) {   // tier 2: only hosts the cache may use there (pin lending)
        const auto hosts = cache_hosts_at(target);
        restrict_hosts(e.gpr, static_cast<std::uint32_t>(hosts));
        restrict_hosts(e.vec, static_cast<std::uint32_t>(hosts >> 32));
    }
    s_.cache_slot[static_cast<std::size_t>(target)] = static_cast<std::int32_t>(s_.cache_snaps.size());
    s_.cache_snaps.push_back(e);
}

void RegionEmitter::reconcile(const CacheSnapshot& e, bool keep_rest, std::uint64_t dead) {
    alias_guard();   // tier 2
    fault_close();
    reconcile_class(gc_, e.gpr, false, keep_rest, static_cast<std::uint32_t>(dead));
    reconcile_class(vc_, e.vec, true, keep_rest, static_cast<std::uint32_t>(dead >> 32));
}

// Current cache `c` -> entry state `t`: store dirty entries `t` does not
// keep dirty (tier 2: drop those in `dead`, which no path observes again),
// then fill t's registers by moves (ordered so no source is overwritten
// first; cycles go through the context) and context loads.
void RegionEmitter::reconcile_class(RegCache& c, const RegCache& t, bool vec, bool keep_rest, std::uint32_t dead) {
    const auto store = [&](unsigned g, unsigned h) {
        if (vec) vmove(ptr(Gp::r15, v_disp(g)), x64::xmm(h));
        else a_.mov(64, gmem(g), x64::gp(h));
    };
    for (auto m = c.dirty & c.mask; m; m &= m - 1) {
        const auto h = static_cast<unsigned>(std::countr_zero(m));
        const auto g = static_cast<unsigned>(c.guest[h]);
        const auto th = t.host[g];
        if (th >= 0 && ((t.dirty >> th) & 1u)) continue;   // stays dirty (moved if th != h)
        if ((dead >> g) & 1u) {   // never observed again: forget it (not kept for the other path either)
            if (th < 0) drop_entry(c, h);
            else c.dirty &= ~(1u << h);   // the target holds it clean: the value is dead, any copy will do
            continue;
        }
        store(g, h);
        c.dirty &= ~(1u << h);
    }
    c.dirty &= c.mask;
    struct Move {
        std::uint8_t dst, g;
        std::int8_t src;
        bool done;
    };
    Move mv[16];
    unsigned n = 0;
    for (auto m = t.mask; m; m &= m - 1) {
        const auto th = static_cast<unsigned>(std::countr_zero(m));
        const auto g = static_cast<unsigned>(t.guest[th]);
        if (c.host[g] == static_cast<std::int8_t>(th)) continue;
        mv[n++] = {static_cast<std::uint8_t>(th), static_cast<std::uint8_t>(g), c.host[g], false};
    }
    for (unsigned left = n; left != 0;) {
        bool progress = false;
        for (unsigned i = 0; i < n; ++i) {
            if (mv[i].done) continue;
            bool blocked = false;
            for (unsigned j = 0; j < n && !blocked; ++j)
                blocked = j != i && !mv[j].done && mv[j].src == static_cast<std::int8_t>(mv[i].dst);
            if (blocked) continue;
            if (mv[i].src < 0) {
                if (vec) vmove(x64::xmm(mv[i].dst), ptr(Gp::r15, v_disp(mv[i].g)));
                else a_.mov(64, x64::gp(mv[i].dst), gmem(mv[i].g));
            } else if (vec) {
                a_.movaps(x64::xmm(mv[i].dst), x64::xmm(static_cast<unsigned>(mv[i].src)));
            } else {
                a_.mov(64, x64::gp(mv[i].dst), x64::gp(static_cast<unsigned>(mv[i].src)));
            }
            mv[i].done = true;
            --left;
            progress = true;
        }
        if (!progress) {   // a cycle of moves: one value goes through the context
            for (unsigned i = 0; i < n; ++i) {
                if (mv[i].done || mv[i].src < 0) continue;
                const auto src = static_cast<unsigned>(mv[i].src);
                if ((c.dirty >> src) & 1u) store(mv[i].g, src);
                mv[i].src = -1;
                break;
            }
        }
    }
    RegCache post = t;
    if (keep_rest) {
        // Registers t does not use keep their (clean) entries.
        for (auto m = c.mask & ~t.mask; m; m &= m - 1) {
            const auto h = static_cast<unsigned>(std::countr_zero(m));
            const auto g = static_cast<unsigned>(c.guest[h]);
            if (t.host[g] >= 0) continue;
            post.host[g] = static_cast<std::int8_t>(h);
            post.guest[h] = static_cast<std::int8_t>(g);
            post.used[h] = c.used[h];
            post.mask |= 1u << h;
        }
    }
    c = post;
}

// ---- driver -------------------------------------------------------------------------------

void RegionEmitter::begin_instruction(std::int32_t index) {
    index_ = index;
    cur_ = &plan_.insts[index];
    pc_ = cur_->pc;
    fault_pc_ = pc_;          // neon (additive): see fold_family_access
    afold_.active = false;
    set_lent(lent_at(index));   // tier 2 (before reset_scratch: lent hosts are scratch)
    reset_scratch();
    written_ = 0;
    vwritten_ = 0;
    pending_gpr_count_ = 0;
    pending_vec_count_ = 0;
    fault_open_ = false;
    family_mode_ = false;
    live_out_ = cur_->flags_live_out;
    cur_reads_flags_ = cur_->reads_flags;
    cur_writes_flags_ = cur_->writes_flags;
    if (bool_reader_ >= 0 && bool_reader_ < index) {   // its reader was not reached (cannot happen)
        bool_reader_ = -1;
        bool_in_slot_ = false;
    }
    // NZCV dead at a BL/BLR/RET exit (TranslateOptions::abi_flags; strict
    // flags keep them live) or at a B out of the region to a target that
    // writes them first (target_flags_dead): drop host flags instead of
    // materializing them.
    if (is_host(flags_) && !cur_->flags_live_in &&
        (cur_->flow == Flow::call || cur_->flow == Flow::indirect || (cur_->flow == Flow::jump && cur_->taken < 0)))
        flags_ = FlagsState::ctx;
    cf_mark_ = a_.control_flow_count();
    in_instruction_ = true;
    after_last_ = index;
}

void RegionEmitter::end_instruction() {
    if (flags_saved_) {   // save_flags_around: OF from al (al + 0x7f overflows iff al == 1), SF ZF CF from ah
        a_.alu(Alu::add, 8, Gp::rax, 0x7f);
        a_.sahf();
        flags_saved_ = false;
    }
    fault_close();
    // Write-back: the result stays dirty in its register (stored at the next
    // observation point unless overwritten first). Write-through without it.
    for (unsigned i = 0; i < pending_gpr_count_; ++i) {
        const auto g = pending_gpr_[i].guest;
        const auto h = x64::id(pending_gpr_[i].host);
        if (!write_back_) a_.mov(64, gmem(g), pending_gpr_[i].host);
        cache_set(gc_, g, h);
        if (write_back_) gc_.dirty |= 1u << h;
    }
    for (unsigned i = 0; i < pending_vec_count_; ++i) {
        const auto v = pending_vec_[i].guest;
        const auto h = x64::id(pending_vec_[i].host);
        if (!write_back_) vmove(ptr(Gp::r15, v_disp(v)), pending_vec_[i].host);
        // tier 2: v's old value may still be another guest's (an alias owns it now).
        alias_drop_guest(v);
        if (const auto old = vc_.host[v]; old >= 0 && static_cast<unsigned>(old) != h && valias_of_[old])
            alias_owner_leaves(static_cast<unsigned>(old));
        cache_set(vc_, v, h);
        vslot_[h] = 0;
        if (write_back_) vc_.dirty |= 1u << h;
        else slot_stored(v, h);
    }
    cf_mark_ = a_.control_flow_count();   // later code runs after this instruction's merge
    pending_gpr_count_ = 0;
    pending_vec_count_ = 0;
    reset_scratch();
    family_mode_ = false;
    in_instruction_ = false;   // tier 2: observed() now asks after the instruction (after_last_)
}

void RegionEmitter::start_block(std::int32_t index) {
    const auto& pi = plan_.insts[index];
    set_lent(lent_at(index));   // tier 2: the code above handled the lending change of its edge
    // A continuation entry right after code that never falls through (the
    // call's exit): its entry stub is laid out here and falls into the block,
    // instead of a stub in the cold section that jumps back (Plan::cold_layout).
    // Not for loop headers: those keep their alignment (a loop that starts
    // right after a call, e.g. a timed loop after a clock read, measured up
    // to 1.7x slower unaligned behind an in-line stub).
    if (plan_.cold_layout && plan_.inline_entries && pi.entry && pi.entry_slot > 0 && !pi.loop_header &&
        no_fall_at_ == a_.size()) {
        if (s_.cache_slot[static_cast<std::size_t>(index)] < 0) {
            reset_cache(gc_);
            reset_cache(vc_);
            decide_entry(index, false);
        }
        a_.bind(entry_labels_[static_cast<std::size_t>(pi.entry_slot)]);
        normalize_entry_flags(index);
        entry_pins(index);
        load_entry_state(index);
        s_.entry_inline[static_cast<std::size_t>(pi.entry_slot)] = 1;
    } else if (pi.loop_header &&
               (!plan_.cold_layout || plan_.align_loops || (plan_.align_hot && plan_.optimize) ||
                s_.loop_end[static_cast<std::size_t>(index)] - index < kSmallLoop) &&
               // Tier 2: not when the code above falls into the header (the
               // padding would run on that path, e.g. every iteration of an
               // enclosing loop).
               !(plan_.t2_align && index > 0 && plan_.insts[static_cast<std::size_t>(index) - 1].fall == index)) {
        // Small loops start on a 16-byte boundary (fewer fetch blocks per
        // iteration); in larger ones the padding only costs code bytes.
        // Layout knobs: hotalign (every tier-2 loop), align32.
        a_.align(plan_.loop_alignment);
    }
    a_.bind(label_of(index));
    vzero_block_start(index);   // fp/neon: the facts every predecessor agrees on (p3)
    alias_guard();       // tier 2: resolved by every predecessor
    alias_forget_knowledge();
    tz_clz_ = -1;        // RBIT->TZCNT never spans blocks
    // The block starts with its entry state (decided by its first edge; a
    // block no edge reached yet starts empty). Code laid out before it that
    // does not fall through has handled its own cache state.
    if (s_.cache_slot[static_cast<std::size_t>(index)] < 0) {
        reset_cache(gc_);
        reset_cache(vc_);
        decide_entry(index, false);
    }
    {
        const auto e = s_.cache_snaps[static_cast<std::size_t>(s_.cache_slot[static_cast<std::size_t>(index)])];
        gc_ = e.gpr;
        vc_ = e.vec;
        // Pending forward edges: bound here when their state fits, else an
        // edge stub reconciles (and materializes flags if needed).
        for (auto p = s_.pend_head[static_cast<std::size_t>(index)]; p >= 0; p = s_.pend[static_cast<std::size_t>(p)].next) {
            const auto pe = s_.pend[static_cast<std::size_t>(p)];
            const auto from_lent = lent_at(pe.from);
            if (pe.materialize || from_lent != lent_at(index) ||
                reconcile_needs_code(s_.cache_snaps[static_cast<std::size_t>(pe.snap)], e, dead_at(index))) {
                stubs_.push_back({Stub::K::edge, pe.label, pe.materialize ? pe.flags : FlagsState::ctx, 0, 0, index, pe.snap});
                stubs_.back().lent = from_lent;
            } else {
                a_.bind(pe.label);
            }
        }
    }
    flags_ = pi.inherit_flags ? inherited_[index] : FlagsState::ctx;
    // Tier 2: a header whose cycles all pass another checked header needs no check.
    if (pi.loop_header && options_.interrupt_checks && !s_.no_check[static_cast<std::size_t>(index)]) {
        a_.cmp(32, ctx_mem(offsetof(GuestContext, interrupt_pending)), 0);
        const std::uint64_t dirty = (pi.dirty_in & plan_.pinned) |
                                    (static_cast<std::uint64_t>(pi.vdirty_in & plan_.vpinned) << 32);
        const auto snap = dirty_snap();
        a_.jcc(Cond::ne, add_stub(Stub::K::interrupt, FlagsState::ctx, dirty, pi.pc, index, snap));
        // Tier 2: counted first-tier code also counts loop iterations, in one
        // per-thread counter (whichever loop runs when it expires is hot). It
        // starts at zero: the first iteration (borrow) or every `threshold`
        // iterations (zero) exits with tier_up at the header, whose eager
        // (optimized) translation then continues the loop.
        if (plan_.count_loops && options_.lazy) {
            a_.alu(Alu::sub, 32, ctx_mem(offsetof(GuestContext, spill) + kLoopCounterSlot * 8), 1);
            a_.jcc(Cond::be, add_stub(Stub::K::loop_tier_up, FlagsState::ctx, dirty, pi.pc, index, snap));
        } else if (loop_counting_) {   // the region's iteration counter: dec dword [rip + slot]; jz
            a_.db(0xff);
            a_.db(0x0d);
            loop_counter_fields_.push_back(static_cast<std::uint32_t>(a_.size()));
            a_.dd(0);
            a_.jcc(Cond::e, add_stub(Stub::K::region_tier_up, FlagsState::ctx, dirty, pi.pc, index, snap));
        }
    }
}

unsigned RegionEmitter::try_families(std::int32_t index) {
    const auto table = registered_families();
    const auto n = static_cast<std::int32_t>(plan_.insts.size());
    std::int32_t len = 1;
    while (index + len < n && len < 64 && plan_.insts[index + len].pc == pc_ + 4u * static_cast<unsigned>(len) &&
           !plan_.insts[index + len].leader && index + len != tz_clz_)   // a pending RBIT->TZCNT CLZ stays core
        ++len;
    const std::span<const std::uint32_t> words(plan_.words.data() + index, static_cast<std::size_t>(len));
    const auto faults_before = out_.faults.size();
    for (unsigned f = 0; f < table.count; ++f) {
        reset_scratch();
        pending_gpr_count_ = 0;
        pending_vec_count_ = 0;
        fault_open_ = false;
        written_ = 0;
        vwritten_ = 0;
        family_mode_ = true;
        const auto consumed = table.families[f].emit(*this, words);
        if (consumed == 0) {
            fault_open_ = false;
            out_.faults.resize(faults_before);
            continue;
        }
        if (consumed > words.size()) throw RetryTranslation{pc_, false};
        std::uint32_t allowed = 0, vallowed = 0;
        for (unsigned j = 0; j < consumed; ++j) {
            const auto& pj = plan_.insts[index + static_cast<std::int32_t>(j)];
            const bool any = pj.flow == Flow::interpret || pj.conservative_writes;
            allowed |= any ? 0xffffffffu : pj.writes;
            vallowed |= any ? 0xffffffffu : pj.vwrites;
        }
        if ((written_ & ~allowed & plan_.pinned) || (vwritten_ & ~vallowed & plan_.vpinned))
            throw RetryTranslation{pc_, true};
        return consumed;
    }
    family_mode_ = false;
    return 0;
}

void RegionEmitter::emit_fallthrough(std::int32_t index, unsigned consumed) {
    const auto& pi = plan_.insts[index];
    auto next_pc = pi.pc + 4ull * consumed;
    auto next = index + static_cast<std::int32_t>(consumed);
    bool adjacent = next < static_cast<std::int32_t>(plan_.insts.size()) && plan_.insts[next].pc == next_pc &&
                    plan_.insts[next].segment == pi.segment;
    if (pi.inline_call || pi.inline_ret) {   // into the callee copy / its continuation (laid out next)
        next = pi.fall;
        next_pc = plan_.insts[next].pc;
        adjacent = true;
    }
    // tier 2: copy aliases stay inside the block.
    if (alias_count_ != 0 && (!adjacent || pi.flow == Flow::interpret || plan_.insts[next].leader)) alias_resolve();
    if (galias_count_ != 0 && (!adjacent || pi.flow == Flow::interpret || plan_.insts[next].leader)) galias_resolve();
    if (adjacent && pi.flow != Flow::interpret && plan_.insts[next].leader) record_edges(index, next);   // tier 2, pass 1
    // Leaving the region (or entering an entry stub): store the dirty cache.
    // Edges into leaders reconcile it (edge_inline); without carry it is
    // stored before every leader.
    if (!adjacent || pi.flow == Flow::interpret ||
        ((!carry_ || carry_clean_) && plan_.insts[next].leader && !dirty_into_merge(next))) {
        flush_dirty();
    } else if (alloc_pass_ == 2 && plan_.insts[next].leader &&
               !(next < static_cast<std::int32_t>(s_.preset_of.size()) && s_.preset_of[static_cast<std::size_t>(next)] >= 0)) {
        // tier 2: only the loop's carried values stay dirty into another block
        std::uint32_t keep_g = 0, keep_v = 0;
        carried_dirty(next, keep_g, keep_v);
        flush_dirty_except(keep_g, keep_v, dead_at(next));
    }
    if (!adjacent) {
        emit_direct_exit(next_pc, current_dirty(), flags_);
        return;
    }
    if (pi.flow == Flow::interpret) {
        // Planned as an interpret exit but a family translated it: the
        // continuation is an entry whose dataflow did not see this edge.
        const auto& t = plan_.insts[next];
        materialize_state(flags_, false);
        flags_ = FlagsState::ctx;
        exit_pins(current_dirty());
        if (t.entry) a_.jmp(entry_labels_[static_cast<std::size_t>(t.entry_slot)]);
        else emit_direct_exit(next_pc, 0, FlagsState::ctx, false);
        return;
    }
    edge_inline(next);
    // `next` is laid out immediately after: fall through.
}

void RegionEmitter::emit_instruction(std::int32_t index) {
    const auto& pi = plan_.insts[index];
    if (pi.leader) start_block(index);
    if (auto* map = instruction_map_sink())   // diagnostics
        map->push_back({pi.pc, static_cast<std::uint32_t>(pi.leader ? a_.offset_of(label_of(index)) : a_.size()),
                        static_cast<std::uint32_t>(a_.size()), pi.leader});
    begin_instruction(index);
    ++out_.stats.guest_instructions;
    // tier 2: copy aliases are resolved before control flow (a core
    // instruction may also emit a following branch with itself).
    if (alias_count_ != 0) {
        bool resolve = pi.flow != Flow::normal;
        for (std::int32_t j = index + 1; !resolve && pi.inst.kind != Kind::unknown && j <= index + 4 &&
                                         j < static_cast<std::int32_t>(plan_.insts.size()) && !plan_.insts[j].leader;
             ++j)
            resolve = plan_.insts[j].flow != Flow::normal;
        if (resolve) alias_resolve();
    }
    // tier 2: GPR copy aliases. A MOV may become one; anything that writes
    // an alias or its host's owner (also in the next few words: core fusions
    // consume them), control flow and family instructions resolve them first.
    if (galiasing_ && pi.inst.kind == Kind::logic_shift && emit_gpr_copy(pi)) {
        end_instruction();
        after_last_ = index;
        emit_fallthrough(index, 1);
        return;
    }
    if (galias_count_ != 0) {
        const auto touched = galias_touched();
        bool resolve = pi.flow != Flow::normal || pi.inst.kind == Kind::unknown || pi.conservative_writes ||
                       (pi.writes & touched) != 0;
        for (std::int32_t j = index + 1; !resolve && j <= index + 3 && j < static_cast<std::int32_t>(plan_.insts.size()) &&
                                         !plan_.insts[static_cast<std::size_t>(j)].leader;
             ++j)
            resolve = (plan_.insts[static_cast<std::size_t>(j)].writes & touched) != 0 ||
                      plan_.insts[static_cast<std::size_t>(j)].flow != Flow::normal;
        if (resolve) {
            // An aliased BR/BLR/RET target takes its host's entry over first: with no
            // spare register the resolve would store the alias and the exit reload it.
            const auto k = pi.inst.kind;
            if ((k == Kind::branch_reg || k == Kind::branch_link_reg || k == Kind::ret) && pi.inst.rn < 31 &&
                galias_host_[pi.inst.rn] >= 0 && codegen_on(knob_no_alias_target))
                galias_promote(pi.inst.rn);
            galias_resolve();
        }
    }
    if (pi.flow != Flow::normal) alias_forget_knowledge();
    if (slot_alias_count_ != 0 && (pi.may_fault || pi.conservative_writes || pi.flow != Flow::normal)) slot_aliases_all();
    if ((pi.flow == Flow::jump || pi.flow == Flow::cond) && pi.table < 0) record_edges(index);   // tier 2, pass 1
    // Calls, returns, traps and interpret exits leave the region: store the
    // dirty cache. Branches inside loops handle it per edge (dirty entries
    // stay in registers across blocks); elsewhere they store it first, which
    // keeps exit paths short (clean entries are still carried). Tier 2's
    // second pass: every edge stores what its target's entry state drops.
    if (dirty_crosses_branch(index) || (pi.flow == Flow::jump && pi.table < 0 && pi.taken >= 0 && dirty_into_merge(pi.taken))) {
        // Both successors take over the cache as it is (single predecessor,
        // or an exit stub that stores the dirty snapshot): no stores here.
    } else if (alloc_pass_ == 2 && !carry_clean_ && (pi.flow == Flow::jump || pi.flow == Flow::cond) && pi.table < 0) {
        // Tier 2: dirty values stay in registers only along back edges whose
        // loop header keeps them dirty there (stores move out of the loop);
        // every other one is stored here, as with clean carrying, unless no
        // successor observes it again.
        std::uint32_t keep_g = 0, keep_v = 0;
        std::uint64_t dead = ~std::uint64_t{0};
        carried_dirty(index, keep_g, keep_v);
        plan_.for_each_successor(index, [&](std::int32_t t) {
            dead &= dead_at(t);
            const auto slot = s_.cache_slot[static_cast<std::size_t>(t)];
            if (t > index || !plan_.insts[static_cast<std::size_t>(t)].leader || slot < 0) return;
            keep_matching(s_.cache_snaps[static_cast<std::size_t>(slot)], keep_g, keep_v);
        });
        if (pi.flow == Flow::cond ? (pi.taken < 0 || pi.fall < 0) : pi.taken < 0) dead = 0;   // an exit observes everything
        flush_dirty_except(keep_g, keep_v, dead);
    } else if (pi.flow != Flow::normal &&
               (!carry_ || carry_clean_ || (pi.flow != Flow::jump && pi.flow != Flow::cond) ||
                s_.loop_of[static_cast<std::size_t>(index)] < 0)) {
        flush_dirty();
    }
    if (pi.inst.kind != Kind::unknown) {
        const auto consumed = emit_core(pi);
        if (consumed > 1) skip_until_ = index + static_cast<std::int32_t>(consumed);
        end_instruction();
        after_last_ = index + static_cast<std::int32_t>(consumed) - 1;
        // Calls, returns, BR and traps end with a jump: nothing falls through
        // (an inlined call or RET has Flow::normal / Flow::jump).
        if (consumed == 1 && (pi.flow == Flow::call || pi.flow == Flow::indirect || pi.flow == Flow::svc ||
                              pi.flow == Flow::trap))
            no_fall_at_ = a_.size();
        if (consumed > 1 || pi.flow != Flow::normal) alias_forget_knowledge();   // tier 2: edges may move hosts
        if (pi.flow == Flow::normal) {
            emit_fact_checks(index, consumed);
            emit_fallthrough(index, consumed);
        }
        out_.stats.guest_instructions += consumed - 1;
        return;
    }
    if (pi.try_family || pi.family >= 0) {
        if (aliasing_ && emit_vec_copy(pi)) {   // tier 2: MOV Vd.16B, Vn.16B
            end_instruction();
            emit_fallthrough(index, 1);
            return;
        }
        const auto consumed = try_families(index);
        if (consumed != 0) {
            end_instruction();
            after_last_ = index + static_cast<std::int32_t>(consumed) - 1;
            emit_fact_checks(index, consumed);
            emit_fallthrough(index, consumed);
            if (consumed > 1) skip_until_ = index + static_cast<std::int32_t>(consumed);
            out_.stats.guest_instructions += consumed - 1;
            return;
        }
    }
    reset_scratch();
    pending_gpr_count_ = 0;
    pending_vec_count_ = 0;
    family_mode_ = false;
    emit_interpret_exit(index);
    reset_scratch();
    no_fall_at_ = a_.size();
}

void RegionEmitter::emit_stubs() {
    for (std::size_t k = 0; k < stubs_.size(); ++k) {
        const auto s = stubs_[k];
        a_.bind(s.label);
        set_lent(s.lent);   // tier 2: the pins lent where the stub was taken
        switch (s.k) {
        case Stub::K::direct_exit:
            spill_snap(s.snap);   // before materialize_state clobbers rax
            emit_direct_exit(s.pc, s.dirty, s.flags, !s.boundary);
            break;
        case Stub::K::edge: {
            const auto to = lent_at(s.target);   // tier 2: pin lending changes on the edge
            lend_enter(s.lent, to);
            if (s.snap >= 0) {   // reconcile the cache at the branch to the target's entry state
                const auto from = s_.cache_snaps[static_cast<std::size_t>(s.snap)];
                gc_ = from.gpr;
                vc_ = from.vec;
                const auto e = s_.cache_snaps[static_cast<std::size_t>(s_.cache_slot[static_cast<std::size_t>(s.target)])];
                reconcile(e, false, dead_at(s.target));
            }
            materialize_state(s.flags, false, edge_raw(s.target));   // rax is never in a merge point's entry state
            lend_leave(s.lent, to);
            a_.jmp(label_of(s.target));
            break;
        }
        case Stub::K::interrupt:
            spill_snap(s.snap);
            materialize_state(s.flags, false);
            exit_pins(s.dirty, !s.boundary);
            emit_exit_with_reason(s.pc, ExitReason::interrupt, 0, false);
            break;
        case Stub::K::return_trampoline:
            // RSB entry of a call whose continuation is not in this region:
            // entered by RET in the flushed state, a linkable direct exit
            // (one jmp to the continuation's region once linked).
            if (plan_.lookup_return_stubs) {   // notier: as before tiering
                fixup_imm64(Gp::rax, s.pc);
                a_.mov(64, ptr(Gp::r15, kPcDisp), Gp::rax);
                a_.jmp(ctx_mem(offsetof(GuestContext, stub_indirect_live)));
                break;
            }
            emit_direct_exit(s.pc, 0, FlagsState::ctx, false);   // entered by RET in the boundary state
            break;
        case Stub::K::tier_up:
            emit_exit_with_reason(s.pc, ExitReason::tier_up, 0, false);
            break;
        case Stub::K::region_tier_up:   // the region's loops are hot: exit at the header (like an interrupt)
            spill_snap(s.snap);
            materialize_state(s.flags, false);
            exit_pins(s.dirty, !s.boundary);
            fixup_imm64(Gp::rax, plan_.start);
            a_.mov(64, ctx_mem(offsetof(GuestContext, exit_arg)), Gp::rax);
            emit_exit_with_reason(s.pc, ExitReason::tier_up_loop, 0, false);
            break;
        case Stub::K::loop_tier_up:   // a loop header of counted code (tier 2), like an interrupt
            spill_snap(s.snap);
            materialize_state(s.flags, false);
            exit_pins(s.dirty, !s.boundary);
            a_.mov(32, ctx_mem(offsetof(GuestContext, spill) + kLoopCounterSlot * 8),
                   static_cast<std::int32_t>(std::max(1u, std::min(options_.tier_threshold, 0x7fffffffu))));
            emit_exit_with_reason(s.pc, ExitReason::tier_up, 0, false);
            break;
        case Stub::K::exit_tail:   // unlinked tail of an in-line exit jmp (s.field: its opcode)
            fixup_imm64(Gp::rax, s.pc);
            a_.mov(64, ptr(Gp::r15, kPcDisp), Gp::rax);
            a_.lea(64, Gp::rax, x64::rip(s.field, 1));
            a_.jmp(ctx_mem(offsetof(GuestContext, stub_link)));
            break;
        case Stub::K::lookup:      // target in rcx (s.target = -1) or host register s.target, state flushed
            if (s.target >= 0) a_.mov(64, Gp::rcx, x64::gp(static_cast<unsigned>(s.target)));
            emit_indirect_jump();
            break;
        case Stub::K::indirect_exit:
            exit_pins(s.dirty);
            emit_indirect_jump();
            break;
        case Stub::K::reason_exit:
            exit_pins(s.dirty);
            emit_exit_with_reason(s.pc, s.reason, s.arg, s.has_arg);
            break;
        case Stub::K::entry:
        case Stub::K::entry_jump: break;
        }
    }
    for (std::size_t k = 1; k < plan_.entries.size(); ++k) {
        if (s_.entry_inline[k]) continue;   // laid out in the body (start_block)
        a_.align(16);
        a_.bind(entry_labels_[k]);
        normalize_entry_flags(plan_.entries[k].index);
        set_lent(lent_at(plan_.entries[k].index));
        entry_pins(plan_.entries[k].index);
        load_entry_state(plan_.entries[k].index);
        a_.jmp(label_of(plan_.entries[k].index));
    }
}

void RegionEmitter::run() {
    const auto n = static_cast<std::int32_t>(plan_.insts.size());
    inherited_.assign(static_cast<std::size_t>(n), FlagsState::ctx);
    s_.cache_slot.assign(static_cast<std::size_t>(n), -1);
    s_.cache_snaps.clear();
    s_.loop_end.assign(static_cast<std::size_t>(n), -1);
    for (std::int32_t i = 0; i < n; ++i)
        plan_.for_each_successor(i, [&](std::int32_t t) {
            if (t <= i) s_.loop_end[static_cast<std::size_t>(t)] = std::max(s_.loop_end[static_cast<std::size_t>(t)], i);
        });
    // Flags read before written inside the region (backward, to a fixed
    // point): the plan's liveness without the exits.
    s_.flags_read.assign(static_cast<std::size_t>(n), 0);
    for (bool changed = true; changed;) {
        changed = false;
        for (std::int32_t i = n - 1; i >= 0; --i) {
            const auto& pi = plan_.insts[static_cast<std::size_t>(i)];
            bool out = false;
            plan_.for_each_successor(i, [&](std::int32_t t) { out = out || s_.flags_read[static_cast<std::size_t>(t)] != 0; });
            const std::uint8_t in = pi.reads_flags || (out && !pi.writes_flags);
            if (in != s_.flags_read[static_cast<std::size_t>(i)]) {
                s_.flags_read[static_cast<std::size_t>(i)] = in;
                changed = true;
            }
        }
    }
    // Innermost loop (layout range [header, last back-edge source]) of each instruction.
    s_.loop_of.assign(static_cast<std::size_t>(n), -1);
    s_.loop_slot.assign(static_cast<std::size_t>(n), -1);
    s_.loops.clear();
    s_.pend.clear();
    s_.pend_head.assign(static_cast<std::size_t>(n), -1);
    for (std::int32_t h = 0; h < n; ++h)
        for (std::int32_t i = h; i <= s_.loop_end[static_cast<std::size_t>(h)]; ++i) s_.loop_of[static_cast<std::size_t>(i)] = h;
    labels_.assign(static_cast<std::size_t>(n), Label{});
    entry_labels_.clear();
    for (std::size_t k = 0; k < plan_.entries.size(); ++k) entry_labels_.push_back(a_.new_label());
    s_.entry_inline.assign(plan_.entries.size(), 0);
    cold_ = false;
    no_fall_at_ = ~std::size_t{0};
    out_.guest_start = plan_.start;
    reset_scratch();
    forget_vec_zero();       // fp/neon
    vzero_prepare();         // p3: facts across labels
    cache_on_ = plan_.reg_cache;
    bool_reader_ = -1;       // e_ccmp fusion
    bool_in_slot_ = false;
    flags_saved_ = false;    // save_flags_around
    write_back_ = cache_on_ && plan_.write_back;
    carry_ = cache_on_ && plan_.carry;
    carry_clean_ = plan_.carry_clean;
    belady_ = plan_.belady;
    branch_dirty_ = codegen_on(knob_no_branch_dirty);
    merge_dirty_ = (codegen_p2_knob_bits() & knob_merge_dirty) != 0;
    // Tier 2 (region_alloc.cpp): liveness, dropped interrupt checks; pass 2
    // starts every block that has a preset with it, and dirty values cross
    // edges (each edge stores what its target does not keep).
    if (!plan_.optimize || !carry_ || !write_back_) alloc_pass_ = 0;
    dead_on_ = plan_.optimize && plan_.t2_dead && write_back_ && plan_.fault_maps && plan_.wb_fault_flush;
    in_instruction_ = false;
    after_last_ = 0;
    s_.edge_recs.clear();
    set_lent(0);
    if (plan_.optimize) {
        compute_tier2();
    } else {
        s_.no_check.assign(static_cast<std::size_t>(n), 0);
        s_.lent.clear();
    }
    if (alloc_pass_ == 2) {
        carry_clean_ = false;
        for (std::int32_t i = 0; i < n && static_cast<std::size_t>(i) < s_.preset_of.size(); ++i)
            if (const auto p = s_.preset_of[static_cast<std::size_t>(i)]; p >= 0) {
                s_.cache_slot[static_cast<std::size_t>(i)] = static_cast<std::int32_t>(s_.cache_snaps.size());
                s_.cache_snaps.push_back(s_.presets[static_cast<std::size_t>(p)]);
            }
    }
    fault_flush_ = plan_.wb_fault_flush;
    fault_maps_ = write_back_ && plan_.fault_maps;
    out_.fault_maps.clear();
    out_.fault_map_regs.clear();
    reset_cache(gc_);
    reset_cache(vc_);
    aliasing_ = plan_.vec_alias && write_back_;   // tier 2
    std::memset(valias_host_, -1, sizeof valias_host_);
    std::memset(vslot_alias_, -1, sizeof vslot_alias_);
    slot_alias_count_ = 0;
    std::memset(valias_of_, 0, sizeof valias_of_);
    std::memset(vslot_, 0, sizeof vslot_);
    valias_dirty_ = 0;
    alias_count_ = 0;
    galiasing_ = plan_.t2_galias && write_back_;   // tier 2: GPR copy aliases
    std::memset(galias_host_, -1, sizeof galias_host_);
    std::memset(galias_of_, 0, sizeof galias_of_);
    galias_dirty_ = 0;
    galias_count_ = 0;
    neon_begin_region(a_);   // per-region constant pool (no-op without the neon family)
    if (auto* map = instruction_map_sink()) map->clear();   // diagnostics: this attempt only

    // Region start entry (offset 0).
    a_.bind(entry_labels_[0]);
    if (options_.interrupt_checks) {
        a_.cmp(32, ctx_mem(offsetof(GuestContext, interrupt_pending)), 0);
        a_.jcc(Cond::ne, add_stub(Stub::K::interrupt, FlagsState::ctx, 0, plan_.start, -1, -1, true));
    }
    // Tier 0: count region-start entries (flushed state: EFLAGS are free).
    // dec dword [rip + rel32]; jz tier_up. The rel32 is patched below to the
    // region's own slot; the runtime may repoint it.
    std::uint32_t counter_field = 0;
    // Tier 0 (counted): a region cut short counts its entries; with tier 2,
    // a complete region counts the iterations of its loops (one without
    // loops is final), or, with t2entries, its entries.
    bool has_loops = false;
    for (std::int32_t i = 0; i < n && !has_loops; ++i) has_loops = plan_.insts[static_cast<std::size_t>(i)].loop_header;
    const bool entry_counter =
        options_.lazy && (plan_.truncated || (plan_.count_hot && !plan_.count_loop_headers));
    loop_counting_ = options_.lazy && !entry_counter && plan_.count_loop_headers && has_loops && options_.interrupt_checks;
    loop_counter_fields_.clear();
    const bool tier0 = entry_counter || loop_counting_;
    if (entry_counter) {
        a_.db(0xff);
        a_.db(0x0d);
        counter_field = static_cast<std::uint32_t>(a_.size());
        a_.dd(0);
        a_.jcc(Cond::e, add_stub(Stub::K::tier_up, FlagsState::ctx, 0, plan_.start, -1, -1, true));
    }
    const auto start_index = plan_.entries[0].index;
    set_lent(lent_at(start_index));   // tier 2: the start block may lie in a loop that lends pins
    normalize_entry_flags(start_index);
    entry_pins(start_index);
    jump_edge(start_index, -1, start_index == 0);   // from the empty cache (may load a loop's registers)

    prepare_facts();
    for (std::int32_t i = 0; i < n; ++i) {
        if (i < skip_until_) continue;
        try {
            emit_instruction(i);
        } catch (const std::logic_error& error) {
            // An emitter invariant: name the guest instruction, so the stop can be reproduced in a unit test.
            const auto& pi = plan_.insts[static_cast<std::size_t>(i)];
            char where[64];
            std::snprintf(where, sizeof(where), " (instruction 0x%08x at 0x%llx)", pi.inst.word, static_cast<unsigned long long>(pi.pc));
            throw std::logic_error(std::string(error.what()) + where);
        }
        vzero_record(skip_until_ > i ? skip_until_ - 1 : i);   // p3: the facts on this instruction's edges
    }
    vzero_finish();
    // A fusion relied on a checked flag: its load must have emitted the check
    // (it always does when it is translated: an interpreted load leaves the
    // region, so the fusion is unreachable from it), and no helper that may
    // run other guest code may share the region (it could reuse the flag).
    for (const auto& c : s_.fact_checks)
        if ((c.kind == 3 ? s_.derived[c.flag].handed_out : s_.fact_flags[c.flag].handed_out) && (!c.emitted || facts_helper_))
            throw RetryTranslation{plan_.start, false, RetryTranslation::Resource::facts};
    cur_ = nullptr;
    written_ = 0;
    vwritten_ = 0;
    // Everything after this point is the cold section (TranslationStats::hot_bytes).
    const auto hot_bytes = static_cast<std::uint32_t>(a_.size());
    cold_ = true;
    emit_stubs();
    const auto pool_begin = a_.size();
    neon_end_region(a_);     // after the last unconditional jump: never executed
    const auto pool_end = a_.size();
    emit_host_tables();      // switch dispatch data (never executed)
    std::uint32_t counter_slot = 0;
    if (entry_counter) {
        a_.align(4);
        counter_slot = static_cast<std::uint32_t>(a_.size());
        a_.dd(std::max(1u, options_.tier_threshold));
    } else if (loop_counting_) {
        // A cache line of its own: 64 bytes on either side hold no code (the
        // region's start is only 16-byte aligned), so the counter's stores
        // never invalidate fetched code. Iterations: 16x the entry threshold.
        for (unsigned k = 0; k < 16; ++k) a_.dd(0);
        counter_slot = static_cast<std::uint32_t>(a_.size());
        a_.dd(static_cast<std::uint32_t>(std::clamp<std::uint64_t>(16ull * std::max(1u, options_.tier_threshold), 1, 0x7fffffffu)));
        for (unsigned k = 0; k < 15; ++k) a_.dd(0);
    }
    a_.finalize();
    patch_host_tables();
    // The constant pool is data: mark it as literals so the persistent cache's
    // relocation audit does not read adjacent constants (e.g. 0x80808080 then
    // 1) as module addresses.
    if (pool_end - pool_begin >= 8) {
        const auto mark = [&](std::size_t at) {
            std::uint64_t value;
            std::memcpy(&value, a_.data() + at, 8);
            out_.fixups.push_back({static_cast<std::uint32_t>(at), FixupKind::literal_imm64, value});
        };
        std::size_t at = pool_begin;
        for (; at + 8 <= pool_end; at += 8) mark(at);
        if (at != pool_end) mark(pool_end - 8);
    }

    out_.entries.clear();
    out_.entries.push_back({plan_.start, 0});
    for (std::size_t k = 1; k < plan_.entries.size(); ++k)
        out_.entries.push_back({plan_.insts[plan_.entries[k].index].pc,
                                static_cast<std::uint32_t>(a_.offset_of(entry_labels_[k]))});
    out_.guest_ranges.clear();
    for (const auto& pi : plan_.insts) {
        if (!out_.guest_ranges.empty() && out_.guest_ranges.back().second == pi.pc) out_.guest_ranges.back().second += 4;
        else out_.guest_ranges.emplace_back(pi.pc, pi.pc + 4);
    }
    if (plan_.inlined_calls != 0) {
        // Inlined callee copies (SMC invalidation covers them): sorted, merged.
        auto& r = out_.guest_ranges;
        std::sort(r.begin(), r.end());
        std::size_t k = 0;
        for (std::size_t i = 1; i < r.size(); ++i) {
            if (r[i].first <= r[k].second) r[k].second = std::max(r[k].second, r[i].second);
            else r[++k] = r[i];
        }
        r.resize(k + 1);
        core_counters().inlined_calls.fetch_add(plan_.inlined_calls, std::memory_order_relaxed);
    }
    out_.pins.clear();
    for (unsigned g = 0; g < 32; ++g)
        if (pinned(g)) out_.pins.push_back({static_cast<std::uint8_t>(g), static_cast<std::uint8_t>(x64::id(pin(g))), false});
    for (unsigned v = 0; v < 32; ++v)
        if (vpinned(v)) out_.pins.push_back({static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(x64::id(vpin(v))), true});
    out_.stats.blocks = plan_.blocks;
    out_.stats.hot_bytes = hot_bytes;
    out_.code = std::move(a_.bytes());
    out_.tier = tier0 ? 0 : 1;
    out_.tier_counter = counter_field;   // 0 with loop counting (the runtime repoints entry counters only)
    out_.tier_counter_slot = counter_slot;
    if (entry_counter) {
        const auto rel = static_cast<std::int32_t>(counter_slot) - static_cast<std::int32_t>(counter_field + 4);
        std::memcpy(out_.code.data() + counter_field, &rel, 4);
    }
    for (const auto field : loop_counter_fields_) {
        const auto rel = static_cast<std::int32_t>(counter_slot) - static_cast<std::int32_t>(field + 4);
        std::memcpy(out_.code.data() + field, &rel, 4);
    }
}

// ---- EmitContext ----------------------------------------------------------------------------

Gp RegionEmitter::read_gpr(unsigned index, bool sp) {
    if (folded(index)) return fold_value();   // neon (additive): folded ADD/SUB result
    const auto v = val(index, sp);
    if (v.is_reg()) return v.r;
    if (v.is_mem()) return read_unpinned(v.g);
    const auto t = alloc();
    a_.mov_imm(t, 0);
    return t;
}

Gp RegionEmitter::write_gpr(unsigned index, bool sp) {
    if (folded(index)) afold_.active = false;   // neon (additive)
    return dst(index, sp);
}

Gp RegionEmitter::copy_gpr(unsigned index, bool sp) {
    if (folded(index)) return fold_value();   // neon (additive): a fresh temp
    const auto v = val(index, sp);
    const auto t = alloc();
    if (v.is_zero()) a_.mov_imm(t, 0);
    else load(64, t, v);
    return t;
}

Gp RegionEmitter::temp_gpr() { return alloc(); }

x64::Xmm RegionEmitter::read_vec(unsigned index) {
    if (vpinned(index)) return vpin(index);
    if (const auto h = borrow_vec(index); x64::id(h) < 16) return h;
    const bool cacheable = may_cache();
    if (const auto source = vslot_alias_[index]; source >= 0) {
        // tier 2: the value is in another slot. Loaded, it is the guest's own
        // (dirty) entry: its slot is older.
        const auto x = alloc_vec();
        vmove(x, ptr(Gp::r15, v_disp(static_cast<unsigned>(source))));
        vslot_[x64::id(x)] = 1u << static_cast<unsigned>(source);
        if (cacheable) {
            vslot_alias_[index] = -1;
            --slot_alias_count_;
            --alias_count_;
            cache_set(vc_, index, x64::id(x));
            vc_.dirty |= 1u << x64::id(x);
            fault_reopen();
        }
        return x;
    }
    const auto x = alloc_vec();
    vmove(x, ptr(Gp::r15, v_disp(index)));
    vslot_[x64::id(x)] = 1u << index;
    if (cacheable) cache_set(vc_, index, x64::id(x));
    return x;
}

EmitContext::VecSrc RegionEmitter::vsrc(unsigned index) {
    if (vpinned(index)) return {vpin(index), {}, false};
    if (const auto h = borrow_vec(index); x64::id(h) < 16) return {h, {}, false};
    // Read again soon: load it once into a cached register (if one is spare).
    if (may_cache() && std::popcount(free_xmm_) >= 4 && vec_read_soon(index)) return {read_vec(index), {}, false};
    // tier 2: a slot alias is loaded. Its source slot could be overwritten
    // later in this instruction (an eviction of the source's newer value).
    if (vslot_alias_[index] >= 0) return {read_vec(index), {}, false};
    return {{}, ptr(Gp::r15, v_disp(index)), true};
}

EmitContext::GprSrc RegionEmitter::gsrc(unsigned index, bool sp) {
    if (folded(index)) return {fold_value(), {}, false};   // neon (additive)
    const auto v = val(index, sp);
    if (v.is_reg()) return {v.r, {}, false};
    if (v.is_zero()) return {read_gpr(index, sp), {}, false};
    if (const auto h = borrow_gpr(v.g); h != Gp::none) return {h, {}, false};
    return {Gp::none, gmem(v.g), true};
}

x64::Xmm RegionEmitter::write_vec(unsigned index) {
    vzero32_ &= ~(1u << index);   // fp/neon: re-noted by the writer if known
    vzero64_ &= ~(1u << index);
    vzwritten_ |= 1u << index;    // p3: written in this block (vzero_record)
    if (vpinned(index)) {
        vwritten_ |= 1u << index;
        return vpin(index);
    }
    if (pending_vec_count_ >= std::size(pending_vec_)) throw RetryTranslation{pc_, false};
    x64::Xmm x;
    if (reusable_vec(index)) {
        // read_vec/vsrc operands may alias a destination of the same guest
        // register, just like a pinned destination. Reserving that host here
        // avoids evicting another live vector just to hold the new result.
        // A faulting instruction must instead keep its old value intact.
        const auto h = static_cast<unsigned>(vc_.host[index]);
        x = x64::xmm(h);
        free_xmm_ &= ~(1u << h);
        cache_drop_guest(vc_, index);
    } else {
        x = alloc_vec();
    }
    pending_vec_[pending_vec_count_++] = {static_cast<std::uint8_t>(index), x};
    return x;
}

x64::Xmm RegionEmitter::temp_vec() { return alloc_vec(); }

// fp/neon (additive): see EmitContext::vec_zero_above.
void RegionEmitter::note_vec_zero_above(unsigned index, unsigned width) {
    if (index >= 32) return;
    // Only for a register this instruction wrote (write_vec forgot it).
    bool written = ((vwritten_ >> index) & 1u) != 0;
    for (unsigned i = 0; i < pending_vec_count_ && !written; ++i) written = pending_vec_[i].guest == index;
    if (!written) return;
    if (width <= 32) vzero32_ |= 1u << index;
    if (width <= 64) vzero64_ |= 1u << index;
}

// ---- p3: zero-above facts across labels (Plan::vzero_join) -------------------------------------

void RegionEmitter::vzero_prepare() {
    const auto n = plan_.insts.size();
    s_.vz32.assign(n, 0);
    s_.vz64.assign(n, 0);
    s_.vz_w.assign(n, 0);
    s_.vz_blk.assign(n, -1);
    s_.vz_rec.assign(n, 0);
    vzblock_ = -1;
    vzsolved_ = false;
    if (alloc_pass_ != 2) {   // pass 2 reads pass 1's records (vzero_finish)
        s_.vz1_32.clear();
        s_.vz1_64.clear();
        s_.vz1_w.clear();
        s_.vz1_blk.clear();
        s_.vz1_rec.clear();
    }
    if (!plan_.vzero_join) return;
    s_.vz_entry.assign(n, 0);
    for (const auto& e : plan_.entries)
        if (e.index >= 0 && static_cast<std::size_t>(e.index) < n) s_.vz_entry[static_cast<std::size_t>(e.index)] = 1;
    s_.vz_pred_head.assign(n, -1);
    s_.vz_pred_next.clear();
    s_.vz_pred_from.clear();
    for (std::int32_t j = 0; j < static_cast<std::int32_t>(n); ++j) {
        if (plan_.insts[static_cast<std::size_t>(j)].entry) s_.vz_entry[static_cast<std::size_t>(j)] = 1;
        plan_.for_each_successor(j, [&](std::int32_t t) {
            if (t < 0 || static_cast<std::size_t>(t) >= n) return;
            s_.vz_pred_from.push_back(j);
            s_.vz_pred_next.push_back(s_.vz_pred_head[static_cast<std::size_t>(t)]);
            s_.vz_pred_head[static_cast<std::size_t>(t)] = static_cast<std::int32_t>(s_.vz_pred_from.size() - 1);
        });
    }
    if (alloc_pass_ == 2 && s_.vz1_rec.size() == n) vzero_solve();
}

// Tier 2, before pass 2: the facts at every block start, as the greatest
// fixpoint of in[b] = AND over predecessors p of out(p), out(p) = (in[block of
// p] & ~W[p]) | (G[p] & W[p]) from pass 1's records: W[p] the V registers
// written in p's block up to p, G[p] the facts there (for written registers a
// lower bound: computed with pass 1's knowledge). Region entries and blocks
// with a predecessor without a record start with none. Loop-invariant values
// keep their facts across back edges.
void RegionEmitter::vzero_solve() {
    const auto n = s_.vz1_rec.size();
    s_.vz_in32.assign(n, ~0u);
    s_.vz_in64.assign(n, ~0u);
    for (std::size_t t = 0; t < n; ++t)
        if (s_.vz_entry[t] || !plan_.insts[t].leader) s_.vz_in32[t] = s_.vz_in64[t] = 0;
    for (unsigned round = 0; round < 64; ++round) {
        bool changed = false;
        for (std::size_t t = 0; t < n; ++t) {
            if (s_.vz_entry[t] || !plan_.insts[t].leader) continue;
            std::uint32_t z32 = ~0u, z64 = ~0u;
            bool any = false;
            for (auto e = s_.vz_pred_head[t]; e >= 0; e = s_.vz_pred_next[static_cast<std::size_t>(e)]) {
                const auto p = static_cast<std::size_t>(s_.vz_pred_from[static_cast<std::size_t>(e)]);
                any = true;
                if (!s_.vz1_rec[p]) {
                    z32 = z64 = 0;
                    break;
                }
                const auto b = s_.vz1_blk[p];
                const std::uint32_t in32 = b >= 0 ? s_.vz_in32[static_cast<std::size_t>(b)] : 0u;
                const std::uint32_t in64 = b >= 0 ? s_.vz_in64[static_cast<std::size_t>(b)] : 0u;
                const auto w = s_.vz1_w[p];
                z32 &= (in32 & ~w) | (s_.vz1_32[p] & w);
                z64 &= (in64 & ~w) | (s_.vz1_64[p] & w);
            }
            if (!any) z32 = z64 = 0;
            z64 |= z32;
            if (z32 != s_.vz_in32[t] || z64 != s_.vz_in64[t]) {
                s_.vz_in32[t] = z32;
                s_.vz_in64[t] = z64;
                changed = true;
            }
        }
        if (!changed) {
            vzsolved_ = true;
            return;
        }
    }
    // No fixpoint within the bound: keep the forward join (never wrong).
}

void RegionEmitter::vzero_record(std::int32_t last) {
    if (!plan_.vzero_join || last < 0 || static_cast<std::size_t>(last) >= s_.vz_rec.size()) return;
    const auto i = static_cast<std::size_t>(last);
    s_.vz32[i] = vzero32_;
    s_.vz64[i] = vzero64_;
    s_.vz_w[i] = vzwritten_;
    s_.vz_blk[i] = vzblock_;
    s_.vz_rec[i] = 1;
}

void RegionEmitter::vzero_block_start(std::int32_t index) {
    forget_vec_zero();
    vzwritten_ = 0;
    vzblock_ = index;
    const auto i = static_cast<std::size_t>(index);
    if (!plan_.vzero_join || i >= s_.vz_entry.size() || s_.vz_entry[i]) return;
    if (vzsolved_) {   // tier 2, pass 2
        vzero32_ = s_.vz_in32[i];
        vzero64_ = s_.vz_in64[i];
        return;
    }
    // Forward predecessors already emitted in this pass (a back edge: none).
    std::uint32_t z32 = ~0u, z64 = ~0u;
    bool any = false;
    for (auto e = s_.vz_pred_head[i]; e >= 0; e = s_.vz_pred_next[static_cast<std::size_t>(e)]) {
        const auto p = static_cast<std::size_t>(s_.vz_pred_from[static_cast<std::size_t>(e)]);
        if (p >= i || !s_.vz_rec[p]) return;
        z32 &= s_.vz32[p];
        z64 &= s_.vz64[p];
        any = true;
    }
    if (!any) return;   // no internal predecessor
    vzero32_ = z32;
    vzero64_ = z64 | z32;
}

void RegionEmitter::vzero_finish() {
    if (alloc_pass_ != 1 || !plan_.vzero_join) return;
    s_.vz1_32 = s_.vz32;
    s_.vz1_64 = s_.vz64;
    s_.vz1_w = s_.vz_w;
    s_.vz1_blk = s_.vz_blk;
    s_.vz1_rec = s_.vz_rec;
}

void RegionEmitter::clobber_host_flags() { prepare_clobber(); }

void RegionEmitter::set_flags(FlagsProducer producer) {
    switch (producer) {
    case FlagsProducer::sub: flags_ = FlagsState::host_sub; break;
    case FlagsProducer::add: flags_ = FlagsState::host_add; break;
    case FlagsProducer::logic: flags_ = FlagsState::host_logic; break;
    }
}

void RegionEmitter::set_nzcv(Gp value) {
    a_.mov(64, ptr(Gp::r15, kNzcvDisp), value);
    flags_ = FlagsState::ctx;
}

// ---- FCMP fusion --------------------------------------------------------------------------

// ARM condition of the next instruction in this block that reads the flags,
// if it is a B.cond/CSEL-class/CCMP or FCSEL/FCCMP reached without another
// flags writer or a block end in between; -1 otherwise.
int RegionEmitter::next_flags_condition() const {
    const auto n = static_cast<std::int32_t>(plan_.insts.size());
    for (std::int32_t j = index_ + 1; j < n && j <= index_ + 8; ++j) {
        const auto& pj = plan_.insts[j];
        if (pj.leader) return -1;
        if (pj.reads_flags) {
            switch (pj.inst.kind) {
            case Kind::branch_cond:
            case Kind::csel:
            case Kind::ccmp: return pj.inst.cond < 14 ? static_cast<int>(pj.inst.cond) : -1;
            default: break;
            }
            const auto w = pj.inst.word;
            if ((w & 0xff200c00u) == 0x1e200c00u || (w & 0xff200c00u) == 0x1e200400u)   // FCSEL / FCCMP
                return static_cast<int>((w >> 12) & 15u);
            return -1;
        }
        if (pj.writes_flags || pj.flow != Flow::normal) return -1;
    }
    return -1;
}

bool RegionEmitter::fcmp_swap() {
    if (!plan_.fcmp_fusion) return false;
    switch (next_flags_condition()) {
    case 2: case 3: case 4: case 5: case 8: case 9: return true;   // CS CC MI PL HI LS
    default: return false;
    }
}

bool RegionEmitter::set_fcmp_flags(bool swapped) {
    if (!plan_.fcmp_fusion) return false;
    flags_ = swapped ? FlagsState::host_fcmp_swapped : FlagsState::host_fcmp;
    return true;
}

// B.cond over fused FCMP flags whose condition needs the parity flag too.
void RegionEmitter::cond_branch_fcmp(const FcmpCond& fc, std::int32_t taken, std::uint64_t taken_pc,
                                     std::int32_t fall, std::uint64_t fall_pc) {
    if (fc.k == FcmpCond::K::single) {
        cond_branch(fc.c, taken, taken_pc, fall, fall_pc);
        return;
    }
    const auto state = flags_;
    const Label target = taken >= 0 ? taken_label(taken, state)
                                    : add_stub(Stub::K::direct_exit, state, current_dirty(), taken_pc, -1, dirty_snap());
    if (fc.k == FcmpCond::K::and_np) {   // taken iff c and ordered
        const auto skip = a_.new_label();
        a_.jcc_short(Cond::p, skip);
        jcc_label(fc.c, target);
        a_.bind(skip);
    } else {                             // taken iff c or unordered
        jcc_label(Cond::p, target);
        jcc_label(fc.c, target);
    }
    if (fall >= 0) {
        // Tier 2: loop-carried dirty values leave the loop stored.
        if (alloc_pass_ == 2 && !plan_.insts[static_cast<std::size_t>(fall)].leader && leaves_loop(index_, fall))
            flush_dirty(true);
        edge_inline(fall);
        if (fall != index_ + 1) jmp_label(label_of(fall));
    } else {
        flush_dirty(true);
        emit_direct_exit(fall_pc, current_dirty(), flags_);
    }
}

Gp RegionEmitter::get_nzcv() {
    materialize();
    const auto t = alloc();
    a_.mov(32, t, ptr(Gp::r15, kNzcvDisp));
    return t;
}

Cond RegionEmitter::condition(unsigned arm_condition) {
    if (arm_condition >= 14) throw std::logic_error("condition(): AL/NV must be handled by the emitter");
    if (bool_reader_ >= 0 && bool_reader_ == index_) {
        // The reader of a fused CCMP (e_ccmp): its answer is in ZF, or parked
        // in kBoolSlot. NZCV is dead from here on (stale in the context, like
        // dead flags at merge points and calls).
        if (bool_in_slot_) a_.test(8, ptr(Gp::r15, spill_disp(kBoolSlot)), 1);
        bool_reader_ = -1;
        bool_in_slot_ = false;
        flags_ = FlagsState::ctx;
        return Cond::ne;
    }
    return host_condition(arm_condition);
}

Gp RegionEmitter::guest_address(std::uint64_t address) {
    const auto t = alloc();
    fixup_imm64(t, address);   // relocatable: recorded as a guest_address_imm64 fixup
    return t;
}

Gp RegionEmitter::temp_gpr_prefer(Gp preferred) {
    if (preferred == Gp::none || x64::id(preferred) >= 16 || !((kScratchMask >> x64::id(preferred)) & 1u))
        return Gp::none;
    if (!is_free(preferred)) {
        // A borrowed cached register the caller may now save, clobber and
        // restore around an implicit use: stop serving it from the cache.
        evict_gpr(x64::id(preferred));
        return Gp::none;
    }
    take(preferred);
    return preferred;
}

void RegionEmitter::load_gpr(Gp dst, unsigned index, bool sp) {
    if (folded(index)) {   // neon (additive)
        a_.mov(64, dst, fold_value());
        return;
    }
    const auto v = val(index, sp);
    if (v.is_zero()) a_.mov_imm(dst, 0);
    else load(64, dst, v);
}

void RegionEmitter::store_gpr(unsigned index, Gp src, bool sp) {
    if (folded(index)) afold_.active = false;   // neon (additive)
    if (index == 31 && !sp) return;   // XZR
    note_write(index);
    if (pinned(index)) {
        if (pin(index) != src) a_.mov(64, pin(index), src);
    } else {
        cache_drop_guest(gc_, index);   // `src` stays the caller's register
        a_.mov(64, gmem(index), src);   // all faulting accesses are done (rule 2)
    }
}

Gp RegionEmitter::call_helper(unsigned id, std::initializer_list<HelperArg> args, HelperEffects effects) {
    if (pending_gpr_count_ != 0 || pending_vec_count_ != 0) throw RetryTranslation{pc_, false};
    if (afold_.active) throw RetryTranslation{pc_, true};   // neon (additive): no helpers in a folded access
    if (args.size() > 3) throw std::logic_error("call_helper supports at most 3 arguments");
    if (effects == HelperEffects::reads_writes_state) facts_helper_ = true;   // see run(): flag facts
    forget_vec_zero();   // fp/neon: helpers may change guest vector state
    flush_dirty();       // helpers may read guest state from the context
    cache_clear();       // volatile registers and (maybe) guest state change
    if (effects == HelperEffects::pure) {
        prepare_clobber();
    } else {
        materialize();
    }
    unsigned slot = kHelperArgSlot;
    for (const auto& arg : args) {
        if (arg.kind == HelperArg::Kind::host) a_.mov(64, ptr(Gp::r15, spill_disp(slot)), arg.reg);
        ++slot;
    }
    const Gp arg_regs[] = {Gp::rdx, Gp::r8, Gp::r9};
    unsigned i = 0;
    for (const auto& arg : args) {
        const auto r = arg_regs[i];
        switch (arg.kind) {
        case HelperArg::Kind::host: a_.mov(64, r, ptr(Gp::r15, spill_disp(kHelperArgSlot + i))); break;
        case HelperArg::Kind::immediate: mov_literal(r, arg.value); break;
        case HelperArg::Kind::guest_gpr: {
            const auto v = val(static_cast<unsigned>(arg.value), false);
            if (v.is_zero()) a_.mov_imm(r, 0);
            else if (v.is_mem()) a_.mov(64, r, gmem(v.g));
            else a_.mov(64, r, v.r);
            break;
        }
        }
        ++i;
    }
    // Pins in volatile host registers (r10/r11) do not survive the call: their
    // context slots receive the current value and they are reloaded after.
    const std::uint32_t volatile_pins = plan_.pin_hosts & kScratchMask;
    if (effects != HelperEffects::pure) {
        flush_pins(current_dirty());
    } else if (volatile_pins != 0) {
        const auto dirty = static_cast<std::uint32_t>(current_dirty());
        for (unsigned g = 0; g < 32; ++g)
            if (pinned(g) && ((volatile_pins >> x64::id(pin(g))) & 1u) && ((dirty >> g) & 1u)) a_.mov(64, gmem(g), pin(g));
    }
    a_.lea(64, Gp::rcx, ptr(Gp::r15, -kContextBias));
    a_.call(ptr(Gp::r15, helper_disp(id)));
    if (effects == HelperEffects::reads_writes_state) {
        load_pins();
        flags_ = FlagsState::ctx;
    } else if (volatile_pins != 0) {
        for (unsigned g = 0; g < 32; ++g)
            if (pinned(g) && ((volatile_pins >> x64::id(pin(g))) & 1u)) a_.mov(64, pin(g), gmem(g));
    }
    reset_scratch();
    take(Gp::rax);
    return Gp::rax;
}

// ---- vector value facts (EmitContext::vec_fact) ------------------------------------------------

namespace {
constexpr unsigned kMaxFactFlags = 216;   // 16-bit flags in spill slots 0..53
// Tier 2: derived vector values (EmitContext::vec_derived) in GuestContext::vspill.
constexpr unsigned kMaxDerived = 16;
x64::Mem derived_slot(unsigned k) {
    return x64::ptr(x64::Gp::r15, ctx_disp(offsetof(GuestContext, vspill) + 16u * k));
}
constexpr unsigned kMaxFamiliesWithFacts = 8;
}

Mem RegionEmitter::fact_flag(unsigned flag) {
    return ptr(Gp::r15, spill_disp(0) + static_cast<std::int32_t>(2 * flag));
}

// The instructions whose writes of V`vec` reach the start of insts[index]
// (each path through the region from a write to it without another write).
// False if a path comes from a region entry (the state there is unknown) or
// passes an interpret exit or a conservative instruction (unknown writes),
// or if there are more than kMaxDefs.
bool RegionEmitter::reaching_defs(std::int32_t index, unsigned vec, std::int32_t* defs, unsigned& count) {
    count = 0;
    const auto& insts = plan_.insts;
    if (insts[static_cast<std::size_t>(index)].entry) return false;
    if (++s_.visit_stamp == 0) {
        std::fill(s_.visit.begin(), s_.visit.end(), 0u);
        s_.visit_stamp = 1;
    }
    const auto stamp = s_.visit_stamp;
    thread_local std::vector<std::int32_t> stack;
    stack.clear();
    const auto push_preds = [&](std::int32_t j) {
        for (auto e = s_.pred_head[static_cast<std::size_t>(j)]; e >= 0; e = s_.pred_next[static_cast<std::size_t>(e)])
            stack.push_back(s_.pred_from[static_cast<std::size_t>(e)]);
    };
    push_preds(index);
    while (!stack.empty()) {
        const auto p = stack.back();
        stack.pop_back();
        auto& seen = s_.visit[static_cast<std::size_t>(p)];
        if (seen == stamp) continue;
        seen = stamp;
        const auto& pp = insts[static_cast<std::size_t>(p)];
        if (pp.conservative_writes || pp.flow == Flow::interpret) return false;
        if ((pp.vwrites >> vec) & 1u) {
            if (count == kMaxDefs) return false;
            defs[count++] = p;
            continue;
        }
        if (pp.entry) return false;
        push_preds(p);
    }
    return count != 0;
}

void RegionEmitter::prepare_facts() {
    s_.fact_uses.clear();
    s_.fact_flags.clear();
    s_.fact_checks.clear();
    s_.derived.clear();
    facts_helper_ = false;
    facts_on_ = plan_.vec_facts;
    if (!facts_on_) return;
    const auto table = registered_families();
    const FamilyFacts* wanting[kMaxFamiliesWithFacts];
    const FamilyFacts* checker = nullptr;
    unsigned n_wanting = 0;
    for (unsigned f = 0; f < table.count; ++f) {
        const auto* ff = table.families[f].facts;
        if (ff && ff->want && n_wanting < kMaxFamiliesWithFacts) wanting[n_wanting++] = ff;
        if (ff && ff->check && !checker) checker = ff;
    }
    if (n_wanting == 0) return;
    static const bool trace = std::getenv("TRANSMUTE_FX_TRACE_FACTS") != nullptr;   // diagnostics
    const auto n = static_cast<std::int32_t>(plan_.insts.size());
    bool preds_built = false;
    for (std::int32_t i = 0; i < n; ++i) {
        const auto& pi = plan_.insts[static_cast<std::size_t>(i)];
        if (!(pi.try_family || pi.family >= 0) || pi.flow != Flow::normal) continue;
        // The family's view of the code from i (as try_families builds it).
        std::int32_t len = 1;
        while (i + len < n && len < 64 &&
               plan_.insts[static_cast<std::size_t>(i + len)].pc == pi.pc + 4u * static_cast<unsigned>(len) &&
               !plan_.insts[static_cast<std::size_t>(i + len)].leader)
            ++len;
        const std::span<const std::uint32_t> words(plan_.words.data() + i, static_cast<std::size_t>(len));
        for (unsigned f = 0; f < n_wanting; ++f) {
            FamilyFactWant want[4];
            const auto count = std::min(4u, wanting[f]->want(words, want));
            for (unsigned k = 0; k < count; ++k) {
                const auto& w = want[k];
                if (w.vec >= 32) continue;
                if (w.k == FamilyFactWant::K::derived && (!plan_.t2_derived || !wanting[f]->derive)) continue;
                if (!preds_built) {
                    preds_built = true;
                    s_.pred_head.assign(static_cast<std::size_t>(n), -1);
                    s_.pred_next.clear();
                    s_.pred_from.clear();
                    for (std::int32_t j = 0; j < n; ++j)
                        plan_.for_each_successor(j, [&](std::int32_t t) {
                            s_.pred_from.push_back(j);
                            s_.pred_next.push_back(s_.pred_head[static_cast<std::size_t>(t)]);
                            s_.pred_head[static_cast<std::size_t>(t)] = static_cast<std::int32_t>(s_.pred_from.size() - 1);
                        });
                    if (s_.visit.size() < static_cast<std::size_t>(n)) s_.visit.resize(static_cast<std::size_t>(n), 0u);
                }
                std::int32_t defs[kMaxDefs];
                unsigned n_defs = 0;
                const bool known = reaching_defs(i, w.vec, defs, n_defs);
                // What each definition leaves in the register (asked of every family).
                FamilyFactDef kind[kMaxDefs];
                VecValue value[kMaxDefs];
                bool all_match = known, all_masks = known, flaggable = known && checker != nullptr;
                for (unsigned d = 0; known && d < n_defs; ++d) {
                    kind[d] = FamilyFactDef::unknown;
                    for (unsigned g = 0; g < table.count && kind[d] == FamilyFactDef::unknown; ++g)
                        if (const auto* ff = table.families[g].facts; ff && ff->def)
                            kind[d] = ff->def(plan_.words[static_cast<std::size_t>(defs[d])], w.vec, value[d]);
                    all_match &= kind[d] == FamilyFactDef::constant && value[d] == w.value;
                    all_masks &= kind[d] == FamilyFactDef::lane_masks;
                    flaggable &= (kind[d] == FamilyFactDef::constant || kind[d] == FamilyFactDef::load128) &&
                                 plan_.insts[static_cast<std::size_t>(defs[d])].flow == Flow::normal;
                }
                if (trace)
                    std::fprintf(stderr, "facts: region %llx at %llx v%u %s: %u defs%s%s%s\n",
                                 static_cast<unsigned long long>(plan_.start), static_cast<unsigned long long>(pi.pc),
                                 static_cast<unsigned>(w.vec), w.k == FamilyFactWant::K::lane_masks ? "lanes" : "value",
                                 n_defs, known ? "" : " (unknown)", all_match || all_masks ? " exact" : "",
                                 flaggable && !all_match ? " flag" : "");
                FactUse use{i, w.vec, 0, 0, w.value};
                if (w.k == FamilyFactWant::K::derived) {
                    // Tier 2: computed after every reaching definition, all of
                    // them in less deeply nested loops than the use.
                    bool ok = known;
                    for (unsigned d = 0; ok && d < n_defs; ++d)
                        ok = plan_.insts[static_cast<std::size_t>(defs[d])].flow == Flow::normal &&
                             s_.depth[static_cast<std::size_t>(defs[d])] < s_.depth[static_cast<std::size_t>(i)];
                    if (!ok) continue;
                    std::size_t slot = 0;
                    while (slot < s_.derived.size() && !(s_.derived[slot].vec == w.vec && s_.derived[slot].kind == w.value.lo))
                        ++slot;
                    if (slot == s_.derived.size()) {
                        if (slot >= kMaxDerived) continue;
                        s_.derived.push_back({w.vec, static_cast<std::uint8_t>(w.value.lo), false});
                    }
                    for (unsigned d = 0; d < n_defs; ++d) {
                        bool have = false;
                        for (const auto& c : s_.fact_checks) have |= c.kind == 3 && c.def == defs[d] && c.flag == slot;
                        if (!have)
                            s_.fact_checks.push_back({defs[d], static_cast<std::uint16_t>(slot), 3, false, wanting[f]});
                    }
                    use.kind = 3;
                    use.flag = static_cast<std::uint16_t>(slot);
                    s_.fact_uses.push_back(use);
                    continue;
                }
                if (w.k == FamilyFactWant::K::lane_masks) {
                    if (!all_masks) continue;
                    use.kind = 2;
                } else if (all_match) {
                    use.kind = 0;
                } else if (flaggable) {
                    std::size_t fl = 0;
                    while (fl < s_.fact_flags.size() && !(s_.fact_flags[fl].vec == w.vec && s_.fact_flags[fl].value == w.value))
                        ++fl;
                    if (fl == s_.fact_flags.size()) {
                        if (fl >= kMaxFactFlags) continue;
                        s_.fact_flags.push_back({w.vec, false, w.value});
                    }
                    for (unsigned d = 0; d < n_defs; ++d) {
                        const std::uint8_t ck = kind[d] == FamilyFactDef::load128 ? 0 : value[d] == w.value ? 1 : 2;
                        bool have = false;
                        for (const auto& c : s_.fact_checks) have |= c.kind != 3 && c.def == defs[d] && c.flag == fl;
                        if (!have) s_.fact_checks.push_back({defs[d], static_cast<std::uint16_t>(fl), ck, false, checker});
                    }
                    use.kind = 1;
                    use.flag = static_cast<std::uint16_t>(fl);
                } else {
                    continue;
                }
                s_.fact_uses.push_back(use);
            }
        }
    }
    // Only definitions that reach a use write its flag: whenever a use runs,
    // the last write of its register was one of its own definitions, and that
    // definition's flag write is also the flag's last write.
}

EmitContext::VecFact RegionEmitter::vec_fact(unsigned index, const VecValue& value) {
    if (!facts_on_ || !cur_) return {};
    for (const auto& u : s_.fact_uses) {
        if (u.index != index_ || u.vec != index || u.kind >= 2 || !(u.value == value)) continue;
        if (u.kind == 0) return {VecFact::K::exact, {}};
        s_.fact_flags[u.flag].handed_out = true;
        return {VecFact::K::flag, fact_flag(u.flag)};
    }
    return {};
}

// Tier 2: a derived value computed after every reaching definition (prepare_facts).
bool RegionEmitter::vec_derived(unsigned index, unsigned kind, x64::Mem& slot) {
    if (!facts_on_ || !cur_) return false;
    for (const auto& u : s_.fact_uses) {
        if (u.index != index_ || u.vec != index || u.kind != 3 || u.value.lo != kind) continue;
        s_.derived[u.flag].handed_out = true;
        slot = derived_slot(u.flag);
        return true;
    }
    return false;
}

bool RegionEmitter::vec_lane_masks(unsigned index) {
    if (!facts_on_ || !cur_) return false;
    for (const auto& u : s_.fact_uses)
        if (u.index == index_ && u.vec == index && u.kind == 2) return true;
    return false;
}

// After instructions [index, index + consumed) are emitted (end_instruction
// done): the flag writes of definitions among them. A load's check reads the
// register's current host (pin or cache entry) or its context slot. EFLAGS
// are preserved.
void RegionEmitter::emit_fact_checks(std::int32_t index, unsigned consumed) {
    if (s_.fact_checks.empty()) return;
    const auto end = index + static_cast<std::int32_t>(consumed);
    for (auto& c : s_.fact_checks) {
        if (c.def < index || c.def >= end) continue;
        const unsigned vec = c.kind == 3 ? s_.derived[c.flag].vec : s_.fact_flags[c.flag].vec;
        // A later instruction of the same span overwrote the register: it is
        // then the definition the uses see, not this one (no check).
        bool later = false;
        for (auto j = c.def + 1; j < end; ++j)
            later |= ((plan_.insts[static_cast<std::size_t>(j)].vwrites >> vec) & 1u) != 0;
        if (later) continue;
        c.emitted = true;
        if (c.kind == 3) {   // tier 2: the derived value of the definition's result
            const auto& dv = s_.derived[c.flag];
            reset_scratch();
            family_mode_ = true;
            x64::Xmm reg;
            if (vpinned(dv.vec)) reg = vpin(dv.vec);
            else if (const auto h = borrow_vec(dv.vec); x64::id(h) < 16) reg = h;
            else reg = read_vec(dv.vec);
            c.facts->derive(*this, reg, dv.kind, derived_slot(c.flag));
            reset_scratch();
            family_mode_ = false;
            cf_mark_ = a_.control_flow_count();
            continue;
        }
        if (c.kind != 0) {   // a constant definition: is it the value?
            a_.mov(16, fact_flag(c.flag), c.kind == 1 ? 0 : 1);
            continue;
        }
        const auto& flag = s_.fact_flags[c.flag];
        reset_scratch();
        family_mode_ = true;
        x64::Xmm reg = x64::xmm(0);
        bool in_reg = false;
        if (vpinned(flag.vec)) {
            reg = vpin(flag.vec);
            in_reg = true;
        } else if (const auto h = borrow_vec(flag.vec); x64::id(h) < 16) {
            reg = h;
            in_reg = true;
        }
        if (!in_reg && vslot_alias_[flag.vec] >= 0) {   // tier 2: not in its own slot
            reg = read_vec(flag.vec);
            in_reg = true;
        }
        c.facts->check(*this, reg, in_reg, ptr(Gp::r15, v_disp(flag.vec)), flag.value, fact_flag(c.flag));
        reset_scratch();
        family_mode_ = false;
        cf_mark_ = a_.control_flow_count();
    }
}

// ---- tier 2: vector copy aliases ---------------------------------------------------------------

void RegionEmitter::alias_guard() const {
    if (alias_count_ != 0 || galias_count_ != 0) throw RetryTranslation{pc_, false, RetryTranslation::Resource::aliases};
}

// Forgets which slots hold which host's value (aliases keep their dirty bits).
void RegionEmitter::alias_forget_knowledge() { std::memset(vslot_, 0, sizeof vslot_); }

void RegionEmitter::slot_stored(unsigned guest, unsigned host) {
    const auto bit = 1u << guest;
    for (auto& s : vslot_) s &= ~bit;
    if (host < 16) vslot_[host] |= bit;
}

void RegionEmitter::alias_add(unsigned guest, unsigned host) {
    valias_host_[guest] = static_cast<std::int8_t>(host);
    valias_of_[host] |= 1u << guest;
    ++alias_count_;
    if (!((vslot_[host] >> guest) & 1u)) {   // its slot holds an older value
        valias_dirty_ |= 1u << guest;
        fault_reopen();
    }
}

// The guest is overwritten: forget its alias (no store).
void RegionEmitter::alias_drop_guest(unsigned guest) {
    if (vslot_alias_[guest] >= 0) {
        vslot_alias_[guest] = -1;
        --slot_alias_count_;
        --alias_count_;
    }
    const auto h = valias_host_[guest];
    if (h < 0) return;
    valias_of_[h] &= ~(1u << guest);
    valias_host_[guest] = -1;
    --alias_count_;
    if ((valias_dirty_ >> guest) & 1u) {
        valias_dirty_ &= ~(1u << guest);
        fault_reopen();
    }
}

// Copies slot s (V`guest`'s value) into V`guest`'s own slot. Slot aliases of
// V`guest`'s slot go first (it is overwritten). The temp is a free host
// without a cache entry, else the copy goes through the host stack (push /
// pop of memory operands: no register, no EFLAGS).
void RegionEmitter::slot_alias_materialize(unsigned guest) {
    const auto source = vslot_alias_[guest];
    if (source < 0) return;
    if (a_.control_flow_count() != cf_mark_)   // the copy would run on one path only
        throw RetryTranslation{pc_, false, RetryTranslation::Resource::aliases};
    vslot_alias_[guest] = -1;
    --slot_alias_count_;
    --alias_count_;
    slot_aliases_of(guest);
    const auto from = ptr(Gp::r15, v_disp(static_cast<unsigned>(source)));
    const auto to = ptr(Gp::r15, v_disp(guest));
    if (const auto spare = free_xmm_ & ~vc_.mask & 0xffffu; spare != 0) {
        const auto t = x64::xmm(static_cast<unsigned>(std::countr_zero(spare)));
        vmove(t, from);
        vmove(to, t);
    } else {
        for (const std::int32_t half : {0, 8}) {
            auto f = from, d = to;
            f.disp += half;
            d.disp += half;
            a_.emit_rm(0, false, {0xff}, 6, f);   // push qword [r15 + source slot]
            a_.emit_rm(0, false, {0x8f}, 0, d);   // pop qword [r15 + guest slot]
        }
    }
    // Hosts holding slot s's value now match the guest's slot too.
    const auto bit = 1u << guest, src = 1u << static_cast<unsigned>(source);
    for (auto& s : vslot_) s = (s & src) ? (s | bit) : (s & ~bit);
}

void RegionEmitter::slot_aliases_of(unsigned source) {
    if (slot_alias_count_ == 0) return;
    for (unsigned g = 0; g < 32; ++g)
        if (vslot_alias_[g] == static_cast<std::int8_t>(source)) slot_alias_materialize(g);
}

void RegionEmitter::slot_aliases_all() {
    for (unsigned g = 0; g < 32 && slot_alias_count_ != 0; ++g) slot_alias_materialize(g);
}

void RegionEmitter::alias_store_dirty(unsigned host) {
    auto m = valias_of_[host] & valias_dirty_;
    if (!m) return;
    if (a_.control_flow_count() != cf_mark_)   // the stores would run on one path only
        throw RetryTranslation{pc_, false, RetryTranslation::Resource::aliases};
    for (; m; m &= m - 1) {
        const auto g = static_cast<unsigned>(std::countr_zero(m));
        slot_aliases_of(g);
        vmove(ptr(Gp::r15, v_disp(g)), x64::xmm(host));
        valias_dirty_ &= ~(1u << g);
        slot_stored(g, host);
    }
    fault_reopen();
}

void RegionEmitter::alias_drop_host(unsigned host) {
    alias_store_dirty(host);
    for (auto m = valias_of_[host]; m; m &= m - 1) {
        valias_host_[std::countr_zero(m)] = -1;
        --alias_count_;
    }
    valias_of_[host] = 0;
}

// The owner of `host` gets a new value elsewhere: one of its aliases becomes
// the owner (dirty iff the alias was), the others stay aliases.
void RegionEmitter::alias_owner_leaves(unsigned host) {
    const auto aliases = valias_of_[host];
    if (!aliases) return;
    const auto owner = static_cast<unsigned>(vc_.guest[host]);
    const auto heir = static_cast<unsigned>(std::countr_zero(aliases));
    const bool dirty = ((valias_dirty_ >> heir) & 1u) != 0;
    valias_of_[host] &= ~(1u << heir);
    valias_host_[heir] = -1;
    valias_dirty_ &= ~(1u << heir);
    --alias_count_;
    // Move the entry: the old owner leaves (its value is overwritten), the
    // heir takes the host with the same LRU stamp.
    vc_.host[owner] = -1;
    vc_.host[heir] = static_cast<std::int8_t>(host);
    vc_.guest[host] = static_cast<std::int8_t>(heir);
    if (dirty) vc_.dirty |= 1u << host;
    else vc_.dirty &= ~(1u << host);
    fault_reopen();
}

void RegionEmitter::alias_resolve() {
    if (alias_count_ == 0) return;
    slot_aliases_all();
    for (unsigned h = 0; h < 16; ++h)
        if (valias_of_[h]) alias_drop_host(h);
}

// MOV Vd.16B, Vn.16B (ORR Vd.16B, Vn.16B, Vn.16B) into unpinned registers,
// no code: Vd becomes an alias of the host holding Vn, or, when Vn is only in
// a context slot, of that slot. A copy of the value Vd's own slot holds
// (a rotation back) leaves Vd clean in its slot.
bool RegionEmitter::emit_vec_copy(const PlanInst& pi) {
    const auto w = pi.inst.word;
    if ((w & 0xffe0fc00u) != 0x4ea01c00u || ((w >> 16) & 31u) != ((w >> 5) & 31u)) return false;
    const unsigned d = w & 31u, n = (w >> 5) & 31u;
    static const bool trace = std::getenv("TRANSMUTE_FX_TRACE_ALIAS") != nullptr;   // diagnostics
    if (trace)
        std::fprintf(stderr, "alias: %llx v%u <- v%u pinned %d %d\n", static_cast<unsigned long long>(pi.pc), d, n,
                     vpinned(d) ? 1 : 0, vpinned(n) ? 1 : 0);
    if (d == n || vpinned(d) || vpinned(n) || !may_cache() || pi.conservative_writes) return false;
    reset_scratch();
    const int h = vc_.host[n] >= 0 ? vc_.host[n] : valias_host_[n];
    const auto bit_n = 1u << n, bit_d = 1u << d;
    if (h >= 0) {
        vc_.used[h] = ++cache_clock_;
        if (vc_.host[d] != h && valias_host_[d] != h) {   // else d already names that value
            alias_drop_guest(d);
            if (const auto old = vc_.host[d]; old >= 0) {
                if (valias_of_[old]) alias_owner_leaves(static_cast<unsigned>(old));
                else cache_drop_guest(vc_, d);   // overwritten: a dirty value is dead
            }
            alias_add(d, static_cast<unsigned>(h));
        }
    } else {
        // n's value is in a slot: its own, or the one it aliases.
        const auto source = vslot_alias_[n] >= 0 ? static_cast<unsigned>(vslot_alias_[n]) : n;
        alias_drop_guest(d);
        if (const auto old = vc_.host[d]; old >= 0) {
            if (valias_of_[old]) alias_owner_leaves(static_cast<unsigned>(old));
            else cache_drop_guest(vc_, d);
        }
        if (source != d) {
            vslot_alias_[d] = static_cast<std::int8_t>(source);
            ++slot_alias_count_;
            ++alias_count_;
        }   // else: d's own slot holds the value: d is clean there
    }
    vzero32_ = (vzero32_ & ~bit_d) | ((vzero32_ & bit_n) ? bit_d : 0u);   // fp/neon: a copy
    vzero64_ = (vzero64_ & ~bit_d) | ((vzero64_ & bit_n) ? bit_d : 0u);
    vzwritten_ |= bit_d;   // p3
    reset_scratch();
    return true;
}

} // namespace transmute::fx::core
