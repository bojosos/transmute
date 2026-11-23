// Integer load/store lowering (owner: translator core). Guest VA == host VA:
// every access is one plain x64 mov covered by a FaultSite.
#include "fx/translator/region_emitter.hpp"

#include <stdexcept>

namespace transmute::fx::core {

using x64::Gp;
using x64::Mem;
using x64::ptr;

// Code-byte hygiene for the persistent cache's relocation audit, which flags
// any 8-byte window of code whose value lies in the module: a disp32 such as
// 0x140 followed by a zero immediate reads as 0x00000001_40xxxxxx. Stores of
// XZR therefore use a zeroed register unless the displacement is a disp8.
// ([modrm 0x4x][disp8 1][imm32 0] reads as 0x00000001_4xxxxxxx too.)
bool RegionEmitter::disp8(const Mem& m) { return m.disp >= -128 && m.disp <= 127 && m.disp != 1; }

Gp RegionEmitter::zero_reg() {
    const auto z = alloc();
    zero(z);
    return z;
}

// Loads or stores `size` bytes between `value` and `m`.
void RegionEmitter::access(const Inst& d, Gp value, const Mem& m, unsigned size, MemOp op, bool sf) {
    (void)d;
    switch (op) {
    case MemOp::store:
        a_.mov(size * 8, m, value);
        return;
    case MemOp::load:
        if (size == 8) a_.mov(64, value, m);
        else if (size == 4) a_.mov(32, value, m);
        else a_.movzx(32, value, m, size * 8);
        return;
    case MemOp::load_signed64:
        a_.movsx(64, value, m, size * 8);
        return;
    case MemOp::load_signed32:
        a_.movsx(32, value, m, size * 8);
        return;
    case MemOp::prefetch: return;
    }
    (void)sf;
}

// A cached unpinned guest value as a register operand: borrowed (read-only,
// not handed out) for the rest of the instruction. TRANSMUTE_FX_CORE=nosrcreg:
// unchanged (the caller copies it as before).
RegionEmitter::Val RegionEmitter::resolve(const Val& v) {
    if (!v.is_mem() || !isel_on(isel_no_srcreg)) return v;
    const auto h = borrow_gpr(v.g);
    if (h == Gp::none) return v;
    Val r = v;
    r.k = Val::K::reg;
    r.r = h;
    return r;
}

// Register-offset address [base + extend(Xm) << amount + imm]. A UXTW/SXTW
// index whose producer already zero/sign-extended it (ext_facts) is used
// directly (TRANSMUTE_FX_CORE=nozext: always extended).
Mem RegionEmitter::reg_address(const Inst& d, Gp base) {
    return index_address(base, d.rm, d.shift, d.amount, static_cast<std::int32_t>(d.imm), index_);
}

// ---- guest address operands shared by the integer and SIMD&FP accesses ---------
// (neon; additive): EmitContext::address, the register-offset form above, and
// folded ADD/SUB producers of SIMD&FP access bases (fold_family_access).

Mem RegionEmitter::index_address(Gp base, unsigned rm, unsigned option, unsigned shift, std::int32_t disp,
                                 std::int32_t fact_index) {
    const auto iv = val(rm);
    if (iv.is_zero()) return ptr(base, disp);
    Gp index;
    if (folded(rm)) {   // an access indexed by the folded register itself
        index = fold_value();
        if (option == 2) a_.mov(32, index, index);
        else if (option == 6) a_.movsx(64, index, index, 32);
    } else {
        const unsigned facts =
            (option == 2 || option == 6) && isel_on(isel_no_zext) ? ext_facts(plan_, fact_index, rm) : 0u;
        const Gp ir = iv.is_reg() ? iv.r : cached_gpr(iv.g);
        if (option == 2 && !(facts & ext_zero32)) {   // UXTW
            index = alloc();
            if (ir != Gp::none) a_.mov(32, index, ir);
            else a_.mov(32, index, gmem(iv.g));
        } else if (option == 6 && !(facts & ext_sign32)) {   // SXTW
            index = alloc();
            if (ir != Gp::none) a_.movsx(64, index, ir, 32);
            else a_.movsx(64, index, gmem(iv.g), 32);
        } else {   // LSL / SXTX, or an already extended W index
            index = in_reg(iv);
        }
    }
    if (shift <= 3) return ptr(base, index, 1u << shift, disp);
    // shift 4 (Q): index * 16 via two lea (no EFLAGS change).
    const Gp t = alloc();
    a_.lea(64, t, x64::ptr_index(index, 8));
    a_.lea(64, t, ptr(base, t, 2));
    return ptr(t, disp);
}

// Xd = Xn + (ext(Xm) << amount) + imm of the active fold, in a fresh temp.
Gp RegionEmitter::fold_value() {
    const auto f = afold_;
    afold_.active = false;   // Xn/Xm below are read as themselves
    const Gp base = in_reg(val(f.rn, true));
    const Mem m = f.rm >= 32 ? ptr(base, f.imm) : index_address(base, f.rm, f.option, f.amount, f.imm, f.index);
    const Gp t = alloc();
    a_.lea(64, t, m);
    afold_ = f;
    return t;
}

x64::Mem RegionEmitter::address(const GuestAddress& g) {
    if (folded(g.base) && !folded(g.index)) {
        const auto f = afold_;
        const std::int32_t disp = f.imm + g.disp;   // |imm| < 2^24, |disp| < 2^16
        if (f.rm >= 32 || g.index >= 32) {
            afold_.active = false;   // Xn, Xm are read as themselves
            const Gp base = in_reg(val(f.rn, true));
            const Mem m = f.rm < 32 ? index_address(base, f.rm, f.option, f.amount, disp, f.index)
                          : g.index < 32 ? index_address(base, g.index, g.option, g.shift, disp, index_)
                                         : ptr(base, disp);
            afold_ = f;
            return m;
        }
        // Both indexed: the folded value in a register, then the access's index.
    }
    const Gp base = folded(g.base) ? fold_value() : in_reg(val(g.base, true));
    if (g.index >= 32) return ptr(base, g.disp);
    return index_address(base, g.index, g.option, g.shift, g.disp, index_);
}

// See region_emitter.hpp. `d` is the ADD/SUB at index_.
unsigned RegionEmitter::fold_family_access(const Inst& d) {
    const auto n = static_cast<std::int32_t>(plan_.insts.size());
    if (index_ + 1 >= n || !isel_on(isel_no_fold)) return 0;
    const auto& pa = plan_.insts[static_cast<std::size_t>(index_)];
    const auto& pb = plan_.insts[static_cast<std::size_t>(index_) + 1];
    if (pb.leader || pb.pc != pa.pc + 4 || pa.flow != Flow::normal || pb.flow != Flow::normal ||
        pb.conservative_writes || pb.inst.kind != Kind::unknown || pb.family < 0)
        return 0;
    if (!d.sf || d.s || d.rd == 31) return 0;   // 64-bit, no flags, not SP
    const auto table = registered_families();
    if (static_cast<unsigned>(pb.family) >= table.count) return 0;
    const auto hook = table.families[pb.family].address_base;
    unsigned base = 0;
    bool indexed = false;
    if (hook == nullptr || !hook(pb.inst.word, base, indexed) || base != d.rd) return 0;
    AddrFold f;
    f.rd = static_cast<std::uint8_t>(d.rd);
    f.rn = static_cast<std::uint8_t>(d.rn);   // SP when 31 (imm, ext)
    switch (d.kind) {
    case Kind::add_sub_imm: f.imm = static_cast<std::int32_t>(d.op ? -d.imm : d.imm); break;
    case Kind::add_sub_shift:
        if (d.op != 0 || d.shift != 0 || d.amount > 3 || d.rn == 31) return 0;   // ADD Xn, Xm, LSL #0..3
        f.rm = static_cast<std::uint8_t>(d.rm);
        f.option = 3;
        f.amount = static_cast<std::uint8_t>(d.amount);
        break;
    case Kind::add_sub_ext:
        if (d.op != 0 || d.amount > 3 || (d.shift & 2) == 0) return 0;   // UXTW/UXTX/SXTW/SXTX #0..3
        f.rm = static_cast<std::uint8_t>(d.rm);
        f.option = static_cast<std::uint8_t>(d.shift);
        f.amount = static_cast<std::uint8_t>(d.amount);
        break;
    default: return 0;
    }
    if (indexed && f.rm < 32) return 0;   // three registers: no x64 form
    if ((pb.writes >> d.rd) & 1u) return 0;
    if (!dead_after(plan_, index_ + 1, d.rd)) return 0;
    const auto add_index = index_;
    const auto add_pc = pc_;
    f.index = add_index;
    f.active = true;
    begin_instruction(add_index + 1);   // the access is the current instruction...
    afold_ = f;
    fault_pc_ = add_pc;                 // ...whose faults report the ADD (Xd unchanged)
    const auto consumed = try_families(index_);
    afold_.active = false;
    if (consumed == 0) {   // declined (nothing emitted): the ADD on its own
        begin_instruction(add_index);
        return 0;
    }
    if (consumed != 1) throw std::logic_error("folded family access consumed several instructions");
    return 2;
}

namespace {
// [Xn|SP + imm] / [Xn|SP + ext(Xm) << amount + imm] of a non-writeback access,
// through EmitContext::address (shared with the SIMD&FP accesses).
EmitContext::GuestAddress guest_address_of(const Inst& d) {
    EmitContext::GuestAddress g;
    g.base = d.rn;
    g.disp = static_cast<std::int32_t>(d.imm);
    if (d.mode == AddrMode::reg) {   // imm: displacement of a folded ADD (else 0)
        g.index = d.rm;
        g.option = d.shift;
        g.shift = d.amount;
    }
    return g;
}
} // namespace

unsigned RegionEmitter::try_fuse_load_alu(const Inst& d, const Mem& m, std::int32_t load_index) {
    if (!isel_on(isel_no_fold) || d.mem != MemOp::load || (d.size != 2 && d.size != 3) || d.rd >= 31) return 0;
    if (bool_reader_ >= 0 || flags_saved_) return 0;
    const auto alu_index = load_index + 1;
    const auto n = static_cast<std::int32_t>(plan_.insts.size());
    if (alu_index >= n) return 0;
    const auto& pl = plan_.insts[static_cast<std::size_t>(load_index)];
    const auto& pb = plan_.insts[static_cast<std::size_t>(alu_index)];
    if (pb.leader || pb.pc != pl.pc + 4 || pb.flow != Flow::normal) return 0;
    const auto& c = pb.inst;
    const bool is64 = d.size == 3;
    const unsigned bits = is64 ? 64u : 32u;
    if (c.sf != is64) return 0;

    // 1. LDR Wt/Xt, [mem]; CMP Wt/Xt, #imm (dead load destination).
    if (c.kind == Kind::add_sub_imm && c.op == 1 && c.s && c.rd == 31 && c.rn == d.rd &&
        gpr_dead_after(alu_index, d.rd)) {
        const auto imm = static_cast<std::int64_t>(c.imm);
        if (imm == static_cast<std::int32_t>(imm) && (imm != 0 || disp8(m))) {
            mark_may_fault();
            a_.alu(x64::Alu::cmp, bits, m, static_cast<std::int32_t>(imm));
            fault_close();
            if (!pinned(d.rd)) cache_drop_guest(gc_, d.rd);
            set_flags(FlagsProducer::sub);
            return 1;
        }
    }
    // 2. LDR Wt/Xt, [mem]; TST Wt/Xt, #imm (dead load destination; avoid imm32 0/1 AOT pattern).
    if (c.kind == Kind::logic_imm && c.op == 3 && c.s && c.rd == 31 && c.rn == d.rd &&
        gpr_dead_after(alu_index, d.rd)) {
        const auto kval = static_cast<std::uint64_t>(c.imm) & (is64 ? ~0ULL : 0xffffffffULL);
        const bool fits = is64 ? static_cast<std::uint64_t>(static_cast<std::int64_t>(static_cast<std::int32_t>(kval))) == kval
                               : true;
        const auto i32 = static_cast<std::int32_t>(static_cast<std::uint32_t>(kval));
        if (fits && i32 != 0 && i32 != 1) {
            mark_may_fault();
            a_.test(bits, m, i32);
            fault_close();
            if (!pinned(d.rd)) cache_drop_guest(gc_, d.rd);
            set_flags(FlagsProducer::logic);
            return 1;
        }
    }
    // 3. LDR Wt/Xt, [mem]; CMP Wn/Xn, Wt/Xt (or CMP Wt/Xt, Wm/Xm).
    if (c.kind == Kind::add_sub_shift && c.op == 1 && c.s && c.rd == 31 && c.amount == 0 &&
        gpr_dead_after(alu_index, d.rd)) {
        if (c.rm == d.rd && c.rn != d.rd) {
            const auto nv = val(c.rn);
            const Gp nr = nv.is_zero() ? zero_reg() : in_reg(nv);
            mark_may_fault();
            a_.alu(x64::Alu::cmp, bits, nr, m);
            fault_close();
            if (!pinned(d.rd)) cache_drop_guest(gc_, d.rd);
            set_flags(FlagsProducer::sub);
            return 1;
        }
        if (c.rn == d.rd && c.rm != d.rd) {
            const auto mv = val(c.rm);
            const Gp mr = mv.is_zero() ? zero_reg() : in_reg(mv);
            mark_may_fault();
            a_.alu(x64::Alu::cmp, bits, m, mr);
            fault_close();
            if (!pinned(d.rd)) cache_drop_guest(gc_, d.rd);
            set_flags(FlagsProducer::sub);
            return 1;
        }
    }
    // 4. LDR Wt/Xt, [mem]; EOR/ORR/AND/ANDS/ADD/ADDS/SUB/SUBS Wd/Xd, Wn, Wm (unshifted).
    x64::Alu alu_op = x64::Alu::xor_;
    FlagsProducer prod = FlagsProducer::logic;
    bool is_logic = false;
    unsigned other = 31;
    if (c.kind == Kind::logic_shift && !c.invert && c.amount == 0 && c.rd < 31) {
        static constexpr x64::Alu kOps[4] = {x64::Alu::and_, x64::Alu::or_, x64::Alu::xor_, x64::Alu::and_};
        if (c.rm == d.rd && c.rn != d.rd) other = c.rn;
        else if (c.rn == d.rd && c.rm != d.rd) other = c.rm;
        alu_op = kOps[c.op];
        prod = FlagsProducer::logic;
        is_logic = true;
    } else if (c.kind == Kind::add_sub_shift && c.amount == 0 && c.rd < 31) {
        if (c.op == 0) {
            if (c.rm == d.rd && c.rn != d.rd) other = c.rn;
            else if (c.rn == d.rd && c.rm != d.rd) other = c.rm;
            alu_op = x64::Alu::add;
            prod = FlagsProducer::add;
        } else if (c.rm == d.rd && c.rn != d.rd) {
            other = c.rn;
            alu_op = x64::Alu::sub;
            prod = FlagsProducer::sub;
        }
    }
    if (other >= 31) return 0;
    if (c.rd != d.rd && !gpr_dead_after(alu_index, d.rd)) return 0;
    // VEH fault exactness: never overwrite a pinned destination before the faulting load-op.
    if (c.rd != other && pinned(c.rd)) return 0;
    if (!pinned(c.rd) && pending_gpr_count_ >= std::size(pending_gpr_)) return 0;

    // Materialize live host flags to ctx->nzcv before mark_may_fault() (never
    // save_flags_around into rax, which would desync fault_flags() on a fault).
    if (!c.s && keep_flags()) materialize();
    Gp dd = Gp::none;
    if (c.rd == other && pinned(c.rd)) {
        note_write(c.rd);
        dd = pin(c.rd);
    } else if (c.rd == other && gc_.host[c.rd] >= 0 &&
               is_free(x64::gp(static_cast<unsigned>(gc_.host[c.rd]))) &&
               galias_of_[static_cast<unsigned>(gc_.host[c.rd])] == 0) {
        const auto h = static_cast<unsigned>(gc_.host[c.rd]);
        free_gpr_ &= ~(1u << h);
        note_write(c.rd);
        dd = x64::gp(h);
        pending_gpr_[pending_gpr_count_++] = {static_cast<std::uint8_t>(c.rd), dd};
    } else {
        dd = alloc();
        load(bits, dd, val(other));
        note_write(c.rd);
        pending_gpr_[pending_gpr_count_++] = {static_cast<std::uint8_t>(c.rd), dd};
    }
    mark_may_fault();
    a_.alu(alu_op, bits, dd, m);
    fault_close();
    if (d.rd != c.rd && !pinned(d.rd)) cache_drop_guest(gc_, d.rd);
    if (!flags_saved_) {
        zf_inst_ = alu_index;
        if (is_logic) logic_flags_inst_ = alu_index;
        zf_guest_ = static_cast<std::uint8_t>(c.rd);
        zf_bits_ = static_cast<std::uint8_t>(bits);
    }
    if (c.s) set_flags(prod);
    return 1;
}

unsigned RegionEmitter::e_load_store(const Inst& d, unsigned index_mask_bits, std::int32_t load_index) {
    if (load_index < 0) load_index = index_;
    const unsigned size = 1u << d.size;
    const bool writeback = d.mode == AddrMode::pre || d.mode == AddrMode::post;
    // Pinned, cached or loaded (read-only); writeback forms keep the register.
    const Gp base = writeback ? in_reg(val(d.rn, true)) : Gp::none;
    const auto imm = static_cast<std::int32_t>(d.imm);
    Mem m;
    if (index_mask_bits != 0) {
        const Gp b = in_reg(val(d.rn, true));
        const auto iv = val(d.rm);
        const Gp ir = iv.is_reg() ? iv.r : borrow_gpr(iv.g);
        const Gp idx = alloc();
        if (index_mask_bits == 8 || index_mask_bits == 16) {
            if (ir != Gp::none) a_.movzx(32, idx, ir, index_mask_bits);
            else a_.movzx(32, idx, gmem(iv.g), index_mask_bits);
        } else {
            prepare_clobber();
            if (ir != Gp::none) a_.mov(32, idx, ir);
            else a_.mov(32, idx, gmem(iv.g));
            a_.and_(32, idx, static_cast<std::int32_t>((1u << index_mask_bits) - 1u));
        }
        m = ptr(b, idx, 1u << d.amount, imm);
    } else {
        switch (d.mode) {
        case AddrMode::offset:
        case AddrMode::reg: m = address(guest_address_of(d)); break;
        case AddrMode::pre: m = ptr(base, imm); break;
        case AddrMode::post: m = ptr(base, 0); break;
        }
    }
    if (!writeback) {
        if (const auto extra = try_fuse_load_alu(d, m, load_index); extra != 0) return extra;
    }
    if (d.mem == MemOp::store) {
        const auto v = val(d.rd);
        Gp value = Gp::none;
        if (!v.is_zero()) value = in_reg(v);
        if (value == Gp::none && !disp8(m)) value = zero_reg();   // avoid [disp32][imm 0] byte patterns
        mark_may_fault();
        if (value == Gp::none) a_.mov(size * 8, m, 0);
        else access(d, value, m, size, d.mem, d.sf);
        fault_close();
    } else {
        // A load whose destination is also the written-back base keeps the
        // writeback (as the reference interpreter does).
        const bool discard = d.rd == 31 || (writeback && d.rd == d.rn);
        const auto value = discard ? alloc() : dst(d.rd);
        mark_may_fault();
        access(d, value, m, size, d.mem, d.sf);
        fault_close();
    }
    if (writeback) {
        const auto w = dst(d.rn, true);
        a_.lea(64, w, ptr(base, imm));
    }
    return 0;
}

void RegionEmitter::e_load_store_pair(const Inst& d) {
    const unsigned size = 1u << d.size;
    const bool writeback = d.mode == AddrMode::pre || d.mode == AddrMode::post;
    const Gp base = writeback ? in_reg(val(d.rn, true)) : Gp::none;   // pinned, cached or loaded (read-only)
    const auto imm = static_cast<std::int32_t>(d.imm);
    const auto first = d.mode == AddrMode::post ? 0 : imm;
    // AddrMode::reg: only a folded ADD (core_isel.hpp) produces it for pairs.
    const auto m1 = writeback ? ptr(base, first) : address(guest_address_of(d));
    auto m2 = m1;
    m2.disp += static_cast<std::int32_t>(size);
    if (d.mem == MemOp::store) {
        const auto v1 = val(d.rd), v2 = val(d.ra);
        const auto reg_of = [&](const Val& v) { return v.is_zero() ? Gp::none : in_reg(v); };
        auto r1 = reg_of(v1), r2 = reg_of(v2);
        if ((r1 == Gp::none || r2 == Gp::none) && (!disp8(m1) || !disp8(m2))) {   // see e_load_store
            const auto z = zero_reg();
            if (r1 == Gp::none) r1 = z;
            if (r2 == Gp::none) r2 = z;
        }
        mark_may_fault();
        if (r1 == Gp::none) a_.mov(size * 8, m1, 0);
        else a_.mov(size * 8, m1, r1);
        if (r2 == Gp::none) a_.mov(size * 8, m2, 0);
        else a_.mov(size * 8, m2, r2);
        fault_close();
    } else {
        const bool keep1 = d.rd != 31 && !(writeback && d.rd == d.rn) && d.rd != d.ra;
        const bool keep2 = d.ra != 31 && !(writeback && d.ra == d.rn);
        // A pinned first destination with an unpinned (pending) or discarded
        // second one: access the second word first and load the pin last,
        // so no pinned register changes before the last access without a
        // scratch and a move (the two accesses of a pair are unordered).
        if (keep1 && pinned(d.rd) && !(keep2 && pinned(d.ra)) && isel_on(isel_no_srcreg)) {
            const auto t2 = keep2 ? dst(d.ra) : alloc();
            const auto t1 = dst(d.rd);
            mark_may_fault();
            access(d, t2, m2, size, d.mem, d.sf);
            access(d, t1, m1, size, d.mem, d.sf);
            fault_close();
            if (writeback) {
                const auto w = dst(d.rn, true);
                a_.lea(64, w, ptr(base, imm));
            }
            return;
        }
        // The first value goes to a scratch unless its destination is one:
        // no pinned guest register changes before the last access.
        Gp t1;
        if (keep1 && !pinned(d.rd)) t1 = dst(d.rd);
        else t1 = alloc();
        const auto t2 = keep2 ? dst(d.ra) : alloc();
        mark_may_fault();
        access(d, t1, m1, size, d.mem, d.sf);
        access(d, t2, m2, size, d.mem, d.sf);
        fault_close();
        if (keep1 && pinned(d.rd)) a_.mov(64, dst(d.rd), t1);
    }
    if (writeback) {
        const auto w = dst(d.rn, true);
        a_.lea(64, w, ptr(base, imm));
    }
}

unsigned RegionEmitter::e_load_literal(const PlanInst& pi) {
    const auto& d = pi.inst;
    const auto target = static_cast<std::uint64_t>(d.imm);
    auto lit_mem = [&]() -> Mem {
        // Reuse a free uncached scratch GPR already holding a nearby literal
        // pool address in the same basic block: avoids repeated 10-byte movabs
        // on consecutive V8 TurboFan map/constant pool loads while keeping
        // AOT relocations invariant (relative offset within the same block).
        if (lit_base_reg_ != Gp::none && is_free(lit_base_reg_) &&
            !((gc_.mask >> x64::id(lit_base_reg_)) & 1u)) {
            const auto delta = static_cast<std::int64_t>(target) - static_cast<std::int64_t>(lit_base_pc_);
            if (delta >= -1048576 && delta <= 1048576) {
                free_gpr_ &= ~(1u << x64::id(lit_base_reg_));
                return ptr(lit_base_reg_, static_cast<std::int32_t>(delta));
            }
        }
        const auto address = alloc();
        fixup_imm64(address, target);
        lit_base_reg_ = address;
        lit_base_pc_ = target;
        return ptr(address, 0);
    };
    // Fuse `LDR Wt/Xt, =lit; CMP Wn/Xn, Wt/Xt` when Wt/Xt is dead after CMP:
    // compares directly against memory [address], avoiding a scratch/cache reg
    // allocation and eviction while preserving exact live read & FaultSite.
    const auto n = static_cast<std::int32_t>(plan_.insts.size());
    if (isel_on(isel_no_fold) && d.rd < 31 && d.mem == MemOp::load && (d.size == 2 || d.size == 3) &&
        index_ + 1 < n && bool_reader_ < 0 && !flags_saved_) {
        const auto& pb = plan_.insts[static_cast<std::size_t>(index_) + 1];
        const auto& c = pb.inst;
        const bool is_cmp = !pb.leader && pb.pc == pi.pc + 4 && pb.flow == Flow::normal &&
                            c.kind == Kind::add_sub_shift && c.op == 1 && c.s && c.rd == 31 && c.amount == 0 &&
                            c.sf == (d.size == 3) && c.rm == d.rd && c.rn != d.rd;
        if (is_cmp && gpr_dead_after(index_ + 1, d.rd)) {
            const unsigned bits = d.size == 3 ? 64 : 32;
            const Mem m = lit_mem();
            const auto nv = val(c.rn);
            const Gp nr = nv.is_zero() ? zero_reg() : in_reg(nv);
            mark_may_fault();
            a_.alu(x64::Alu::cmp, bits, nr, m);
            fault_close();
            if (!pinned(d.rd)) cache_drop_guest(gc_, d.rd);
            set_flags(FlagsProducer::sub);
            return 2;
        }
    }
    const Mem m = lit_mem();
    const auto value = d.rd == 31 ? alloc() : dst(d.rd);
    mark_may_fault();
    access(d, value, m, 1u << d.size, d.mem, d.sf);
    fault_close();
    return 1;
}

} // namespace transmute::fx::core
