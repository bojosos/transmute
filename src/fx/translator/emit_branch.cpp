// Branch, call, return and trap lowering (owner: translator core).
#include "fx/translator/region_emitter.hpp"

#include "fx/runtime/runtime.hpp"   // lookup table encoding (constants only)

#include <bit>

namespace transmute::fx::core {

using x64::Cond;
using x64::Gp;
using x64::Mem;
using x64::ptr;

// Indirect branch to the guest address in rcx with the state flushed: the
// runtime's stub_indirect lookup inlined (fx/runtime/runtime.hpp), so every
// BR/BLR/RET-miss site has its own predicted `jmp`. Misses, misaligned
// targets and contexts without arena_base take stub_indirect. Clobbers rax,
// rdx, r8, r9 and EFLAGS.
void RegionEmitter::emit_indirect_jump() {
    const auto stub = a_.new_label();
    a_.mov(64, ctx_mem(offsetof(GuestContext, pc)), Gp::rcx);   // the miss path (and empty slots) read it
    a_.mov(64, Gp::r8, ctx_mem(offsetof(GuestContext, arena_base)));
    a_.test(64, Gp::r8, Gp::r8);
    a_.jcc_short(Cond::e, stub);
    a_.test(8, Gp::rcx, 3);
    a_.jcc_short(Cond::ne, stub);
    a_.mov(64, Gp::rax, Gp::rcx);
    a_.shr(64, Gp::rax, static_cast<std::uint8_t>(kLookupTagShift));   // tag
    a_.mov(32, Gp::rdx, Gp::rcx);
    a_.shr(32, Gp::rdx, 2);
    a_.xor_(32, Gp::rdx, Gp::rax);
    a_.alu(x64::Alu::and_, 32, Gp::rdx, ctx_mem(offsetof(GuestContext, lookup_mask)));
    a_.mov(64, Gp::r9, ctx_mem(offsetof(GuestContext, lookup_table)));
    a_.mov(64, Gp::rdx, ptr(Gp::r9, Gp::rdx, 8));                      // entry = tag:offset
    a_.mov(64, Gp::r9, Gp::rdx);
    a_.shr(64, Gp::r9, 32);
    a_.cmp(64, Gp::rax, Gp::r9);
    a_.jcc_short(Cond::ne, stub);
    a_.mov(32, Gp::rdx, Gp::rdx);
    a_.add(64, Gp::rdx, Gp::r8);
    a_.jmp(Gp::rdx);
    a_.bind(stub);
    a_.jmp(ctx_mem(offsetof(GuestContext, stub_indirect_live)));   // convention registers live
}

void RegionEmitter::e_branch(const PlanInst& pi) {
    jump_to(pi.taken, static_cast<std::uint64_t>(pi.inst.imm));
}

void RegionEmitter::e_branch_cond(const PlanInst& pi) {
    const auto& d = pi.inst;
    const auto target = static_cast<std::uint64_t>(d.imm);
    if (d.cond >= 14) {
        jump_to(pi.taken, target);
        return;
    }
    if (is_fcmp(flags_)) {   // fused FCMP: one or two jcc on the raw compare flags
        cond_branch_fcmp(fcmp_cond(d.cond), pi.taken, target, pi.fall, pi.pc + 4);
        return;
    }
    const auto c = condition(d.cond);
    cond_branch(c, pi.taken, target, pi.fall, pi.pc + 4);
}

void RegionEmitter::e_compare_branch(const PlanInst& pi) {
    const auto& d = pi.inst;
    const auto target = static_cast<std::uint64_t>(d.imm);
    const unsigned bits = d.sf ? 64 : 32;
    const auto v = val(d.rd);
    if (v.is_zero()) {   // CBZ XZR always branches, CBNZ XZR never does
        if (d.op == 0) {
            jump_to(pi.taken, target);
        } else if (pi.fall >= 0) {
            edge_inline(pi.fall);
        } else {
            flush_dirty(true);
            emit_direct_exit(pi.pc + 4, current_dirty(), flags_);
        }
        return;
    }
    prepare_clobber();
    const Gp r = v.is_reg() ? v.r : cached_gpr(v.g);
    if (r != Gp::none) a_.test(bits, r, r);
    else a_.cmp(bits, gmem(v.g), 0);
    cond_branch(d.op == 0 ? Cond::e : Cond::ne, pi.taken, target, pi.fall, pi.pc + 4);
}

void RegionEmitter::e_test_branch(const PlanInst& pi) {
    const auto& d = pi.inst;
    const auto target = static_cast<std::uint64_t>(d.imm);
    const unsigned bit = d.amount;
    const auto v = val(d.rd);
    if (v.is_zero()) {   // the bit is clear
        if (d.op == 0) {
            jump_to(pi.taken, target);
        } else if (pi.fall >= 0) {
            edge_inline(pi.fall);
        } else {
            flush_dirty(true);
            emit_direct_exit(pi.pc + 4, current_dirty(), flags_);
        }
        return;
    }
    prepare_clobber();
    Cond taken_if_set;
    const Gp r = v.is_reg() ? v.r : cached_gpr(v.g);
    if (r == Gp::none) {
        a_.test(8, ptr(Gp::r15, x_disp(v.g) + static_cast<std::int32_t>(bit / 8)), static_cast<std::int32_t>(1u << (bit % 8)));
        taken_if_set = Cond::ne;
    } else if (bit < 32) {
        a_.test(32, r, static_cast<std::int32_t>(1u << bit));
        taken_if_set = Cond::ne;
    } else {
        a_.bt(64, r, static_cast<std::uint8_t>(bit));
        taken_if_set = Cond::b;
    }
    cond_branch(d.op == 0 ? x64::invert(taken_if_set) : taken_if_set, pi.taken, target, pi.fall, pi.pc + 4);
}

void RegionEmitter::e_call(const PlanInst& pi) {
    const auto& d = pi.inst;
    const auto return_pc = pi.pc + 4;
    if (pi.inline_call) {
        // Inlined callee (laid out next): only x30 = return address. It stays
        // dirty in the register cache (fault maps describe it; a later write
        // before any observation point drops the store). Overwritten before
        // anything can observe it (the next inlined call): no code at all.
        if (dead_after(plan_, index_, 30)) return;
        fixup_imm64(dst(30, false), return_pc);
        return;
    }
    const bool indirect = d.kind == Kind::branch_link_reg;
    take(Gp::rax);
    take(Gp::rdx);
    take(Gp::r8);
    if (indirect) {
        take(Gp::rcx);
        load(64, Gp::rcx, val(d.rn));   // target before x30 changes (BLR x30)
    }
    materialize_state(flags_, false);
    flags_ = FlagsState::ctx;
    // x30 = return pc. Plan::cold_layout: straight into x30's pin (the RSB
    // push stores it from there), else through rdx.
    Gp ret_reg = Gp::rdx;
    if (plan_.cold_layout && pinned(30)) {
        ret_reg = pin(30);
        fixup_imm64(ret_reg, return_pc);
    } else {
        fixup_imm64(Gp::rdx, return_pc);
        if (pinned(30)) a_.mov(64, pin(30), Gp::rdx);
        else a_.mov(64, gmem(30), Gp::rdx);
    }
    cache_drop_guest(gc_, 30);
    note_write(30);
    rsb_push(return_pc, pi.continuation, ret_reg);
    exit_pins(current_dirty());
    if (indirect) {
        emit_indirect_jump();
        return;
    }
    // Calls always leave the region: the callee is translated as its own region.
    emit_direct_exit(static_cast<std::uint64_t>(d.imm), 0, FlagsState::ctx, false);
}

// ---- switch dispatch --------------------------------------------------------------
// BR xN through a recognised jump table (Plan::tables). Dispatch stays in the
// region: index wI selects host table entry k = {target_k - region start,
// host label_k - table}; the jump is taken only if wI < count and xN ==
// target_k, so the guest's own computation decides the target and the table
// is only a fast path. Anything else is the ordinary indirect exit. Pins stay
// in their registers (the dispatch is an internal edge for the dirty-pin
// dataflow); the register cache was stored by emit_instruction and is empty
// on every table edge; flags are in the context.
void RegionEmitter::e_table_branch(const PlanInst& pi) {
    const auto& t = plan_.tables[static_cast<std::size_t>(pi.table)];
    take(Gp::rax);
    take(Gp::rcx);
    take(Gp::rdx);
    take(Gp::r8);
    take(Gp::r9);
    materialize_state(flags_, false);   // clobbers rax only
    flags_ = FlagsState::ctx;
    load(64, Gp::rcx, val(pi.inst.rn));          // target
    load(32, Gp::rdx, val(t.index_reg));         // index (UXTW)
    reset_cache(gc_);                            // clean (stored): the context holds every value
    reset_cache(vc_);
    forget_vec_zero();
    // The fallback (pins stored, the ordinary indirect exit) is only reached
    // by the checks' jumps: cold with Plan::cold_layout.
    const auto dirty = current_dirty();
    const bool cold = plan_.cold_layout;
    const auto generic = cold ? add_stub(Stub::K::indirect_exit, FlagsState::ctx, dirty, 0, -1) : a_.new_label();
    auto& ht = host_tables_.emplace_back();
    ht.label = a_.new_label();
    ht.table = pi.table;
    a_.cmp(32, Gp::rdx, static_cast<std::int32_t>(t.count));
    a_.jcc(Cond::ae, generic);
    a_.lea(64, Gp::r8, x64::rip(ht.label));
    a_.movsx(64, Gp::r9, ptr(Gp::r8, Gp::rdx, 8), 32);
    fixup_imm64(Gp::rax, plan_.start);
    a_.add(64, Gp::r9, Gp::rax);
    a_.cmp(64, Gp::r9, Gp::rcx);
    a_.jcc(Cond::ne, generic);
    a_.movsx(64, Gp::r9, ptr(Gp::r8, Gp::rdx, 8, 4), 32);
    a_.add(64, Gp::r9, Gp::r8);
    a_.jmp(Gp::r9);
    // One label per distinct target: in-region blocks (multiway edges), else
    // a linkable direct exit; unrepresentable entries take the fallback.
    ht.entries.resize(t.count);
    ht.guest_rel.resize(t.count);
    for (std::uint32_t k = 0; k < t.count; ++k) {
        const auto pc = plan_.table_pcs[t.first + k];
        const auto rel = static_cast<std::int64_t>(pc - plan_.start);
        std::uint32_t same = k;
        for (std::uint32_t j = 0; j < k && same == k; ++j)
            if (plan_.table_pcs[t.first + j] == pc) same = j;
        if (rel != static_cast<std::int32_t>(rel) || (pc & 3) != 0) {
            ht.entries[k] = generic;
            ht.guest_rel[k] = 0;   // the region start: a match takes the fallback, which is exact
            continue;
        }
        ht.guest_rel[k] = static_cast<std::int32_t>(rel);
        if (same != k) {
            ht.entries[k] = ht.entries[same];
            continue;
        }
        const auto target = plan_.table_targets[t.first + k];
        ht.entries[k] = target >= 0 ? multiway_label(target)
                                    : add_stub(Stub::K::direct_exit, FlagsState::ctx, dirty, pc, -1, -1);
    }
    if (!cold) {
        a_.bind(generic);
        exit_pins(dirty);
        emit_indirect_jump();   // rcx = target
    }
    core_counters().tables.fetch_add(1, std::memory_order_relaxed);
}

x64::Label RegionEmitter::multiway_label(std::int32_t target) {
    const auto& t = plan_.insts[static_cast<std::size_t>(target)];
    if (is_pending_target(target)) return pending_edge(target, index_, FlagsState::ctx);
    if (t.inherit_flags) inherited_[static_cast<std::size_t>(target)] = FlagsState::ctx;
    if (s_.cache_slot[static_cast<std::size_t>(target)] < 0) decide_entry(target, true);
    const auto& e = s_.cache_snaps[static_cast<std::size_t>(s_.cache_slot[static_cast<std::size_t>(target)])];
    if (reconcile_needs_code(CacheSnapshot{gc_, vc_}, e))
        return add_stub(Stub::K::edge, FlagsState::ctx, 0, 0, target, push_snap());
    return label_of(target);
}

void RegionEmitter::emit_host_tables() {
    for (auto& ht : host_tables_) {
        a_.align(8);
        a_.bind(ht.label);
        ht.offset = static_cast<std::uint32_t>(a_.size());
        for (const auto rel : ht.guest_rel) {
            a_.dd(static_cast<std::uint32_t>(rel));
            a_.dd(0);   // host offset (patch_host_tables)
        }
    }
}

void RegionEmitter::patch_host_tables() {
    for (const auto& ht : host_tables_)
        for (std::size_t k = 0; k < ht.entries.size(); ++k) {
            const auto host = static_cast<std::int64_t>(a_.offset_of(ht.entries[k])) - static_cast<std::int64_t>(ht.offset);
            a_.put32(ht.offset + 8 * k + 4, static_cast<std::uint32_t>(static_cast<std::int32_t>(host)));
        }
}

void RegionEmitter::e_indirect(const PlanInst& pi) {
    const auto& d = pi.inst;
    if (pi.inline_ret) {   // RET X30 of an inlined callee: x30 == link_pc here
        if (pi.flow == Flow::jump) jump_to(pi.taken, pi.link_pc);
        return;            // Flow::normal: the continuation follows
    }
    if (pi.table >= 0) {
        e_table_branch(pi);
        return;
    }
    // Plan::cold_layout: RET compares a pinned target register (x30's
    // convention host) with the RSB directly; only the miss path copies it
    // to rcx. Not when the exit reloads an evicted convention register there.
    Gp target = Gp::rcx;
    if (const auto v = val(d.rn); plan_.cold_layout && d.kind == Kind::ret && v.is_reg()) {
        bool restored = false;
        const auto& conv = translator_register_convention();
        for (auto m = plan_.evicted; m; m &= m - 1)
            restored |= conv.host[std::countr_zero(m)] == static_cast<std::int8_t>(x64::id(v.r));
        if (!restored) target = v.r;
    }
    if (target == Gp::rcx) {
        take(Gp::rcx);
        load(64, Gp::rcx, val(d.rn));
    }
    materialize_state(flags_, false);
    flags_ = FlagsState::ctx;
    exit_pins(current_dirty());
    if (d.kind == Kind::ret) {
        // RSB: compare with the top entry, pop and jump directly on a hit.
        // Plan::cold_layout: the miss path (the full lookup) is cold.
        const auto top = ctx_mem(offsetof(GuestContext, rsb_top));
        const auto rsb = ctx_disp(offsetof(GuestContext, rsb));
        const bool cold = plan_.cold_layout;
        const auto miss = cold ? add_stub(Stub::K::lookup, FlagsState::ctx, 0, 0,
                                          target == Gp::rcx ? -1 : static_cast<std::int32_t>(x64::id(target)))
                               : a_.new_label();
        a_.mov(32, Gp::rax, top);
        a_.lea(32, Gp::rax, ptr(Gp::rax, -1));
        a_.and_(32, Gp::rax, static_cast<std::int32_t>(kRsbEntries - 1));
        a_.mov(32, Gp::rdx, Gp::rax);
        a_.shl(32, Gp::rdx, 4);
        a_.cmp(64, target, ptr(Gp::r15, Gp::rdx, 1, rsb));
        if (cold) a_.jcc(Cond::ne, miss);
        else a_.jcc_short(Cond::ne, miss);
        a_.mov(32, top, Gp::rax);
        a_.jmp(ptr(Gp::r15, Gp::rdx, 1, rsb + 8));
        if (cold) return;
        a_.bind(miss);
    }
    emit_indirect_jump();
}

void RegionEmitter::e_trap(const PlanInst& pi) {
    const auto& d = pi.inst;
    materialize_state(flags_, false);
    flags_ = FlagsState::ctx;
    const auto imm = static_cast<std::uint64_t>(d.imm);
    if (d.kind == Kind::svc) emit_reason_exit(current_dirty(), pi.pc + 4, ExitReason::svc, imm, true);
    else emit_reason_exit(current_dirty(), pi.pc, ExitReason::brk, imm, true);
}

} // namespace transmute::fx::core
