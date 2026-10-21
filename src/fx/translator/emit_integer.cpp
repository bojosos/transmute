// Integer data-processing lowering (owner: translator core).
#include "fx/translator/region_emitter.hpp"

#include "fx/common/cpu_features.hpp"

#include <bit>
#include <cstdlib>
#include <string>

namespace transmute::fx::core {

using x64::Alu;
using x64::Cond;
using x64::Gp;
using x64::Label;
using x64::Mem;
using x64::Shift;
using x64::ptr;

namespace {

constexpr std::uint64_t ones(unsigned n) { return n >= 64 ? ~0ULL : (1ULL << n) - 1; }

bool fits_imm32(std::uint64_t v, unsigned bits) {
    return bits == 32 || static_cast<std::int64_t>(v) == static_cast<std::int64_t>(static_cast<std::int32_t>(v));
}

Shift shift_op(unsigned type) {
    switch (type & 3) {
    case 0: return Shift::shl;
    case 1: return Shift::shr;
    case 2: return Shift::sar;
    default: return Shift::ror;
    }
}

// mov r16, imm16 (keeps bits 16..63).
void mov16_imm(x64::Assembler& a, Gp r, std::uint16_t imm) {
    a.db(0x66);
    if (x64::id(r) >= 8) a.db(0x41);
    a.db(static_cast<std::uint8_t>(0xb8 + (x64::id(r) & 7)));
    a.dw(imm);
}

void bsr(x64::Assembler& a, unsigned bits, Gp dst, Gp src) {
    a.emit_rr(0, bits == 64, {0x0f, 0xbd}, x64::id(dst), x64::id(src));
}

constexpr std::int32_t kScanBack = 64;    // instructions scanned back for a register's writer
constexpr std::int32_t kScanAhead = 24;   // instructions scanned ahead for an overwrite

unsigned value_facts(std::uint64_t v) {
    unsigned f = 0;
    if ((v >> 32) == 0) f |= ext_zero32;
    if (static_cast<std::int64_t>(v) == static_cast<std::int64_t>(static_cast<std::int32_t>(v))) f |= ext_sign32;
    return f;
}

// Facts about a result of `bits` significant low bits that is zero- or
// sign-extended to 64 bits.
unsigned narrow_facts(unsigned bits, bool sign) {
    if (sign) return bits <= 32 ? ext_sign32 : 0u;
    return (bits <= 32 ? ext_zero32 : 0u) | (bits <= 31 ? ext_sign32 : 0u);
}

bool known_constant_at(const Plan& plan, std::int32_t index, unsigned g, std::uint64_t& value, unsigned depth) {
    if (g >= 31 || depth > 4) return false;
    const auto j = last_writer(plan, index, g);
    if (j < 0) return false;
    const auto& d = plan.insts[static_cast<std::size_t>(j)].inst;
    const auto mask = ones(d.sf ? 64 : 32);
    if (d.kind == Kind::move_wide) {
        const unsigned shift = d.amount;
        const auto imm = static_cast<std::uint64_t>(d.imm) & 0xffffu;
        if (d.op == 3) {   // MOVK: the rest comes from the previous value
            std::uint64_t prev = 0;
            if (!known_constant_at(plan, j, g, prev, depth + 1)) return false;
            value = ((prev & ~(0xffffULL << shift)) | (imm << shift)) & mask;
            return true;
        }
        value = imm << shift;
        if (d.op == 0) value = ~value;
        value &= mask;
        return true;
    }
    if (d.kind == Kind::logic_imm && d.op == 1 && d.rn == 31) {   // MOV (bitmask immediate)
        value = static_cast<std::uint64_t>(d.imm) & mask;
        return true;
    }
    return false;
}

} // namespace

// ---- instruction-selection analyses (core_isel.hpp) ------------------------------

std::uint32_t isel_knob_bits() {
    static const std::uint32_t bits = [] {
        std::uint32_t b = 0;
        if (const char* v = std::getenv("TRANSMUTE_FX_CORE")) {
            const std::string s(v);
            const bool all = s.find("oldisel") != std::string::npos;
            if (all || s.find("nofold") != std::string::npos) b |= isel_no_fold;
            if (all || s.find("nozext") != std::string::npos) b |= isel_no_zext;
            if (all || s.find("nosrcreg") != std::string::npos) b |= isel_no_srcreg;
            if (all || s.find("nomovk") != std::string::npos) b |= isel_no_movk;
        }
        return b;
    }();
    return bits;
}

std::int32_t last_writer(const Plan& plan, std::int32_t index, unsigned g) {
    const auto bit = 1u << g;
    for (std::int32_t j = index - 1; j >= 0 && index - j <= kScanBack; --j) {
        if (plan.insts[static_cast<std::size_t>(j) + 1].leader) return -1;   // j + 1 starts the block
        const auto& pj = plan.insts[static_cast<std::size_t>(j)];
        if (pj.flow == Flow::interpret || pj.conservative_writes) return -1;
        if (pj.writes & bit) return j;
    }
    return -1;
}

unsigned ext_facts(const Plan& plan, std::int32_t index, unsigned g) {
    if (g >= 31) return 0;
    const auto j = last_writer(plan, index, g);
    if (j < 0) return 0;
    const auto& pj = plan.insts[static_cast<std::size_t>(j)];
    const auto& d = pj.inst;
    switch (d.kind) {
    case Kind::add_sub_imm:
    case Kind::add_sub_shift:
    case Kind::add_sub_ext:
    case Kind::adc:
    case Kind::logic_shift:
    case Kind::csel:
    case Kind::dp1:
    case Kind::dp2:
    case Kind::extr:
        return d.sf ? 0u : ext_zero32;   // a W result zeroes bits 63..32
    case Kind::dp3:
        return d.op <= dp3_msub && !d.sf ? ext_zero32 : 0u;
    case Kind::logic_imm: {
        if (!d.sf) return ext_zero32;
        const auto imm = static_cast<std::uint64_t>(d.imm);
        if (d.op == 0 || d.op == 3)   // AND: no bit above the mask's highest
            return narrow_facts(64u - static_cast<unsigned>(std::countl_zero(imm)), false);
        std::uint64_t v = 0;
        return known_constant_at(plan, j + 1, g, v, 0) ? value_facts(v) : 0u;
    }
    case Kind::move_wide: {
        std::uint64_t v = 0;
        if (known_constant_at(plan, j + 1, g, v, 0)) return value_facts(v);
        return d.sf ? 0u : ext_zero32;
    }
    case Kind::bitfield: {
        if (!d.sf) return ext_zero32;
        const unsigned r = d.amount, s = d.imms;
        if (d.op == 1) return 0;   // BFM keeps the other bits
        const bool sign = d.op == 0;
        // s >= r: extract of s - r + 1 bits; else insert of s + 1 bits at 64 - r.
        const unsigned top = s >= r ? s - r + 1 : 64 - r + s + 1;
        return narrow_facts(top, sign);
    }
    case Kind::load_store:
    case Kind::load_store_pair:
    case Kind::load_literal: {
        const bool writeback = d.kind != Kind::load_literal && (d.mode == AddrMode::pre || d.mode == AddrMode::post);
        if (writeback && d.rn == g) return 0;   // the written-back base (wins over a load into it)
        if (d.mem == MemOp::load) return d.sf ? 0u : narrow_facts(8u << d.size, false);
        if (d.mem == MemOp::load_signed32) return ext_zero32;
        if (d.mem == MemOp::load_signed64) return ext_sign32;
        return 0;
    }
    default: return 0;
    }
}

bool known_constant(const Plan& plan, std::int32_t index, unsigned g, std::uint64_t& value) {
    return known_constant_at(plan, index, g, value, 0);
}

bool dead_after(const Plan& plan, std::int32_t index, unsigned g) {
    const auto bit = 1u << g;
    const auto n = static_cast<std::int32_t>(plan.insts.size());
    for (std::int32_t j = index + 1; j < n && j <= index + kScanAhead; ++j) {
        const auto& pj = plan.insts[static_cast<std::size_t>(j)];
        if (pj.leader || pj.flow == Flow::interpret || pj.conservative_writes) return false;
        if (pj.reads & bit) return false;
        // Overwritten: dead unless this instruction may observe the state first.
        if (pj.writes & bit) return !pj.may_fault && pj.flow == Flow::normal;
        if (pj.may_fault || pj.flow != Flow::normal) return false;
    }
    return false;
}

bool fold_address(const Plan& plan, std::int32_t index, Inst& access) {
    const auto n = static_cast<std::int32_t>(plan.insts.size());
    if (index + 1 >= n) return false;
    const auto& pa = plan.insts[static_cast<std::size_t>(index)];
    const auto& pb = plan.insts[static_cast<std::size_t>(index) + 1];
    if (pb.leader || pb.pc != pa.pc + 4 || pa.flow != Flow::normal) return false;
    const auto& a = pa.inst;
    const auto& m = pb.inst;
    if (!a.sf || a.s || a.rd == 31) return false;   // 64-bit, no flags, not SP
    if (m.kind != Kind::load_store && m.kind != Kind::load_store_pair) return false;
    if (m.mode != AddrMode::offset || m.rn != a.rd || m.mem == MemOp::prefetch) return false;
    const bool pair = m.kind == Kind::load_store_pair;
    const bool uses = m.rd == a.rd || (pair && m.ra == a.rd);
    if (m.mem == MemOp::store) {
        if (uses) return false;   // the stored value is the address itself
    }
    const bool self = m.mem != MemOp::store && uses;   // the load rewrites Xd
    access = m;
    switch (a.kind) {
    case Kind::add_sub_imm: {
        const auto imm = a.op ? -a.imm : a.imm;
        const auto disp = imm + m.imm + (pair ? (1 << m.size) : 0);
        if (disp != static_cast<std::int32_t>(disp) || imm + m.imm != static_cast<std::int32_t>(imm + m.imm)) return false;
        access.rn = a.rn;   // SP when 31, as for the access
        access.imm = imm + m.imm;
        break;
    }
    case Kind::add_sub_shift:
        if (a.op != 0 || a.shift != 0 || a.amount > 3 || a.rn == 31) return false;   // ADD Xn, Xm, LSL #0..3
        access.mode = AddrMode::reg;
        access.rn = a.rn;
        access.rm = a.rm;
        access.shift = 3;   // LSL
        access.amount = a.amount;
        break;
    case Kind::add_sub_ext:
        if (a.op != 0 || a.amount > 3 || (a.shift & 2) == 0) return false;   // UXTW/UXTX/SXTW/SXTX #0..3
        access.mode = AddrMode::reg;
        access.rn = a.rn;   // SP when 31
        access.rm = a.rm;
        access.shift = a.shift;
        access.amount = a.amount;
        break;
    default: return false;
    }
    return self || dead_after(plan, index + 1, a.rd);
}

unsigned RegionEmitter::emit_core(const PlanInst& pi) {
    const auto& d = pi.inst;
    switch (d.kind) {
    case Kind::unknown: break;
    case Kind::nop: return 1;
    case Kind::add_sub_imm:
    case Kind::add_sub_shift:
    case Kind::add_sub_ext:
        // ADD/SUB feeding only the address of the next load/store: one access
        // (core_isel.hpp, fold_address). Its fault site reports this pc.
        if (isel_on(isel_no_fold)) {
            Inst access;
            if (fold_address(plan_, index_, access)) {
                if (access.kind == Kind::load_store) e_load_store(access);
                else e_load_store_pair(access);
                return 2;
            }
            // neon (additive): the same for a SIMD&FP access (emit_load_store.cpp).
            if (const auto consumed = fold_family_access(d); consumed != 0) return consumed;
        }
        if (d.kind == Kind::add_sub_imm) e_add_sub_imm(d);
        else if (d.kind == Kind::add_sub_shift) e_add_sub_shift(d);
        else e_add_sub_ext(d);
        return 1;
    case Kind::logic_imm: e_logic_imm(d); return 1;
    case Kind::move_wide: return e_move_wide(pi);
    case Kind::adr: return e_adr(pi);
    case Kind::bitfield: e_bitfield(d); return 1;
    case Kind::extr: e_extr(d); return 1;
    case Kind::adc: e_adc(d); return 1;
    case Kind::logic_shift: e_logic_shift(d); return 1;
    case Kind::ccmp: e_ccmp(d); return 1;
    case Kind::csel: e_csel(d); return 1;
    case Kind::dp1: e_dp1(d); return 1;
    case Kind::dp2: e_dp2(d); return 1;
    case Kind::dp3: e_dp3(d); return 1;
    case Kind::load_store: e_load_store(d); return 1;
    case Kind::load_store_pair: e_load_store_pair(d); return 1;
    case Kind::load_literal: e_load_literal(d); return 1;
    case Kind::branch: e_branch(pi); return 1;
    case Kind::branch_link:
    case Kind::branch_link_reg: e_call(pi); return 1;
    case Kind::branch_cond: e_branch_cond(pi); return 1;
    case Kind::compare_branch: e_compare_branch(pi); return 1;
    case Kind::test_branch: e_test_branch(pi); return 1;
    case Kind::branch_reg:
    case Kind::ret: e_indirect(pi); return 1;
    case Kind::brk:
    case Kind::hlt:
    case Kind::svc: e_trap(pi); return 1;
    case Kind::udf: emit_interpret_exit(index_); return 1;
    }
    return 1;
}

// ---- operand helpers ----------------------------------------------------------

void RegionEmitter::shift_imm(Shift op, unsigned bits, Gp d, Gp s, unsigned amount) {
    amount &= bits - 1;
    if (amount == 0) {
        if (d != s || bits == 32) a_.mov(bits, d, s);
        return;
    }
    const auto& cpu = cpu_features();
    if (op == Shift::ror) {
        if (cpu.bmi2) {
            a_.rorx(bits, d, s, static_cast<std::uint8_t>(amount));
            return;
        }
        prepare_clobber();
        if (d != s) a_.mov(bits, d, s);
        a_.ror(bits, d, static_cast<std::uint8_t>(amount));
        return;
    }
    // LSL #1..3 into another register: one lea instead of mov + shl.
    if (keep_flags() || (d != s && isel_on(isel_no_srcreg))) {
        if (op == Shift::shl && amount == 1) { a_.lea(bits, d, ptr(s, s, 1)); return; }
        if (op == Shift::shl && amount <= 3) { a_.lea(bits, d, x64::ptr_index(s, 1u << amount)); return; }
    }
    if (keep_flags()) {
        if (cpu.bmi2) {
            const auto c = alloc();
            a_.mov_imm(c, amount);
            if (op == Shift::shl) a_.shlx(bits, d, s, c);
            else if (op == Shift::shr) a_.shrx(bits, d, s, c);
            else a_.sarx(bits, d, s, c);
            return;
        }
        prepare_clobber();
    }
    if (d != s) a_.mov(bits, d, s);
    a_.shift(op, bits, d, static_cast<std::uint8_t>(amount));
}

RegionEmitter::Val RegionEmitter::shifted(unsigned rm, unsigned type, unsigned amount, unsigned bits) {
    const auto mv = val(rm);
    if (amount == 0 || mv.is_zero()) return mv;
    const auto m = resolve(mv);   // a cached source needs no copy
    const auto t = alloc();
    Gp src = m.r;
    if (!m.is_reg()) {
        load(64, t, m);
        src = t;
    }
    shift_imm(shift_op(type), bits, t, src, amount);
    Val r;
    r.k = Val::K::reg;
    r.r = t;
    return r;
}

// Extended register operand (UXTB..SXTX) without the shift.
RegionEmitter::Val RegionEmitter::extended(unsigned rm, unsigned option, unsigned bits) {
    const auto m = val(rm);
    const unsigned src_bits = 8u << (option & 3);
    if (m.is_zero() || src_bits >= bits) return m;
    const bool sign = (option & 4) != 0;
    // UXTW/SXTW of a value its producer already extended: the register itself.
    if (src_bits == 32 && isel_on(isel_no_zext) && (ext_facts(plan_, index_, rm) & (sign ? ext_sign32 : ext_zero32)))
        return m;
    const Gp mr = m.is_reg() ? m.r : cached_gpr(m.g);
    const auto t = alloc();
    if (mr != Gp::none) {
        if (src_bits == 32) {
            if (sign) a_.movsx(64, t, mr, 32);
            else a_.mov(32, t, mr);
        } else if (sign) {
            a_.movsx(64, t, mr, src_bits);
        } else {
            a_.movzx(32, t, mr, src_bits);
        }
    } else {
        const auto mm = gmem(m.g);
        if (src_bits == 32) {
            if (sign) a_.movsx(64, t, mm, 32);
            else a_.mov(32, t, mm);
        } else if (sign) {
            a_.movsx(64, t, mm, src_bits);
        } else {
            a_.movzx(32, t, mm, src_bits);
        }
    }
    Val r;
    r.k = Val::K::reg;
    r.r = t;
    return r;
}

RegionEmitter::Val RegionEmitter::shl_val(const Val& vv, unsigned amount, unsigned bits) {
    if (amount == 0 || vv.is_zero()) return vv;
    const auto v = resolve(vv);
    const auto t = alloc();
    Gp src = v.r;
    if (!v.is_reg()) {
        load(64, t, v);
        src = t;
    }
    shift_imm(Shift::shl, bits, t, src, amount);
    Val r;
    r.k = Val::K::reg;
    r.r = t;
    return r;
}

// d = n + (m << scale) (scale <= 3); flag-free when the flags must survive.
void RegionEmitter::add_value(unsigned bits, Gp d, const Val& n0, const Val& m0, unsigned scale) {
    if (m0.is_zero()) {
        load(bits, d, n0);
        return;
    }
    // Cached sources count as registers: lea d, [n + m] instead of mov + add.
    const auto n = resolve(n0), m = resolve(m0);
    if (n.is_zero()) {
        if (scale == 0) {
            load(bits, d, m);
            return;
        }
        const auto mr = in_reg(m);
        if (scale == 1) a_.lea(bits, d, ptr(mr, mr, 1));
        else a_.lea(bits, d, x64::ptr_index(mr, 1u << scale));
        return;
    }
    if (scale == 0 && !keep_flags() &&
        (!n.is_reg() || !m.is_reg() || n.r == d || m.r == d)) {   // two-operand form; lea otherwise
        binop(Alu::add, bits, d, n, m, true);
        return;
    }
    const auto nr = in_reg(n);
    const auto mr = in_reg(m);
    a_.lea(bits, d, ptr(nr, mr, 1u << scale));
}

// d = n - m; flag-free (n + ~m + 1) when the flags must survive.
void RegionEmitter::sub_value(unsigned bits, Gp d, const Val& n, const Val& m, bool m_is_temp) {
    if (m.is_zero()) {
        load(bits, d, n);
        return;
    }
    if (!keep_flags()) {
        binop(Alu::sub, bits, d, n, m, false);
        return;
    }
    const auto t = m_is_temp ? m.r : copy_of(m);
    a_.not_(bits, t);
    if (n.is_zero()) {
        a_.lea(bits, d, ptr(t, 1));
    } else {
        const auto nr = in_reg(n);
        a_.lea(bits, d, ptr(nr, t, 1, 1));
    }
}

void RegionEmitter::compare(unsigned bits, const Val& n, const Val& m) {
    if (n.is_zero()) {
        const auto t = alloc();
        zero(t);
        alu(Alu::cmp, bits, t, m);
        return;
    }
    if (n.is_reg()) {
        alu(Alu::cmp, bits, n.r, m);
        return;
    }
    if (const auto h = cached_gpr(n.g); h != Gp::none) {
        alu(Alu::cmp, bits, h, m);
        return;
    }
    if (m.is_reg()) a_.alu(Alu::cmp, bits, gmem(n.g), m.r);
    else if (m.is_zero()) a_.cmp(bits, gmem(n.g), 0);
    else a_.alu(Alu::cmp, bits, gmem(n.g), in_reg(m));
}

void RegionEmitter::alu_imm(Alu op, unsigned bits, Gp d, std::uint64_t imm) {
    if (fits_imm32(imm, bits)) {
        a_.alu(op, bits, d, static_cast<std::int32_t>(static_cast<std::uint32_t>(imm)));
        return;
    }
    const auto t = alloc();
    mov_literal(t, imm);
    a_.alu(op, bits, d, t);
}

// d = sign/zero-extended n[lsb + len - 1 : lsb].
void RegionEmitter::field_extract(unsigned bits, Gp d, Gp n, unsigned lsb, unsigned len, bool sign) {
    if (lsb == 0) {
        if (len >= bits) {
            if (d != n || bits == 32) a_.mov(bits, d, n);
            return;
        }
        if (len == 8 || len == 16) {
            if (sign) a_.movsx(bits, d, n, len);
            else a_.movzx(32, d, n, len);
            return;
        }
        if (len == 32) {
            if (sign) a_.movsx(64, d, n, 32);
            else a_.mov(32, d, n);
            return;
        }
    }
    if (lsb + len == bits) {
        shift_imm(sign ? Shift::sar : Shift::shr, bits, d, n, lsb);
        return;
    }
    const bool bmi2 = cpu_features().bmi2 && isel_on(isel_no_srcreg);
    if (bmi2 && (len == 8 || len == 16 || len == 32)) {
        // Rotate the field to the bottom (copying n), then extend it: two
        // flag-free instructions.
        a_.rorx(bits, d, n, static_cast<std::uint8_t>(lsb));
        if (len == 32) {
            if (sign) a_.movsx(64, d, d, 32);
            else a_.mov(32, d, d);
        } else if (sign) {
            a_.movsx(bits, d, d, len);
        } else {
            a_.movzx(32, d, d, len);
        }
        return;
    }
    if (bmi2 && !sign && len <= 31 && !keep_flags() && d != n) {
        prepare_clobber();
        a_.rorx(bits, d, n, static_cast<std::uint8_t>(lsb));   // copy + move the field down
        a_.and_(bits, d, static_cast<std::int32_t>(ones(len)));
        return;
    }
    if (!sign && len <= 31 && !keep_flags()) {
        shift_imm(Shift::shr, bits, d, n, lsb);
        a_.and_(bits, d, static_cast<std::int32_t>(ones(len)));
        return;
    }
    const unsigned left = bits - lsb - len, right = bits - len;
    if (keep_flags() && cpu_features().bmi2) {
        const auto c = alloc();
        a_.mov_imm(c, left);
        a_.shlx(bits, d, n, c);
        a_.mov_imm(c, right);
        if (sign) a_.sarx(bits, d, d, c);
        else a_.shrx(bits, d, d, c);
        return;
    }
    prepare_clobber();
    if (d != n) a_.mov(bits, d, n);
    if (left) a_.shl(bits, d, static_cast<std::uint8_t>(left));
    a_.shift(sign ? Shift::sar : Shift::shr, bits, d, static_cast<std::uint8_t>(right));
}

// d = extend(n[len - 1 : 0]) << pos.
void RegionEmitter::field_insert_zero(unsigned bits, Gp d, Gp n, unsigned pos, unsigned len, bool sign) {
    if (pos + len >= bits) {
        shift_imm(Shift::shl, bits, d, n, pos);
        return;
    }
    if (pos == 0) {
        field_extract(bits, d, n, 0, len, sign);
        return;
    }
    if (len == 8 || len == 16 || len == 32) {
        field_extract(bits, d, n, 0, len, sign);
        shift_imm(Shift::shl, bits, d, d, pos);
        return;
    }
    if (!sign && pos + len <= 31 && !keep_flags() && d != n && cpu_features().bmi2 && isel_on(isel_no_srcreg)) {
        prepare_clobber();
        a_.rorx(bits, d, n, static_cast<std::uint8_t>(bits - pos));   // copy + move the field up
        a_.and_(bits, d, static_cast<std::int32_t>(ones(len) << pos));
        return;
    }
    const unsigned left = bits - len, right = bits - len - pos;
    if (keep_flags() && cpu_features().bmi2) {
        const auto c = alloc();
        a_.mov_imm(c, left);
        a_.shlx(bits, d, n, c);
        a_.mov_imm(c, right);
        if (sign) a_.sarx(bits, d, d, c);
        else a_.shrx(bits, d, d, c);
        return;
    }
    prepare_clobber();
    if (d != n) a_.mov(bits, d, n);
    a_.shl(bits, d, static_cast<std::uint8_t>(left));
    a_.shift(sign ? Shift::sar : Shift::shr, bits, d, static_cast<std::uint8_t>(right));
}

// ---- data processing (immediate) ----------------------------------------------

void RegionEmitter::e_add_sub_imm(const Inst& d) {
    const unsigned bits = d.sf ? 64 : 32;
    const bool sub = d.op != 0;
    const auto imm = static_cast<std::int32_t>(d.imm);
    const auto n = val(d.rn, true);
    if (!d.s) {
        const auto t = dst(d.rd, true);
        if (imm == 0) {
            load(bits, t, n);
            return;
        }
        const auto nv = resolve(n);   // a cached source needs no copy
        Gp nr = nv.r;
        if (!nv.is_reg()) {
            load(64, t, nv);
            nr = t;
        }
        a_.lea(bits, t, ptr(nr, sub ? -imm : imm));
        return;
    }
    // CMP #imm; CMN #imm (imm != 0) is CMP #-imm: same result, N Z V, and
    // C = carry of n + imm = !borrow of n - (-imm) -- the sub producer.
    const bool cmn_as_cmp = d.rd == 31 && !sub && imm != 0 && isel_on(isel_no_srcreg);
    if (d.rd == 31 && (sub || cmn_as_cmp)) {
        const auto k = sub ? imm : -imm;
        const Gp h = n.is_mem() ? cached_gpr(n.g) : n.r;
        if (h == Gp::none) a_.cmp(bits, gmem(n.g), k);
        else a_.cmp(bits, h, k);
        set_flags(FlagsProducer::sub);
        return;
    } else {
        const auto t = d.rd == 31 ? alloc() : dst(d.rd);
        if (!(n.is_reg() && n.r == t)) load(bits, t, n);
        if (sub) a_.sub(bits, t, imm);
        else a_.add(bits, t, imm);
    }
    set_flags(sub ? FlagsProducer::sub : FlagsProducer::add);
}

void RegionEmitter::e_logic_imm(const Inst& d) {
    const unsigned bits = d.sf ? 64 : 32;
    const auto imm = static_cast<std::uint64_t>(d.imm) & ones(bits);
    const auto n = val(d.rn);
    static constexpr Alu ops[4] = {Alu::and_, Alu::or_, Alu::xor_, Alu::and_};
    if (d.op == 3) {   // ANDS / TST
        if (d.rd == 31) {
            if (fits_imm32(imm, bits)) {
                const auto i32 = static_cast<std::int32_t>(static_cast<std::uint32_t>(imm));
                // [r15 + disp8][imm32 1] would read as a module-range window: test a register.
                if (n.is_mem() && i32 != 1 && cached_gpr(n.g) == Gp::none) a_.test(bits, gmem(n.g), i32);
                else a_.test(bits, in_reg(n), i32);
            } else {
                const auto t = alloc();
                mov_literal(t, imm);
                a_.test(bits, in_reg(n), t);
            }
        } else {
            const auto t = dst(d.rd);
            if (!(n.is_reg() && n.r == t)) load(bits, t, n);
            alu_imm(Alu::and_, bits, t, imm);
        }
        set_flags(FlagsProducer::logic);
        return;
    }
    const auto t = dst(d.rd, true);
    if (n.is_zero()) {
        if (d.op == 0) a_.mov_imm(t, 0);
        else mov_literal(t, imm);
        return;
    }
    if (d.op == 0) {
        if (imm == 0xff || imm == 0xffff) {
            const unsigned width = imm == 0xff ? 8 : 16;
            const Gp h = n.is_reg() ? n.r : cached_gpr(n.g);
            if (h != Gp::none) a_.movzx(32, t, h, width);
            else a_.movzx(32, t, gmem(n.g), width);
            return;
        }
        if (imm == 0xffffffffULL) {
            load(32, t, n);
            return;
        }
    }
    prepare_clobber();
    if (!(n.is_reg() && n.r == t)) load(bits, t, n);
    alu_imm(ops[d.op], bits, t, imm);
}

unsigned RegionEmitter::e_move_wide(const PlanInst& pi) {
    const auto& d = pi.inst;
    const unsigned bits = d.sf ? 64 : 32;
    const unsigned shift = d.amount;
    if (d.op == 3) {   // MOVK
        const auto imm16 = static_cast<std::uint16_t>(d.imm);
        if (d.rd == 31) return 1;
        // MOVK of a register holding a known constant (MOVZ/MOVN earlier in
        // the block, not adjacent): the whole new constant in one mov.
        if (std::uint64_t prev = 0; isel_on(isel_no_movk) && known_constant(plan_, index_, d.rd, prev)) {
            const auto value = ((prev & ~(0xffffULL << shift)) | (static_cast<std::uint64_t>(imm16) << shift)) & ones(bits);
            mov_literal(dst(d.rd), value);
            return 1;
        }
        note_write(d.rd);
        if (!pinned(d.rd)) {
            if (gc_.host[d.rd] >= 0) evict_gpr(static_cast<unsigned>(gc_.host[d.rd]));   // slot changes in place
            a_.mov(16, ptr(Gp::r15, x_disp(d.rd) + static_cast<std::int32_t>(shift / 8)), imm16);
            if (!d.sf) a_.mov(32, ptr(Gp::r15, x_disp(d.rd) + 4), 0);
            return 1;
        }
        const auto r = pin(d.rd);
        if (shift == 0) {
            mov16_imm(a_, r, imm16);
            if (!d.sf) a_.mov(32, r, r);
            return 1;
        }
        if (cpu_features().bmi2) {
            a_.rorx(bits, r, r, static_cast<std::uint8_t>(shift));
            mov16_imm(a_, r, imm16);
            a_.rorx(bits, r, r, static_cast<std::uint8_t>(bits - shift));
            return 1;
        }
        prepare_clobber();
        const auto t = alloc();
        mov_literal(t, ~(0xffffULL << shift) & ones(bits));
        a_.and_(bits, r, t);
        mov_literal(t, static_cast<std::uint64_t>(imm16) << shift);
        a_.or_(bits, r, t);
        return 1;
    }
    std::uint64_t value = static_cast<std::uint64_t>(d.imm) << shift;
    if (d.op == 0) value = ~value;
    value &= ones(bits);
    // Fuse the MOVKs that complete the constant.
    unsigned consumed = 1;
    const auto n = static_cast<std::int32_t>(plan_.insts.size());
    for (auto j = index_ + 1; j < n; ++j) {
        const auto& pj = plan_.insts[j];
        if (pj.leader || pj.pc != pi.pc + 4ull * consumed || pj.inst.kind != Kind::move_wide || pj.inst.op != 3 ||
            pj.inst.rd != d.rd || pj.inst.sf != d.sf)
            break;
        const unsigned s = pj.inst.amount;
        value = (value & ~(0xffffULL << s)) | (static_cast<std::uint64_t>(pj.inst.imm) << s);
        ++consumed;
    }
    const auto t = dst(d.rd);
    mov_literal(t, value);
    return consumed;
}

unsigned RegionEmitter::e_adr(const PlanInst& pi) {
    const auto& d = pi.inst;
    auto value = static_cast<std::uint64_t>(d.imm);
    unsigned consumed = 1;
    // ADRP xd; ADD xd, xd, #lo12  ->  one relocatable constant.
    if (index_ + 1 < static_cast<std::int32_t>(plan_.insts.size())) {
        const auto& next = plan_.insts[index_ + 1];
        if (!next.leader && next.pc == pi.pc + 4 && next.inst.kind == Kind::add_sub_imm && next.inst.op == 0 &&
            !next.inst.s && next.inst.sf && next.inst.rd == d.rd && next.inst.rn == d.rd && d.rd != 31) {
            value += static_cast<std::uint64_t>(next.inst.imm);
            consumed = 2;
        }
    }
    const auto t = dst(d.rd);
    fixup_imm64(t, value);
    return consumed;
}

void RegionEmitter::e_bitfield(const Inst& d) {
    const unsigned bits = d.sf ? 64 : 32;
    const unsigned r = d.amount, s = d.imms;
    if (d.op == 1) {   // BFM: BFI / BFXIL
        if (d.rd == 31) return;
        const auto nv = val(d.rn);
        const unsigned pos = s >= r ? 0 : bits - r;
        const unsigned len = s >= r ? s - r + 1 : s + 1;
        const unsigned src_lsb = s >= r ? r : 0;
        const auto dd = dst_rmw(d.rd);
        if (pos == 0 && src_lsb == 0 && (len == 8 || len == 16) && !nv.is_zero()) {
            const auto nr = in_reg(nv);
            a_.mov(len, dd, nr);   // partial register write keeps the other bits
            if (bits == 32) a_.mov(32, dd, dd);
            return;
        }
        prepare_clobber();
        Gp t = Gp::none;
        if (!nv.is_zero()) {
            t = alloc();
            const auto nr = in_reg(nv);
            if (s >= r) field_extract(bits, t, nr, src_lsb, len, false);
            else field_insert_zero(bits, t, nr, pos, len, false);
        }
        const auto inv = ~(ones(len) << pos) & ones(bits);
        alu_imm(Alu::and_, bits, dd, inv);
        if (t != Gp::none) a_.or_(bits, dd, t);
        return;
    }
    const bool sign = d.op == 0;
    const auto dd = dst(d.rd);
    const auto nv = resolve(val(d.rn));   // a cached source needs no copy
    if (nv.is_zero()) {
        zero(dd);
        return;
    }
    Gp n = nv.r;
    if (!nv.is_reg()) {
        load(64, dd, nv);
        n = dd;
    }
    if (s >= r) field_extract(bits, dd, n, r, s - r + 1, sign);
    else field_insert_zero(bits, dd, n, bits - r, s + 1, sign);
}

void RegionEmitter::e_extr(const Inst& d) {
    const unsigned bits = d.sf ? 64 : 32, lsb = d.amount;
    if (d.rn == d.rm) {   // ROR (immediate)
        const auto dd = dst(d.rd);
        const auto v = resolve(val(d.rn));   // a cached source needs no copy (rorx)
        if (v.is_zero()) {
            zero(dd);
            return;
        }
        Gp n = v.r;
        if (!v.is_reg()) {
            load(64, dd, v);
            n = dd;
        }
        shift_imm(Shift::ror, bits, dd, n, lsb);
        return;
    }
    const auto nv = val(d.rn), mv = val(d.rm);
    const auto dd = dst(d.rd);
    if (lsb == 0) {
        load(bits, dd, mv);
        return;
    }
    const auto m = in_reg(mv);
    const auto n = in_reg(nv);
    if (keep_flags() && cpu_features().bmi2) {
        // (m >> lsb) and (n << (width - lsb)) have disjoint bits: combine with lea.
        const auto c = alloc();
        const auto t1 = dd != n ? dd : alloc();
        const auto t2 = alloc();
        a_.mov_imm(c, lsb);
        a_.shrx(bits, t1, m, c);
        a_.mov_imm(c, bits - lsb);
        a_.shlx(bits, t2, n, c);
        a_.lea(bits, dd, ptr(t1, t2, 1));
        return;
    }
    prepare_clobber();
    if (dd != n) {
        if (dd != m) a_.mov(bits, dd, m);
        a_.shrd(bits, dd, n, static_cast<std::uint8_t>(lsb));
        return;
    }
    const auto t = alloc();
    a_.mov(bits, t, m);
    a_.shrd(bits, t, n, static_cast<std::uint8_t>(lsb));
    a_.mov(bits, dd, t);
}

// ---- data processing (register) -----------------------------------------------

void RegionEmitter::e_add_sub_shift(const Inst& d) {
    const unsigned bits = d.sf ? 64 : 32;
    const bool sub = d.op != 0;
    const auto n = val(d.rn);
    if (!d.s) {
        const auto dd = dst(d.rd);
        if (!sub && d.shift == 0 && d.amount <= 3) {
            add_value(bits, dd, n, val(d.rm), d.amount);
            return;
        }
        const auto m = shifted(d.rm, d.shift, d.amount, bits);
        if (sub) sub_value(bits, dd, n, m);
        else add_value(bits, dd, n, m, 0);
        return;
    }
    const auto m = shifted(d.rm, d.shift, d.amount, bits);
    if (sub) {
        if (d.rd == 31) compare(bits, n, m);
        else binop(Alu::sub, bits, dst(d.rd), n, m, false);
        set_flags(FlagsProducer::sub);
    } else {
        if (d.rd == 31) {
            const auto t = alloc();
            load(bits, t, n);
            alu(Alu::add, bits, t, m);
        } else {
            binop(Alu::add, bits, dst(d.rd), n, m, true);
        }
        set_flags(FlagsProducer::add);
    }
}

void RegionEmitter::e_add_sub_ext(const Inst& d) {
    const unsigned bits = d.sf ? 64 : 32;
    const bool sub = d.op != 0;
    const auto n = val(d.rn, true);
    auto m = extended(d.rm, d.shift, bits);
    unsigned sh = d.amount;
    if (!d.s) {
        const auto dd = dst(d.rd, true);
        if (!sub && sh <= 3) {
            add_value(bits, dd, n, m, sh);
            return;
        }
        m = shl_val(m, sh, bits);
        if (sub) sub_value(bits, dd, n, m);
        else add_value(bits, dd, n, m, 0);
        return;
    }
    m = shl_val(m, sh, bits);
    if (sub) {
        if (d.rd == 31) compare(bits, n, m);
        else binop(Alu::sub, bits, dst(d.rd), n, m, false);
        set_flags(FlagsProducer::sub);
    } else {
        if (d.rd == 31) {
            const auto t = alloc();
            load(bits, t, n);
            alu(Alu::add, bits, t, m);
        } else {
            binop(Alu::add, bits, dst(d.rd), n, m, true);
        }
        set_flags(FlagsProducer::add);
    }
}

void RegionEmitter::e_adc(const Inst& d) {
    const unsigned bits = d.sf ? 64 : 32;
    const bool sbc = d.op != 0;
    if (is_fcmp(flags_)) materialize();   // the carry-in comes from ctx->nzcv below
    if (!d.s && is_host(flags_) && live_out_) materialize();
    auto n = val(d.rn), m = val(d.rm);
    const auto dd = d.rd == 31 ? alloc() : dst(d.rd);
    if (!sbc && m.is_reg() && m.r == dd && !(n.is_reg() && n.r == dd)) std::swap(n, m);
    Gp acc = dd;
    if (m.is_reg() && m.r == dd && !(n.is_reg() && n.r == dd)) acc = alloc();
    if (!(n.is_reg() && n.r == acc)) {
        // Flag-free: the carry-in still lives in the current flags.
        if (n.is_zero()) a_.mov_imm(acc, 0);
        else load(bits, acc, n);
    }
    bool want_c = !sbc;   // ADC wants CF = C, SBC wants CF = !C
    switch (flags_) {
    case FlagsState::host_sub:
        if (want_c) a_.cmc();
        break;
    case FlagsState::host_add:
    case FlagsState::host_logic:
        if (!want_c) a_.cmc();
        break;
    default:   // ctx (fused FCMP flags were materialized above)
        a_.emit_rm(0, false, {0x0f, 0xba}, 4, ptr(Gp::r15, ctx_disp(offsetof(GuestContext, nzcv))), 1);
        a_.db(29);   // bt dword [nzcv], 29 -> CF = C
        if (!want_c) a_.cmc();
        break;
    }
    alu(sbc ? Alu::sbb : Alu::adc, bits, acc, m);
    if (acc != dd) a_.mov(bits, dd, acc);
    if (d.s) set_flags(sbc ? FlagsProducer::sub : FlagsProducer::add);
    else flags_ = FlagsState::ctx;
}

void RegionEmitter::e_logic_shift(const Inst& d) {
    const unsigned bits = d.sf ? 64 : 32;
    const auto n = val(d.rn);
    const auto mraw = val(d.rm);
    if (!d.s) {
        if (n.is_zero()) {   // MOV / MVN / zero
            const auto dd = dst(d.rd);
            if (d.op == 0) {   // AND/BIC with XZR
                a_.mov_imm(dd, 0);
                return;
            }
            const auto ms = shifted(d.rm, d.shift, d.amount, bits);
            load(bits, dd, ms);
            if (d.invert) a_.not_(bits, dd);
            return;
        }
        if (mraw.is_zero()) {
            const auto dd = dst(d.rd);
            if (!d.invert) {
                if (d.op == 0) a_.mov_imm(dd, 0);
                else load(bits, dd, n);
            } else if (d.op == 0) {
                load(bits, dd, n);
            } else if (d.op == 1) {
                mov_literal(dd, ones(bits));
            } else {
                load(bits, dd, n);
                a_.not_(bits, dd);
            }
            return;
        }
        prepare_clobber();
    }
    auto ms = shifted(d.rm, d.shift, d.amount, bits);
    static constexpr Alu ops[4] = {Alu::and_, Alu::or_, Alu::xor_, Alu::and_};
    if (d.invert) {
        if ((d.op == 0 || d.op == 3) && cpu_features().bmi1) {   // BIC / BICS: andn
            const auto mr = in_reg(ms);
            const auto nr = in_reg(n);
            const auto dd = d.rd == 31 ? alloc() : dst(d.rd);
            a_.andn(bits, dd, mr, nr);
            if (d.s) set_flags(FlagsProducer::logic);
            return;
        }
        const auto t = alloc();
        load(bits, t, ms);
        a_.not_(bits, t);
        ms = Val{Val::K::reg, t, 0};
    }
    if (d.s && d.rd == 31) {   // TST
        const auto mr = in_reg(ms);
        const auto nr = in_reg(n);
        a_.test(bits, nr, mr);
        set_flags(FlagsProducer::logic);
        return;
    }
    binop(ops[d.op], bits, dst(d.rd), n, ms, true);
    if (d.s) set_flags(FlagsProducer::logic);
}

void RegionEmitter::e_ccmp(const Inst& d) {
    const unsigned bits = d.sf ? 64 : 32;
    const bool ccmn = d.op == 0;
    take(Gp::rax);
    const auto nv = val(d.rn);
    Val mv;
    if (!d.imm_form) mv = val(d.rm);
    // Operands for the compare, prepared without touching the flags.
    Gp nr = Gp::none;
    if (ccmn || nv.is_zero()) {
        nr = alloc();
        if (nv.is_zero()) a_.mov_imm(nr, 0);
        else load(64, nr, nv);
    }
    Gp mr = Gp::none;
    if (!d.imm_form && mv.is_mem() && (nv.is_mem() && nr == Gp::none)) mr = in_reg(mv);
    const auto do_compare = [&] {
        if (ccmn) {
            if (d.imm_form) a_.add(bits, nr, static_cast<std::int32_t>(d.imm));
            else if (mv.is_zero()) a_.add(bits, nr, 0);
            else alu(Alu::add, bits, nr, mv);
            return;
        }
        if (d.imm_form) {
            if (nr != Gp::none) a_.cmp(bits, nr, static_cast<std::int32_t>(d.imm));
            else if (nv.is_mem() && cached_gpr(nv.g) != Gp::none) a_.cmp(bits, cached_gpr(nv.g), static_cast<std::int32_t>(d.imm));
            else if (nv.is_mem()) a_.cmp(bits, gmem(nv.g), static_cast<std::int32_t>(d.imm));
            else a_.cmp(bits, nv.r, static_cast<std::int32_t>(d.imm));
            return;
        }
        if (nr != Gp::none) { alu(Alu::cmp, bits, nr, mv); return; }
        if (nv.is_reg()) { alu(Alu::cmp, bits, nv.r, mv); return; }
        if (const auto h = cached_gpr(nv.g); h != Gp::none) { alu(Alu::cmp, bits, h, mv); return; }
        if (mv.is_zero()) a_.cmp(bits, gmem(nv.g), 0);
        else if (mv.is_reg()) a_.alu(Alu::cmp, bits, gmem(nv.g), mv.r);
        else a_.alu(Alu::cmp, bits, gmem(nv.g), mr);
    };
    const auto producer = ccmn ? FlagsProducer::add : FlagsProducer::sub;
    if (d.cond >= 14) {
        do_compare();
        set_flags(producer);
        return;
    }
    const auto c = condition(d.cond);
    const auto lfalse = a_.new_label(), ldone = a_.new_label();
    a_.jcc_short(x64::invert(c), lfalse);
    do_compare();
    a_.jmp_short(ldone);
    a_.bind(lfalse);
    {
        const unsigned nz = d.imms;
        const bool fn = nz & 8, fz = nz & 4, fc = nz & 2, fv = nz & 1;
        const bool cf = ccmn ? fc : !fc;
        const unsigned ah = (fn ? 0x80u : 0u) | (fz ? 0x40u : 0u) | (cf ? 1u : 0u);
        a_.mov_imm(Gp::rax, (ah << 8) | (fv ? 1u : 0u));
        a_.alu(Alu::add, 8, Gp::rax, 0x7f);   // OF = (al == 1)
        a_.sahf();                            // SF ZF CF from ah
    }
    a_.bind(ldone);
    set_flags(producer);
}

void RegionEmitter::e_csel(const Inst& d) {
    const unsigned bits = d.sf ? 64 : 32;
    const auto n = val(d.rn), m = val(d.rm);
    const auto dd = dst(d.rd);
    if (d.cond >= 14) {
        load(bits, dd, n);
        return;
    }
    // Fused FCMP flags whose condition needs the parity flag as well: two cmovs.
    const bool pair = is_fcmp(flags_) && fcmp_cond(d.cond).k != FcmpCond::K::single;
    if (d.op == 1 && n.is_zero() && m.is_zero() && !pair) {   // CSET
        a_.mov_imm(dd, 0);
        const auto c = condition(d.cond);
        a_.setcc(x64::invert(c), dd);
        return;
    }
    if (d.op == 2 && n.is_zero() && m.is_zero() && !pair) {   // CSETM
        const auto t = alloc();
        a_.mov_imm(dd, 0);
        mov_literal(t, ones(bits));
        const auto c = condition(d.cond);
        a_.cmov(x64::invert(c), bits, dd, t);
        return;
    }
    // x = else-value f(m)
    const bool dd_is_n = n.is_reg() && n.r == dd;
    const Gp x = dd_is_n ? alloc() : dd;
    switch (d.op) {
    case 0:
        if (m.is_zero()) a_.mov_imm(x, 0);
        else load(bits, x, m);
        break;
    case 1:
        if (m.is_zero()) {
            a_.mov_imm(x, 1);
        } else {
            const auto mv = resolve(m);   // a cached source needs no copy
            Gp mr = mv.r;
            if (!mv.is_reg()) {
                load(64, x, mv);
                mr = x;
            }
            a_.lea(bits, x, ptr(mr, 1));
        }
        break;
    case 2:
        if (m.is_zero() && isel_on(isel_no_srcreg)) {   // ~XZR: all ones
            mov_literal(x, ones(bits));
            break;
        }
        if (m.is_zero()) a_.mov_imm(x, 0);
        else load(bits, x, m);
        a_.not_(bits, x);
        break;
    default:
        if (m.is_zero() && isel_on(isel_no_srcreg)) {   // -XZR: zero
            a_.mov_imm(x, 0);
            break;
        }
        if (m.is_zero()) a_.mov_imm(x, 0);
        else load(bits, x, m);
        a_.not_(bits, x);
        a_.lea(bits, x, ptr(x, 1));
        break;
    }
    if (pair) {
        const auto fc = fcmp_cond(d.cond);
        if (dd_is_n) {   // dd holds n: replace it by x unless (c AND ordered) / (c OR unordered)
            if (fc.k == FcmpCond::K::and_np) {
                a_.cmov(x64::invert(fc.c), bits, dd, x);
                a_.cmov(Cond::p, bits, dd, x);
            } else {
                a_.cmov(fc.c, bits, x, dd);
                a_.cmov(Cond::p, bits, x, dd);
                a_.mov(bits, dd, x);
            }
            return;
        }
        // t = n in a writable register.
        const auto t = alloc();
        if (n.is_zero()) a_.mov_imm(t, 0);
        else load(bits, t, n);
        if (fc.k == FcmpCond::K::and_np) {
            a_.cmov(Cond::p, bits, t, x);   // unordered: else-value
            a_.cmov(fc.c, bits, x, t);
        } else {
            a_.cmov(fc.c, bits, x, t);
            a_.cmov(Cond::p, bits, x, t);
        }
        return;
    }
    if (dd_is_n) {
        const auto c = condition(d.cond);
        a_.cmov(x64::invert(c), bits, dd, x);
        return;
    }
    Gp zr = Gp::none;
    if (n.is_zero()) {
        zr = alloc();
        a_.mov_imm(zr, 0);
    }
    const auto c = condition(d.cond);
    if (n.is_zero()) a_.cmov(c, bits, x, zr);
    else if (n.is_reg()) a_.cmov(c, bits, x, n.r);
    else if (const auto h = cached_gpr(n.g); h != Gp::none) a_.cmov(c, bits, x, h);
    else a_.cmov(c, bits, x, gmem(n.g));
}

// RBIT Xd, Xn whose only reader is a CLZ Xm, Xd a few instructions later
// (the ARM64 count-trailing-zeros idiom), with Xd overwritten before any other
// read and nothing in between that may fault or leave the block: RBIT emits
// nothing and the CLZ emits TZCNT Xm, Xn. Xd is never observable meanwhile.
std::int32_t RegionEmitter::rbit_clz_target(const Inst& d) const {
    if (!plan_.rbit_fusion || !cpu_features().bmi1 || d.rd == 31) return -1;
    const auto n = static_cast<std::int32_t>(plan_.insts.size());
    const std::uint32_t xd = 1u << d.rd, xn = d.rn == 31 ? 0u : 1u << d.rn;
    std::int32_t clz = -1;
    for (std::int32_t j = index_ + 1; j < n && j <= index_ + 16; ++j) {
        const auto& pj = plan_.insts[j];
        if (pj.leader || pj.flow != Flow::normal || pj.may_fault || pj.conservative_writes) return -1;
        if (clz < 0) {
            if (pj.reads & xd) {
                const auto& c = pj.inst;
                if (c.kind != Kind::dp1 || c.op != dp1_clz || c.sf != d.sf || c.rn != d.rd) return -1;
                clz = j;
                if (c.rd == d.rd) return clz;   // the CLZ overwrites Xd itself
                continue;
            }
            if (pj.writes & (xd | xn)) return -1;
        } else {
            if (pj.reads & xd) return -1;
            if (pj.writes & xd) return clz;     // Xd overwritten: the RBIT value is dead
        }
    }
    return -1;
}

void RegionEmitter::e_dp1(const Inst& d) {
    const unsigned bits = d.sf ? 64 : 32;
    if (d.op == dp1_rbit) {
        if (const auto j = rbit_clz_target(d); j >= 0) {
            tz_clz_ = j;   // emitted by the CLZ as TZCNT of Xn
            tz_src_ = static_cast<std::uint8_t>(d.rn);
            return;
        }
    } else if (d.op == dp1_clz && index_ == tz_clz_) {
        tz_clz_ = -1;
        const auto src = val(tz_src_);
        const auto dd = dst(d.rd);
        prepare_clobber();
        if (src.is_zero()) a_.mov_imm(dd, bits);
        else a_.tzcnt(bits, dd, in_reg(src));
        return;
    }
    const auto nv = val(d.rn);
    const auto dd = dst(d.rd);
    const auto& cpu = cpu_features();
    switch (d.op) {
    case dp1_rev:
        load(bits, dd, nv);
        a_.bswap(bits, dd);
        return;
    case dp1_rev32:   // 64-bit only
        load(64, dd, nv);
        a_.bswap(64, dd);
        if (cpu.bmi2) a_.rorx(64, dd, dd, 32);
        else { prepare_clobber(); a_.ror(64, dd, 32); }
        return;
    case dp1_rev16:
        if (bits == 32) {
            load(32, dd, nv);
            a_.bswap(32, dd);
            if (cpu.bmi2) a_.rorx(32, dd, dd, 16);
            else { prepare_clobber(); a_.ror(32, dd, 16); }
            return;
        }
        {
            prepare_clobber();
            const auto t = alloc(), u = alloc(), k = alloc();
            mov_literal(k, 0x00ff00ff00ff00ffULL);
            load(64, t, nv);
            a_.mov(64, u, t);
            a_.shr(64, u, 8);
            a_.and_(64, u, k);
            a_.and_(64, t, k);
            a_.shl(64, t, 8);
            a_.or_(64, t, u);
            a_.mov(64, dd, t);
        }
        return;
    case dp1_clz: {
        prepare_clobber();
        const auto nr = in_reg(nv);
        if (cpu.lzcnt) {
            a_.lzcnt(bits, dd, nr);
            return;
        }
        const auto t = alloc(), c = alloc();
        a_.mov_imm(c, ones(64));
        bsr(a_, bits, t, nr);
        a_.cmov(Cond::e, 64, t, c);
        a_.neg(64, t);
        a_.lea(bits, dd, ptr(t, static_cast<std::int32_t>(bits - 1)));
        return;
    }
    case dp1_cls: {
        prepare_clobber();
        const auto nr = in_reg(nv);
        const auto t = alloc();
        a_.lea(bits, t, ptr(nr, nr, 1));
        a_.xor_(bits, t, nr);
        a_.or_(bits, t, 1);
        if (cpu.lzcnt) {
            a_.lzcnt(bits, dd, t);
        } else {
            const auto u = alloc();
            bsr(a_, bits, u, t);
            a_.mov_imm(dd, bits - 1);
            a_.sub(bits, dd, u);
        }
        return;
    }
    default: {   // RBIT
        if (cpu.gfni && cpu.avx && std::popcount(free_xmm_) >= 2) {
            // Reverse the bits of every byte with GF2P8AFFINEQB (matrix
            // 0x8040201008040201), then the byte order with BSWAP. Flag-free.
            const auto k = alloc();
            mov_literal(k, 0x8040201008040201ULL);
            const auto x = alloc_vec(), m = alloc_vec();
            const bool w = bits == 64;
            if (nv.is_zero()) a_.vex_rr({1, 1, 0xef, false}, false, x64::id(x), x64::id(x), x64::id(x));   // vpxor
            else a_.vex_rr({1, 1, 0x6e, w}, false, x64::id(x), 0, x64::id(in_reg(nv)));                   // vmovq/d x, n
            a_.vex_rr({1, 1, 0x6e, true}, false, x64::id(m), 0, x64::id(k));                               // vmovq m, k
            a_.vex_rr({1, 3, 0xce, true}, false, x64::id(x), x64::id(x), x64::id(m));                      // vgf2p8affineqb
            a_.db(0);
            a_.vex_rr({1, 1, 0x7e, w}, false, x64::id(x), 0, x64::id(dd));                                // vmovq/d dd, x
            a_.bswap(bits, dd);
            return;
        }
        prepare_clobber();
        const auto t = alloc(), u = alloc();
        load(bits, t, nv);
        a_.bswap(bits, t);
        struct Step { unsigned k; std::uint64_t mask; };
        static constexpr Step steps[3] = {{4, 0x0f0f0f0f0f0f0f0fULL}, {2, 0x3333333333333333ULL}, {1, 0x5555555555555555ULL}};
        const auto k = bits == 64 ? alloc() : Gp::none;
        for (const auto& s : steps) {
            a_.mov(bits, u, t);
            a_.shr(bits, u, static_cast<std::uint8_t>(s.k));
            if (bits == 64) {
                mov_literal(k, s.mask);
                a_.and_(64, u, k);
                a_.and_(64, t, k);
            } else {
                a_.and_(32, u, static_cast<std::int32_t>(static_cast<std::uint32_t>(s.mask)));
                a_.and_(32, t, static_cast<std::int32_t>(static_cast<std::uint32_t>(s.mask)));
            }
            if (s.k == 4) {
                a_.shl(bits, t, 4);
                a_.or_(bits, t, u);
            } else {
                a_.lea(bits, t, ptr(u, t, 1u << s.k));
            }
        }
        a_.mov(bits, dd, t);
        return;
    }
    }
}

void RegionEmitter::e_dp2(const Inst& d) {
    const unsigned bits = d.sf ? 64 : 32;
    const auto& cpu = cpu_features();
    if (d.op == dp2_udiv || d.op == dp2_sdiv) {
        take(Gp::rax);
        take(Gp::rdx);
        const auto nv = val(d.rn), mv = val(d.rm);
        const auto dd = dst(d.rd);
        if (mv.is_zero()) {
            a_.mov_imm(dd, 0);
            return;
        }
        prepare_clobber();
        const auto dv = in_reg(mv);
        const auto done = a_.new_label();
        a_.xor_(32, Gp::rax, Gp::rax);
        a_.test(bits, dv, dv);
        a_.jcc_short(Cond::e, done);
        load(bits, Gp::rax, nv);
        if (d.op == dp2_sdiv) {
            const auto do_div = a_.new_label();
            a_.cmp(bits, dv, -1);
            a_.jcc_short(Cond::ne, do_div);
            a_.neg(bits, Gp::rax);   // x / -1 = -x (INT_MIN / -1 = INT_MIN)
            a_.jmp_short(done);
            a_.bind(do_div);
            if (bits == 64) a_.cqo();
            else a_.cdq();
            a_.idiv(bits, dv);
        } else {
            a_.xor_(32, Gp::rdx, Gp::rdx);
            a_.div(bits, dv);
        }
        a_.bind(done);
        a_.mov(bits, dd, Gp::rax);
        return;
    }
    const auto nv = val(d.rn), mv = val(d.rm);
    if (d.op == dp2_rorv || !cpu.bmi2) {
        prepare_clobber();
        take(Gp::rcx);
        load(32, Gp::rcx, mv);
        const auto dd = dst(d.rd);
        const auto t = alloc();
        load(bits, t, nv);
        static constexpr Shift ops[4] = {Shift::shl, Shift::shr, Shift::sar, Shift::ror};
        a_.shift_cl(ops[d.op - dp2_lslv], bits, t);
        a_.mov(bits, dd, t);
        return;
    }
    const auto dd = dst(d.rd);
    if (nv.is_zero()) {
        zero(dd);
        return;
    }
    if (mv.is_zero()) {
        load(bits, dd, nv);
        return;
    }
    const auto nr = in_reg(nv);
    const auto mr = in_reg(mv);
    if (d.op == dp2_lslv) a_.shlx(bits, dd, nr, mr);
    else if (d.op == dp2_lsrv) a_.shrx(bits, dd, nr, mr);
    else a_.sarx(bits, dd, nr, mr);
}

void RegionEmitter::e_dp3(const Inst& d) {
    const unsigned bits = d.sf ? 64 : 32;
    const auto& cpu = cpu_features();
    const auto nv = val(d.rn), mv = val(d.rm);
    if (d.op == dp3_smulh || d.op == dp3_umulh) {
        if (d.op == dp3_umulh && cpu.bmi2 && keep_flags()) {
            take(Gp::rdx);
            load(64, Gp::rdx, nv);
            const auto mr = in_reg(mv);
            const auto hi = alloc();
            a_.mulx(64, hi, Gp::rdx, mr);   // low half (unused) overwrites the rdx source
            const auto dd = dst(d.rd);
            a_.mov(64, dd, hi);
            return;
        }
        prepare_clobber();
        take(Gp::rax);
        take(Gp::rdx);
        const auto dd = dst(d.rd);
        if (nv.is_zero() || mv.is_zero()) {
            a_.mov_imm(dd, 0);
            return;
        }
        load(64, Gp::rax, nv);
        const unsigned ext = d.op == dp3_smulh ? 5 : 4;
        const Gp mh = mv.is_reg() ? mv.r : cached_gpr(mv.g);
        if (mh != Gp::none) a_.unary(ext, 64, mh);
        else a_.unary(ext, 64, gmem(mv.g));
        a_.mov(64, dd, Gp::rdx);
        return;
    }
    const auto av = val(d.ra);
    const bool sub = d.op == dp3_msub || d.op == dp3_smsubl || d.op == dp3_umsubl;
    const bool widen = d.op >= dp3_smaddl;
    const bool sign = d.op == dp3_smaddl || d.op == dp3_smsubl;
    const unsigned mbits = widen ? 64 : bits;
    Gp p = Gp::none;   // product
    if (nv.is_zero() || mv.is_zero()) {
        // a +/- 0
        const auto dd = dst(d.rd);
        load(widen ? 64 : bits, dd, av);
        return;
    }
    const auto widen_operand = [&](const Val& v) {
        const Gp vr = v.is_reg() ? v.r : cached_gpr(v.g);
        const auto t = alloc();
        if (vr != Gp::none) {
            if (sign) a_.movsx(64, t, vr, 32);
            else a_.mov(32, t, vr);
        } else {
            if (sign) a_.movsx(64, t, gmem(v.g), 32);
            else a_.mov(32, t, gmem(v.g));
        }
        return t;
    };
    // SMADDL/UMADDL operands whose producer already sign/zero-extended them
    // are used as they are (no movsxd / mov r32 copy).
    const unsigned want = sign ? ext_sign32 : ext_zero32;
    const bool zext_on = widen && isel_on(isel_no_zext);
    const bool known_n = zext_on && (ext_facts(plan_, index_, d.rn) & want) != 0;
    const bool known_m = zext_on && (ext_facts(plan_, index_, d.rm) & want) != 0;
    if (keep_flags() && cpu.bmi2 && !(widen && sign)) {
        take(Gp::rdx);
        if (widen) {
            load(32, Gp::rdx, nv);
        } else {
            load(mbits, Gp::rdx, nv);
        }
        const auto mr = widen && !known_m ? widen_operand(mv) : in_reg(mv);
        p = alloc();
        a_.mulx(mbits, Gp::rdx, p, mr);   // high half (unused) overwrites the rdx source
    } else {
        prepare_clobber();
        if (widen && (known_n || known_m)) {
            // Widen the other operand into the product register; multiply by
            // the already extended one (register, cached or context slot).
            const bool swap = known_n && !known_m;
            p = widen_operand(swap ? mv : nv);
            const auto q = resolve(swap ? nv : mv);
            const Gp qh = q.is_reg() ? q.r : cached_gpr(q.g);
            if (qh != Gp::none) a_.imul(64, p, qh);
            else a_.imul(64, p, gmem(q.g));
        } else if (widen) {
            p = widen_operand(nv);
            const auto q = widen_operand(mv);
            a_.imul(64, p, q);
        } else {
            // MADD Xd, Xd, Xm, Xa with a pinned Xd: multiply in place.
            const bool in_place = !sub && nv.is_reg() && pinned(d.rd) && d.rd != 31 && nv.r == pin(d.rd) &&
                                  !(av.is_reg() && av.r == nv.r);
            p = in_place ? nv.r : copy_of(nv);
            if (in_place) note_write(d.rd);
            const Gp mh = mv.is_reg() ? mv.r : cached_gpr(mv.g);
            if (mh != Gp::none) a_.imul(bits, p, mh);
            else a_.imul(bits, p, gmem(mv.g));
        }
    }
    const auto dd = dst(d.rd);
    const Val pv{Val::K::reg, p, 0};
    if (av.is_zero() && !sub && isel_on(isel_no_zext)) {   // MUL: the product register is already zero-extended
        if (dd != p) a_.mov(mbits == 64 ? 64 : 32, dd, p);
        return;
    }
    if (sub) sub_value(mbits, dd, av, pv, true);
    else add_value(mbits, dd, av, pv, 0);
}

} // namespace transmute::fx::core
