// Integer data-processing lowering (owner: translator core).
#include "fx/translator/region_emitter.hpp"

#include "fx/common/cpu_features.hpp"

#include <atomic>
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

namespace {
std::atomic<std::uint32_t> g_p2_test_knobs{~0u};
}

void set_p2_knobs_for_testing(std::uint32_t bits) { g_p2_test_knobs.store(bits, std::memory_order_relaxed); }

std::uint32_t codegen_p2_knob_bits() {
    if (const auto t = g_p2_test_knobs.load(std::memory_order_relaxed); t != ~0u) return t;
    static const std::uint32_t bits = [] {
        std::uint32_t b = 0;
        if (const char* v = std::getenv("TRANSMUTE_FX_CORE")) {
            const std::string s(v);
            if (s.find("nobranchdirty") != std::string::npos) b |= knob_no_branch_dirty;
            if (s.find("brdeadflags") != std::string::npos) b |= knob_br_flags_dead;
            if (s.find("nodeadpinloads") != std::string::npos) b |= knob_no_dead_pin_loads;
            if (s.find("noflagsave") != std::string::npos) b |= knob_no_flag_save;
            if (s.find("noccmpfusion") != std::string::npos) b |= knob_no_ccmp_fusion;
            if (s.find("nogprreuse") != std::string::npos) b |= knob_no_gpr_reuse;
            if (s.find("nomergedirty") != std::string::npos) b |= knob_no_merge_dirty;
            if (s.find("noaliastarget") != std::string::npos) b |= knob_no_alias_target;
            if (s.find("norawflags") != std::string::npos) b |= knob_no_raw_flags;
            if (s.find("abiflags") != std::string::npos) b |= knob_abi_flags;
            if (s.find("nodeadtargets") != std::string::npos) b |= knob_no_dead_targets;
            if (s.find("hwrsb") != std::string::npos && s.find("nohwrsb") == std::string::npos) b |= knob_hw_rsb;
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
    if (m.mem == MemOp::prefetch) return false;
    const bool pair = m.kind == Kind::load_store_pair;
    const bool uses = m.rd == a.rd || (pair && m.ra == a.rd);
    if (m.mem == MemOp::store && uses) return false;   // the stored value is the address itself
    const bool self = m.mem != MemOp::store && uses;   // the load rewrites Xd
    // ADD/SUB Xd, Xn, #imm feeding LDR/STR [xBase, Xd] (e.g. V8 FixedArray load):
    // fold into [xBase + Xn + imm] when Xd is overwritten or dead after.
    if (!pair && m.mode == AddrMode::reg && a.kind == Kind::add_sub_imm && m.rm == a.rd && m.rn != a.rd &&
        m.shift == 3 && m.amount == 0 && m.imm == 0 && a.rn != 31) {
        const auto imm = a.op ? -a.imm : a.imm;
        if (imm != static_cast<std::int32_t>(imm)) return false;
        access = m;
        access.rm = a.rn;
        access.imm = imm;
        return self || dead_after(plan, index + 1, a.rd);
    }
    if (m.mode != AddrMode::offset || m.rn != a.rd) return false;
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

unsigned fold_indexed_address(const Plan& plan, std::int32_t index, Inst& access, unsigned& index_mask_bits,
                              bool allow_flag_clobber) {
    index_mask_bits = 0;
    const auto n = static_cast<std::int32_t>(plan.insts.size());
    if (index + 1 >= n) return 0;
    const auto& pa = plan.insts[static_cast<std::size_t>(index)];
    const auto& pb = plan.insts[static_cast<std::size_t>(index) + 1];
    if (pb.leader || pb.pc != pa.pc + 4 || pa.flow != Flow::normal || pb.flow != Flow::normal) return 0;
    const auto& a = pa.inst;
    // UBFM 64-bit: UBFIZ Xd, Xn, #pos, #len (imms < 32, len = imms + 1) or LSL Xd, Xn, #pos (imms == 63 - pos),
    // with pos in 1..3.
    if (a.kind != Kind::bitfield || !a.sf || a.op != 2 || a.rd == 31 || a.rn == 31) return 0;
    if (a.amount < 61 || a.amount > 63) return 0;
    const unsigned pos = 64u - a.amount;
    unsigned option = 3;
    unsigned mask_bits = 0;
    if (a.imms == 31) {
        option = 2;   // UXTW << pos
    } else if (a.imms == 63u - pos) {
        option = 3;   // LSL << pos
    } else if (a.imms < 31) {
        mask_bits = a.imms + 1u;   // 1..31-bit zero-extended mask
        if (mask_bits != 8u && mask_bits != 16u && !allow_flag_clobber) return 0;
    } else {
        return 0;
    }
    const auto try_access = [&](const Inst& m, std::int32_t access_index, std::int64_t imm) -> bool {
        if (m.kind != Kind::load_store || m.mode != AddrMode::reg || m.mem == MemOp::prefetch) return false;
        if (m.rm != a.rd || m.rn == a.rd || m.shift != 3 || m.amount != 0 || m.imm != 0) return false;
        if (m.mem == MemOp::store && m.rd == a.rd) return false;
        const bool self = m.mem != MemOp::store && m.rd == a.rd;
        if (!self && !dead_after(plan, access_index, a.rd)) return false;
        access = m;
        access.rm = a.rn;
        access.shift = static_cast<std::uint8_t>(option);
        access.amount = static_cast<std::uint8_t>(pos);
        access.imm = imm;
        index_mask_bits = mask_bits;
        return true;
    };
    // 3-instruction form: UBFIZ/LSL + ADD/SUB Xd, Xd, #imm + LDR/STR [xBase, Xd].
    const auto& b = pb.inst;
    if (index + 2 < n && b.kind == Kind::add_sub_imm && b.sf && !b.s && b.rd == a.rd && b.rn == a.rd) {
        const auto& pc = plan.insts[static_cast<std::size_t>(index) + 2];
        const auto imm = b.op ? -b.imm : b.imm;
        if (!pc.leader && pc.pc == pb.pc + 4 && pc.flow == Flow::normal && imm == static_cast<std::int32_t>(imm) &&
            try_access(pc.inst, index + 2, imm))
            return 3;
    }
    // 2-instruction form: UBFIZ/LSL + LDR/STR [xBase, Xd].
    if (try_access(b, index + 1, 0)) return 2;
    return 0;
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
                folded_access_ = true;
                unsigned extra = 0;
                if (access.kind == Kind::load_store) extra = e_load_store(access, 0, index_ + 1);
                else e_load_store_pair(access);
                folded_access_ = false;
                return 2u + extra;
            }
            // neon (additive): the same for a SIMD&FP access (emit_load_store.cpp).
            if (const auto consumed = fold_family_access(d); consumed != 0) return consumed;
        }
        // Adjacent non-flag ADD/SUB #imm chain (e.g. SUB SP, SP, #0x2000;
        // SUB SP, SP, #0x3b0 or ADD Xd, SP, #hi; ADD Xd, Xd, #lo): one lea.
        if (d.kind == Kind::add_sub_imm && !d.s && isel_on(isel_no_fold) &&
            index_ + 1 < static_cast<std::int32_t>(plan_.insts.size())) {
            const auto& pb = plan_.insts[static_cast<std::size_t>(index_) + 1];
            const auto& b = pb.inst;
            if (!pb.leader && pb.pc == pi.pc + 4 && pb.flow == Flow::normal && b.kind == Kind::add_sub_imm && !b.s &&
                b.sf == d.sf && b.rn == d.rd && (b.rd == d.rd || (d.rd != 31 && dead_after(plan_, index_ + 1, d.rd)))) {
                const std::int64_t a_imm = d.op ? -d.imm : d.imm;
                const std::int64_t b_imm = b.op ? -b.imm : b.imm;
                const unsigned bits = d.sf ? 64 : 32;
                const std::uint64_t eff = static_cast<std::uint64_t>(a_imm + b_imm) & ones(bits);
                if (fits_imm32(eff, bits)) {
                    const auto disp = static_cast<std::int32_t>(static_cast<std::uint32_t>(eff));
                    const auto n = val(d.rn, true);
                    const auto t = dst(b.rd, true);
                    if (disp == 0) {
                        load(bits, t, n);
                    } else {
                        const auto nv = resolve(n);
                        Gp nr = nv.r;
                        if (!nv.is_reg()) {
                            if (nv.is_mem() && may_cache() && std::popcount(free_gpr_) >= 2 && gpr_read_soon(nv.g)) {
                                nr = read_unpinned(nv.g);
                            } else {
                                load(64, t, nv);
                                nr = t;
                            }
                        }
                        a_.lea(bits, t, ptr(nr, disp));
                    }
                    return 2;
                }
            }
        }
        if (d.kind == Kind::add_sub_imm) e_add_sub_imm(d);
        else if (d.kind == Kind::add_sub_shift) e_add_sub_shift(d);
        else e_add_sub_ext(d);
        return 1;
    case Kind::logic_imm: e_logic_imm(d); return 1;
    case Kind::move_wide: return e_move_wide(pi);
    case Kind::adr: return e_adr(pi);
    case Kind::bitfield:
        if (isel_on(isel_no_fold)) {
            Inst access;
            unsigned mask_bits = 0;
            const bool allow_clobber = !keep_flags() && !flags_saved_ && bool_reader_ < 0;
            if (const auto consumed = fold_indexed_address(plan_, index_, access, mask_bits, allow_clobber); consumed != 0) {
                folded_access_ = true;
                const auto extra = e_load_store(access, mask_bits, index_ + static_cast<std::int32_t>(consumed) - 1);
                folded_access_ = false;
                return consumed + extra;
            }
        }
        e_bitfield(d);
        return 1;
    case Kind::extr: e_extr(d); return 1;
    case Kind::adc: e_adc(d); return 1;
    case Kind::logic_shift: e_logic_shift(d); return 1;
    case Kind::ccmp: e_ccmp(d); return 1;
    case Kind::csel: return e_csel(d);
    case Kind::dp1: return e_dp1(pi);
    case Kind::dp2: e_dp2(d); return 1;
    case Kind::dp3: e_dp3(d); return 1;
    case Kind::load_store: return 1u + e_load_store(d, 0, index_);
    case Kind::load_store_pair: e_load_store_pair(d); return 1;
    case Kind::load_literal: return e_load_literal(pi);
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
    // SBFX Xd, Xn, #lsb, #(32 - lsb) (e.g. V8 SmiUntag SBFX xD, xN, #1, #31):
    // sign-extend low 32 bits then arithmetic-shift right (2 insns instead of 3).
    if (sign && bits == 64 && lsb > 0 && lsb + len == 32 && !keep_flags()) {
        prepare_clobber();
        a_.movsx(64, d, n, 32);
        a_.sar(64, d, static_cast<std::uint8_t>(lsb));
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
        if (lsb == 0) a_.mov(32, d, n);
        else a_.rorx(bits, d, n, static_cast<std::uint8_t>(lsb));   // copy + move the field down
        a_.and_(32, d, static_cast<std::int32_t>(ones(len)));
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
            if (nv.is_mem() && may_cache() && std::popcount(free_gpr_) >= 2 && gpr_read_soon(nv.g)) {
                nr = read_unpinned(nv.g);
            } else {
                load(64, t, nv);
                nr = t;
            }
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
        Gp t = d.rd < 31 && d.rd != d.rn ? handoff_dead_src(d.rd, d.rn) : Gp::none;
        const bool handed = t != Gp::none;
        if (!handed) t = d.rd == 31 ? alloc() : dst(d.rd);
        const Gp nr = handed ? t : (n.is_reg() ? n.r : (n.is_mem() ? cached_gpr(n.g) : Gp::none));
        if (nr != t) load(bits, t, n);
        if (sub) a_.sub(bits, t, imm);
        else a_.add(bits, t, imm);
        if (d.rd != 31 && !flags_saved_) {
            zf_inst_ = index_;
            zf_guest_ = static_cast<std::uint8_t>(d.rd);
            zf_bits_ = static_cast<std::uint8_t>(bits);
        }
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
            Gp t = d.rd != d.rn ? handoff_dead_src(d.rd, d.rn) : Gp::none;
            const bool handed = t != Gp::none;
            if (!handed) t = dst(d.rd);
            const Gp nr = handed ? t : (n.is_reg() ? n.r : (n.is_mem() ? cached_gpr(n.g) : Gp::none));
            if (nr != t) load(bits, t, n);
            alu_imm(Alu::and_, bits, t, imm);
            if (!flags_saved_) {
                zf_inst_ = index_;
                zf_guest_ = static_cast<std::uint8_t>(d.rd);
                zf_bits_ = static_cast<std::uint8_t>(bits);
            }
        }
        set_flags(FlagsProducer::logic);
        return;
    }
    if (n.is_zero()) {
        const auto t = dst(d.rd, true);
        if (d.op == 0) a_.mov_imm(t, 0);
        else mov_literal(t, imm);
        return;
    }
    if (d.op == 0) {
        if (imm == 0xff || imm == 0xffff) {
            const auto t = dst(d.rd, true);
            const unsigned width = imm == 0xff ? 8 : 16;
            const Gp h = n.is_reg() ? n.r : cached_gpr(n.g);
            if (h != Gp::none) a_.movzx(32, t, h, width);
            else a_.movzx(32, t, gmem(n.g), width);
            return;
        }
        if (imm == 0xffffffffULL) {
            load(32, dst(d.rd, true), n);
            return;
        }
    }
    prepare_clobber();
    Gp t = d.rd < 31 && d.rd != d.rn ? handoff_dead_src(d.rd, d.rn) : Gp::none;
    const bool handed = t != Gp::none;
    if (!handed) t = dst(d.rd, true);
    const Gp nr = handed ? t : (n.is_reg() ? n.r : (n.is_mem() ? cached_gpr(n.g) : Gp::none));
    if (nr != t) load(bits, t, n);
    alu_imm(ops[d.op], bits, t, imm);
    if (d.rd != 31 && !flags_saved_) {
        zf_inst_ = index_;
        logic_flags_inst_ = index_;
        zf_guest_ = static_cast<std::uint8_t>(d.rd);
        zf_bits_ = static_cast<std::uint8_t>(bits);
    }
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
    if (d.rd == 31) return consumed;
    // Fuse a dead-temp constant chain (MOVZ/MOVN [+ MOVK...]) directly into
    // an immediately following CMP, non-flag ADD/SUB, or disp8 STR.
    const auto next_idx = index_ + static_cast<std::int32_t>(consumed);
    if (isel_on(isel_no_fold) && consumed <= 3 && next_idx < n) {
        const auto& pb = plan_.insts[static_cast<std::size_t>(next_idx)];
        const auto& c = pb.inst;
        if (!pb.leader && pb.pc == pi.pc + 4ull * consumed && pb.flow == Flow::normal) {
            // MOVZ/MOVN[+MOVK] Xt, #imm; CMP Xn, Xt (Xt dead after) -> cmp Xn, imm32.
            if (c.kind == Kind::add_sub_shift && c.op == 1 && c.s && c.rd == 31 && c.amount == 0 && c.rm == d.rd &&
                c.rn != d.rd && c.rn < 31 && bool_reader_ < 0 && !flags_saved_) {
                const unsigned cbits = c.sf ? 64 : 32;
                const auto kval = value & ones(cbits);
                if (fits_imm32(kval, cbits) && dead_after(plan_, next_idx, d.rd)) {
                    const auto i32 = static_cast<std::int32_t>(static_cast<std::uint32_t>(kval));
                    const auto nv = val(c.rn);
                    const Gp h = nv.is_mem() ? cached_gpr(nv.g) : nv.r;
                    if (h == Gp::none) a_.cmp(cbits, gmem(nv.g), i32);
                    else a_.cmp(cbits, h, i32);
                    set_flags(FlagsProducer::sub);
                    return consumed + 1;
                }
            }
            // MOVZ/MOVN[+MOVK] Xt, #imm; ADD/SUB Xd, Xn, Xt (or ADD Xd, Xt, Xm) -> lea.
            if (c.kind == Kind::add_sub_shift && !c.s && c.amount == 0 &&
                ((c.rm == d.rd && c.rn != d.rd) || (!c.op && c.rn == d.rd && c.rm != d.rd)) &&
                (c.rd == d.rd || dead_after(plan_, next_idx, d.rd))) {
                const unsigned cbits = c.sf ? 64 : 32;
                const auto kval = value & ones(cbits);
                const auto eff = (c.op ? (0ULL - kval) : kval) & ones(cbits);
                if (fits_imm32(eff, cbits)) {
                    const unsigned base_g = c.rm == d.rd ? c.rn : c.rm;
                    const auto base_v = val(base_g);
                    const auto dd = dst(c.rd);
                    if (base_v.is_zero()) {
                        mov_literal(dd, eff);
                    } else if (eff == 0) {
                        load(cbits, dd, base_v);
                    } else {
                        const auto disp = static_cast<std::int32_t>(static_cast<std::uint32_t>(eff));
                        const auto bv = resolve(base_v);
                        Gp br = bv.r;
                        if (!bv.is_reg()) {
                            if (bv.is_mem() && may_cache() && std::popcount(free_gpr_) >= 2 && gpr_read_soon(bv.g)) {
                                br = read_unpinned(bv.g);
                            } else {
                                load(64, dd, bv);
                                br = dd;
                            }
                        }
                        a_.lea(cbits, dd, ptr(br, disp));
                    }
                    return consumed + 1;
                }
            }
            // MOVZ/MOVN[+MOVK] Xt, #imm; AND/ORR/EOR/ANDS/TST Xd, Xn, Xt (Xt dead after) -> imm ALU/TEST.
            if (c.kind == Kind::logic_shift && !c.invert && c.amount == 0 &&
                ((c.rm == d.rd && c.rn != d.rd) || (c.rn == d.rd && c.rm != d.rd)) &&
                (!c.s || (bool_reader_ < 0 && !flags_saved_)) &&
                (c.rd == d.rd || dead_after(plan_, next_idx, d.rd))) {
                const unsigned cbits = c.sf ? 64 : 32;
                const auto kval = value & ones(cbits);
                const unsigned base_g = c.rm == d.rd ? c.rn : c.rm;
                const auto base_v = val(base_g);
                if (!base_v.is_zero() && fits_imm32(kval, cbits)) {
                    static constexpr Alu lops[4] = {Alu::and_, Alu::or_, Alu::xor_, Alu::and_};
                    const auto i32 = static_cast<std::int32_t>(static_cast<std::uint32_t>(kval));
                    if (c.s && c.rd == 31) {
                        if (base_v.is_mem() && i32 != 1 && cached_gpr(base_v.g) == Gp::none)
                            a_.test(cbits, gmem(base_v.g), i32);
                        else
                            a_.test(cbits, in_reg(base_v), i32);
                        set_flags(FlagsProducer::logic);
                        return consumed + 1;
                    }
                    if (!c.s && c.rd == 31) return consumed + 1;
                    if (!c.s && c.op == 0) {
                        if (kval == 0xff || kval == 0xffff) {
                            const auto dd = dst(c.rd);
                            const unsigned width = kval == 0xff ? 8 : 16;
                            const Gp h = base_v.is_reg() ? base_v.r : cached_gpr(base_v.g);
                            if (h != Gp::none) a_.movzx(32, dd, h, width);
                            else a_.movzx(32, dd, gmem(base_v.g), width);
                            return consumed + 1;
                        }
                        if (kval == 0xffffffffULL) {
                            load(32, dst(c.rd), base_v);
                            return consumed + 1;
                        }
                    }
                    if (!c.s) prepare_clobber();
                    const auto dd = dst(c.rd);
                    const Gp br = base_v.is_reg() ? base_v.r : (base_v.is_mem() ? cached_gpr(base_v.g) : Gp::none);
                    if (br != dd) load(cbits, dd, base_v);
                    a_.alu(lops[c.op], cbits, dd, i32);
                    if (c.rd != 31 && !flags_saved_) {
                        zf_inst_ = next_idx;
                        logic_flags_inst_ = next_idx;
                        zf_guest_ = static_cast<std::uint8_t>(c.rd);
                        zf_bits_ = static_cast<std::uint8_t>(cbits);
                    }
                    if (c.s) set_flags(FlagsProducer::logic);
                    return consumed + 1;
                }
            }
            // MOVZ/MOVN[+MOVK] Xt, #imm; MUL Xd, Xn, Xt (Xt dead after) -> imul Xd, Xn, imm32.
            if (c.kind == Kind::dp3 && c.op == dp3_madd && c.ra == 31 && !keep_flags() &&
                ((c.rm == d.rd && c.rn != d.rd) || (c.rn == d.rd && c.rm != d.rd)) &&
                (c.rd == d.rd || dead_after(plan_, next_idx, d.rd))) {
                const unsigned cbits = c.sf ? 64 : 32;
                const auto kval = value & ones(cbits);
                const unsigned base_g = c.rm == d.rd ? c.rn : c.rm;
                const auto base_v = val(base_g);
                if (fits_imm32(kval, cbits)) {
                    if (c.rd == 31) return consumed + 1;
                    if (base_v.is_zero() || kval == 0) {
                        a_.mov_imm(dst(c.rd), 0);
                        return consumed + 1;
                    }
                    prepare_clobber();
                    const auto i32 = static_cast<std::int32_t>(static_cast<std::uint32_t>(kval));
                    const Gp br = base_v.is_reg() ? base_v.r : (base_v.is_mem() ? borrow_gpr(base_v.g) : Gp::none);
                    const auto dd = dst(c.rd);
                    if (br != Gp::none) {
                        a_.imul(cbits, dd, br, i32);
                    } else {
                        load(cbits, dd, base_v);
                        a_.imul(cbits, dd, dd, i32);
                    }
                    return consumed + 1;
                }
            }
            // MOVZ/MOVN[+MOVK] Xt, #imm; STR Xt, [base, #disp8] (Xt dead after) -> mov [m], imm.
            // Relocation hygiene: disp8(m) and (sbits < 32 || i32 != 1) avoid module-window byte patterns.
            // Fault sites report the MOVZ/MOVN pc with Xt unchanged so re-execution is exact.
            if (c.kind == Kind::load_store && c.mem == MemOp::store &&
                (c.mode == AddrMode::offset || c.mode == AddrMode::reg) && c.rd == d.rd && c.rn != d.rd &&
                (c.mode == AddrMode::offset || c.rm != d.rd) && c.imm >= -128 && c.imm <= 127 && c.imm != 1) {
                const unsigned sbits = 8u << c.size;
                const auto sval = value & ones(sbits);
                const auto i32 = static_cast<std::int32_t>(static_cast<std::uint32_t>(sval));
                if (fits_imm32(sval, sbits) && (sbits < 32 || i32 != 1) && dead_after(plan_, next_idx, d.rd)) {
                    folded_access_ = true;
                    EmitContext::GuestAddress ga;
                    ga.base = c.rn;
                    ga.disp = static_cast<std::int32_t>(c.imm);
                    if (c.mode == AddrMode::reg) {
                        ga.index = c.rm;
                        ga.option = c.shift;
                        ga.shift = c.amount;
                    }
                    const Mem m = address(ga);
                    mark_may_fault();
                    a_.mov(sbits, m, i32);
                    fault_close();
                    folded_access_ = false;
                    return consumed + 1;
                }
            }
        }
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
        const bool skip_mask =
            bits == 64 && inv == 0xffffffffULL && isel_on(isel_no_zext) && (ext_facts(plan_, index_, d.rd) & ext_zero32);
        if (!skip_mask) alu_imm(Alu::and_, bits, dd, inv);
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
    const bool had_keep = keep_flags();
    if (s >= r) field_extract(bits, dd, n, r, s - r + 1, sign);
    else field_insert_zero(bits, dd, n, bits - r, s + 1, sign);
    if (d.rd != 31 && !had_keep && !flags_saved_) {
        bool sets_zf = false;
        if (s >= r) {
            const unsigned len = s - r + 1;
            if (r > 0 && (r + len == bits || (sign && bits == 64 && r + len == 32))) sets_zf = true;
        } else {
            const unsigned pos = bits - r, len = s + 1;
            if (pos > 0 && pos + len >= bits && !(dd != n && pos <= 3 && isel_on(isel_no_srcreg))) sets_zf = true;
        }
        if (sets_zf) {
            if (is_host(flags_)) flags_ = FlagsState::ctx;   // dead host flags were clobbered by the shift
            zf_inst_ = index_;
            zf_guest_ = static_cast<std::uint8_t>(d.rd);
            zf_bits_ = static_cast<std::uint8_t>(bits);
        }
    }
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
    // Known constant operand (unshifted): fold to lea / cmp / add / sub imm32.
    if (d.amount == 0 && isel_on(isel_no_fold)) {
        std::uint64_t cval = 0;
        unsigned base_g = d.rn;
        bool have_const = known_constant(plan_, index_, d.rm, cval);
        if (!have_const && !sub && known_constant(plan_, index_, d.rn, cval)) {
            have_const = true;
            base_g = d.rm;
        }
        if (have_const) {
            const auto base_v = val(base_g);
            const auto kval = cval & ones(bits);
            if (!d.s) {
                const auto eff = (sub ? (0ULL - kval) : kval) & ones(bits);
                if (fits_imm32(eff, bits)) {
                    const auto dd = dst(d.rd);
                    if (base_v.is_zero()) {
                        mov_literal(dd, eff);
                    } else if (eff == 0) {
                        load(bits, dd, base_v);
                    } else {
                        const auto disp = static_cast<std::int32_t>(static_cast<std::uint32_t>(eff));
                        const auto bv = resolve(base_v);
                        Gp br = bv.r;
                        if (!bv.is_reg()) {
                            if (bv.is_mem() && may_cache() && std::popcount(free_gpr_) >= 2 && gpr_read_soon(bv.g)) {
                                br = read_unpinned(bv.g);
                            } else {
                                load(64, dd, bv);
                                br = dd;
                            }
                        }
                        a_.lea(bits, dd, ptr(br, disp));
                    }
                    return;
                }
            } else if (!base_v.is_zero() && fits_imm32(kval, bits)) {
                const auto i32 = static_cast<std::int32_t>(static_cast<std::uint32_t>(kval));
                const bool cmn_as_cmp = d.rd == 31 && !sub && i32 != 0 && i32 != INT32_MIN && isel_on(isel_no_srcreg);
                if (d.rd == 31 && (sub || cmn_as_cmp)) {
                    const auto k = sub ? i32 : -i32;
                    const Gp h = base_v.is_mem() ? cached_gpr(base_v.g) : base_v.r;
                    if (h == Gp::none) a_.cmp(bits, gmem(base_v.g), k);
                    else a_.cmp(bits, h, k);
                    set_flags(FlagsProducer::sub);
                    return;
                }
                if (d.rd != 31) {
                    const auto t = dst(d.rd);
                    const Gp br = base_v.is_reg() ? base_v.r : (base_v.is_mem() ? cached_gpr(base_v.g) : Gp::none);
                    if (br != t) load(bits, t, base_v);
                    if (sub) a_.sub(bits, t, i32);
                    else a_.add(bits, t, i32);
                    if (!flags_saved_) {
                        zf_inst_ = index_;
                        zf_guest_ = static_cast<std::uint8_t>(d.rd);
                        zf_bits_ = static_cast<std::uint8_t>(bits);
                    }
                    set_flags(sub ? FlagsProducer::sub : FlagsProducer::add);
                    return;
                }
            }
        }
    }
    if (!d.s) {
        if (!sub && d.shift == 0 && d.amount <= 3) {
            add_value(bits, dst(d.rd), n, val(d.rm), d.amount);
            return;
        }
        if (sub && d.amount == 0 && d.rd < 31 && d.rn != d.rm && d.rd != d.rn && !n.is_zero()) {
            prepare_clobber();
            if (const auto dd = handoff_dead_src(d.rd, d.rn); dd != Gp::none) {
                alu(Alu::sub, bits, dd, resolve(val(d.rm)));
                if (!flags_saved_) {
                    zf_inst_ = index_;
                    zf_guest_ = static_cast<std::uint8_t>(d.rd);
                    zf_bits_ = static_cast<std::uint8_t>(bits);
                }
                return;
            }
        }
        const auto dd = dst(d.rd);
        const auto m = shifted(d.rm, d.shift, d.amount, bits);
        if (sub) sub_value(bits, dd, n, m);
        else add_value(bits, dd, n, m, 0);
        return;
    }
    if (d.amount == 0 && d.rd < 31 && d.rn != d.rm && !n.is_zero()) {
        const auto mv = val(d.rm);
        if (!mv.is_zero()) {
            Gp dd = Gp::none;
            const Val* other = nullptr;
            if (d.rd != d.rn && (dd = handoff_dead_src(d.rd, d.rn)) != Gp::none) {
                other = &mv;
            } else if (!sub && d.rd != d.rm && (dd = handoff_dead_src(d.rd, d.rm)) != Gp::none) {
                other = &n;
            }
            if (dd != Gp::none) {
                alu(sub ? Alu::sub : Alu::add, bits, dd, resolve(*other));
                set_flags(sub ? FlagsProducer::sub : FlagsProducer::add);
                if (!flags_saved_) {
                    zf_inst_ = index_;
                    zf_guest_ = static_cast<std::uint8_t>(d.rd);
                    zf_bits_ = static_cast<std::uint8_t>(bits);
                }
                return;
            }
        }
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
    if (d.rd != 31 && !flags_saved_) {
        zf_inst_ = index_;
        zf_guest_ = static_cast<std::uint8_t>(d.rd);
        zf_bits_ = static_cast<std::uint8_t>(bits);
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
    if (d.rd != 31 && !flags_saved_) {
        zf_inst_ = index_;
        zf_guest_ = static_cast<std::uint8_t>(d.rd);
        zf_bits_ = static_cast<std::uint8_t>(bits);
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
    }
    static constexpr Alu ops[4] = {Alu::and_, Alu::or_, Alu::xor_, Alu::and_};
    // Known constant operand (unshifted, non-inverted): fold to immediate ALU/TEST.
    if (!d.invert && d.amount == 0 && isel_on(isel_no_fold)) {
        std::uint64_t cval = 0;
        unsigned base_g = d.rn;
        bool have_const = known_constant(plan_, index_, d.rm, cval);
        if (!have_const && known_constant(plan_, index_, d.rn, cval)) {
            have_const = true;
            base_g = d.rm;
        }
        if (have_const) {
            const auto base_v = val(base_g);
            const auto kval = cval & ones(bits);
            if (!base_v.is_zero() && fits_imm32(kval, bits)) {
                const auto i32 = static_cast<std::int32_t>(static_cast<std::uint32_t>(kval));
                if (d.s && d.rd == 31) {
                    if (base_v.is_mem() && i32 != 1 && cached_gpr(base_v.g) == Gp::none) a_.test(bits, gmem(base_v.g), i32);
                    else a_.test(bits, in_reg(base_v), i32);
                    set_flags(FlagsProducer::logic);
                    return;
                }
                if (!d.s && d.rd == 31) return;
                if (!d.s && d.op == 0) {
                    if (kval == 0xff || kval == 0xffff) {
                        const auto dd = dst(d.rd);
                        const unsigned width = kval == 0xff ? 8 : 16;
                        const Gp h = base_v.is_reg() ? base_v.r : cached_gpr(base_v.g);
                        if (h != Gp::none) a_.movzx(32, dd, h, width);
                        else a_.movzx(32, dd, gmem(base_v.g), width);
                        return;
                    }
                    if (kval == 0xffffffffULL) {
                        load(32, dst(d.rd), base_v);
                        return;
                    }
                }
                if (!d.s) prepare_clobber();
                const auto dd = dst(d.rd);
                const Gp br = base_v.is_reg() ? base_v.r : (base_v.is_mem() ? cached_gpr(base_v.g) : Gp::none);
                if (br != dd) load(bits, dd, base_v);
                a_.alu(ops[d.op], bits, dd, i32);
                if (d.rd != 31 && !flags_saved_) {
                    zf_inst_ = index_;
                    logic_flags_inst_ = index_;
                    zf_guest_ = static_cast<std::uint8_t>(d.rd);
                    zf_bits_ = static_cast<std::uint8_t>(bits);
                }
                if (d.s) set_flags(FlagsProducer::logic);
                return;
            }
        }
    }
    if (!d.s) prepare_clobber();
    // Dead-source host handoff for unshifted non-inverted AND/ORR/EOR/ANDS:
    // avoids `mov dst, rn; or dst, rm` when an unpinned cached source is dead
    // (notably V8 compressed-pointer `LDR wTmp, [obj]; ORR xDst, x28, xTmp`).
    if (!d.invert && d.amount == 0 && d.rd < 31 && d.rn != d.rm && d.rd != d.rn && d.rd != d.rm &&
        !n.is_zero() && !mraw.is_zero()) {
        Gp dd = Gp::none;
        const Val* other = nullptr;
        if ((dd = handoff_dead_src(d.rd, d.rn)) != Gp::none) other = &mraw;
        else if ((dd = handoff_dead_src(d.rd, d.rm)) != Gp::none) other = &n;
        if (dd != Gp::none) {
            alu(ops[d.op], bits, dd, resolve(*other));
            if (!flags_saved_) {
                zf_inst_ = index_;
                logic_flags_inst_ = index_;
                zf_guest_ = static_cast<std::uint8_t>(d.rd);
                zf_bits_ = static_cast<std::uint8_t>(bits);
            }
            if (d.s) set_flags(FlagsProducer::logic);
            return;
        }
    }
    // Commutative shifted AND/ORR/EOR/ANDS: shift rm directly into dd when dd
    // will not alias rn, saving a scratch temp and a mov (e.g. V8 LoadIC hash).
    if (!d.invert && d.amount != 0 && !mraw.is_zero() && !(d.s && d.rd == 31) &&
        !(n.is_reg() && d.rd == d.rn) &&
        !(n.is_mem() && d.rd == d.rn && reusable_gpr(d.rd) != Gp::none)) {
        const auto dd = dst(d.rd);
        const auto mv = resolve(mraw);
        Gp src = mv.r;
        if (!mv.is_reg()) {
            load(64, dd, mv);
            src = dd;
        }
        shift_imm(shift_op(d.shift), bits, dd, src, d.amount);
        alu(ops[d.op], bits, dd, n);
        if (d.rd != 31 && !flags_saved_) {
            zf_inst_ = index_;
            logic_flags_inst_ = index_;
            zf_guest_ = static_cast<std::uint8_t>(d.rd);
            zf_bits_ = static_cast<std::uint8_t>(bits);
        }
        if (d.s) set_flags(FlagsProducer::logic);
        return;
    }
    auto ms = shifted(d.rm, d.shift, d.amount, bits);
    if (d.invert) {
        if ((d.op == 0 || d.op == 3) && cpu_features().bmi1) {   // BIC / BICS: andn
            const auto mr = in_reg(ms);
            const auto nr = in_reg(n);
            const auto dd = d.rd == 31 ? alloc() : dst(d.rd);
            a_.andn(bits, dd, mr, nr);
            if (d.rd != 31 && !flags_saved_) {
                zf_inst_ = index_;
                logic_flags_inst_ = index_;
                zf_guest_ = static_cast<std::uint8_t>(d.rd);
                zf_bits_ = static_cast<std::uint8_t>(bits);
            }
            if (d.s) set_flags(FlagsProducer::logic);
            return;
        }
        const auto t = alloc();
        load(bits, t, ms);
        a_.not_(bits, t);
        ms = Val{Val::K::reg, t, 0};
    }
    if (d.s && d.rd == 31) {   // TST
        // Redundant TST Xd, Xd right after an x64 AND/OR/XOR/ANDN writing Xd
        // at the same width: host EFLAGS already hold exact SF/ZF/CF=0/OF=0.
        if (!d.invert && d.op == 3 && d.amount == 0 && d.rn == d.rm && d.rn < 31 &&
            logic_flags_inst_ == index_ - 1 && !cur_->leader && zf_guest_ == d.rn && zf_bits_ == bits &&
            !flags_saved_ && bool_reader_ < 0) {
            set_flags(FlagsProducer::logic);
            return;
        }
        const auto mr = in_reg(ms);
        const auto nr = in_reg(n);
        a_.test(bits, nr, mr);
        set_flags(FlagsProducer::logic);
        return;
    }
    binop(ops[d.op], bits, dst(d.rd), n, ms, true);
    if (d.rd != 31 && !flags_saved_) {
        zf_inst_ = index_;
        logic_flags_inst_ = index_;
        zf_guest_ = static_cast<std::uint8_t>(d.rd);
        zf_bits_ = static_cast<std::uint8_t>(bits);
    }
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
    // The next instruction is the only reader of the result (B.cond/CSEL
    // family, NZCV dead after it): no branch. With c1 the condition, c2 the
    // reader's and K = c2 on the immediate NZCV (known here), the reader is
    // taken iff c1 ? c2(compare) : K, i.e. (c1 & c2) or (!c1 | c2): two
    // setcc and one test/or, read as NE (bool_reader_).
    if (const auto reader = ccmp_bool_reader(); reader >= 0) {
        const unsigned c2 = plan_.insts[static_cast<std::size_t>(reader)].inst.cond;
        const bool k = condition_holds(c2, static_cast<std::uint32_t>(d.imms) << 28);
        const auto t1 = alloc();
        const auto c = condition(d.cond);
        a_.setcc(k ? x64::invert(c) : c, t1);
        do_compare();
        set_flags(producer);
        const auto t2 = alloc();
        a_.setcc(condition(c2), t2);
        if (k) a_.alu(Alu::or_, 8, t1, t2);
        else a_.test(8, t1, t2);
        set_flags(FlagsProducer::logic);
        bool_reader_ = reader;
        return;
    }
    const auto c = condition(d.cond);
    const auto lfalse = a_.new_label(), ldone = a_.new_label();
    const unsigned nz = d.imms;
    const bool fn = (nz & 8) != 0, fz = (nz & 4) != 0, fc = (nz & 2) != 0, fv = (nz & 1) != 0;
    const bool cf = ccmn ? fc : !fc;
    const unsigned ah = (fn ? 0x80u : 0u) | (fz ? 0x40u : 0u) | (cf ? 1u : 0u);
    const unsigned imm = (ah << 8) | (fv ? 1u : 0u);
    // When CCMP feeds an adjacent B.cond:
    //  - If the immediate-NZCV outcome forces B.cond taken (k == true), defer
    //    the immediate-NZCV sahf block out-of-line jumping straight to B.cond's
    //    taken destination (via cond_branch): zero hot jmp, zero hot sahf bytes,
    //    strictly better or equal on every path (0/1/2 jumps vs 1/2/2 inline).
    //  - If k == false and the fallthrough exits the region, defer back to ldone.
    bool defer_taken = false;
    bool defer_ldone = false;
    if (can_defer_cold() && index_ + 1 < static_cast<std::int32_t>(plan_.insts.size())) {
        const auto& pb = plan_.insts[static_cast<std::size_t>(index_) + 1];
        if (pb.inst.kind == Kind::branch_cond && !pb.leader && pb.pc == cur_->pc + 4 && pb.inst.cond < 14) {
            const bool k = condition_holds(pb.inst.cond, static_cast<std::uint32_t>(d.imms) << 28);
            if (k) {
                // lfalse jumps straight to B.cond's taken destination, bypassing
                // any code between CCMP and B.cond's jcc. Allow only when
                // B.cond will emit neither alias resolves, nor dirty-cache
                // flushes, nor inline edge_cache(taken, true) before its jcc.
                const bool no_pre_flush =
                    alias_count_ == 0 && galias_count_ == 0 && slot_alias_count_ == 0 &&
                    ((gc_.dirty | vc_.dirty) == 0 || dirty_crosses_branch(index_ + 1));
                const bool no_inline_edge =
                    pb.taken < 0 ||
                    (pb.taken > index_ + 1 && plan_.insts[static_cast<std::size_t>(pb.taken)].leader &&
                     lent_at(pb.taken) == lent_ &&
                     (is_pending_target(pb.taken) || s_.cache_slot[static_cast<std::size_t>(pb.taken)] >= 0));
                if (no_pre_flush && no_inline_edge) defer_taken = true;
                else defer_ldone = true;
            } else if (pb.fall < 0) {
                defer_ldone = true;
            }
        }
    }
    if (defer_taken) {
        a_.jcc(x64::invert(c), lfalse);
        do_compare();
        ccmp_cold_inst_ = index_ + 1;
        ccmp_cold_label_ = lfalse;
        ccmp_cold_nzcv_ = nz;
    } else if (defer_ldone) {
        a_.jcc(x64::invert(c), lfalse);
        do_compare();
        a_.bind(ldone);
        defer_cold([this, lfalse, ldone, imm]() {
            a_.bind(lfalse);
            a_.mov_imm(Gp::rax, imm);
            a_.alu(Alu::add, 8, Gp::rax, 0x7f);   // OF = (al == 1)
            a_.sahf();                            // SF ZF CF from ah
            a_.jmp(ldone);
        });
    } else {
        a_.jcc_short(x64::invert(c), lfalse);
        do_compare();
        a_.jmp_short(ldone);
        a_.bind(lfalse);
        a_.mov_imm(Gp::rax, imm);
        a_.alu(Alu::add, 8, Gp::rax, 0x7f);   // OF = (al == 1)
        a_.sahf();                            // SF ZF CF from ah
        a_.bind(ldone);
    }
    set_flags(producer);
}

// CCMP fusion (e_ccmp): index of its flags reader when that is a B.cond or a
// CSEL-family instruction in the same block after which NZCV is dead, with
// at most three instructions in between that neither touch the flags nor
// may fault (they keep EFLAGS or park the answer: RegionEmitter::park_bool);
// else -1. Single-flag FCMP conditions (e.g. LE/GT/GE/LT/VS/VC) fuse directly
// via their native x64 condition without needing parity checks.
std::int32_t RegionEmitter::ccmp_bool_reader() const {
    if (!codegen_on(knob_no_ccmp_fusion)) return -1;
    if (is_fcmp(flags_) && (cur_->inst.cond >= 14 || fcmp_cond(cur_->inst.cond).k != FcmpCond::K::single)) return -1;
    const auto n = static_cast<std::int32_t>(plan_.insts.size());
    for (std::int32_t j = index_ + 1; j < n && j <= index_ + 4; ++j) {
        const auto& pj = plan_.insts[static_cast<std::size_t>(j)];
        if (pj.leader || pj.pc != plan_.insts[static_cast<std::size_t>(j) - 1].pc + 4 || pj.family >= 0 ||
            pj.try_family || pj.inst.kind == Kind::unknown || pj.conservative_writes || pj.inline_call || pj.inline_ret)
            return -1;
        if (pj.reads_flags) {
            if (pj.flags_live_out || pj.writes_flags) return -1;
            if (pj.inst.kind != Kind::branch_cond && pj.inst.kind != Kind::csel) return -1;
            return pj.inst.cond < 14 ? j : -1;
        }
        if (pj.writes_flags || pj.may_fault || pj.flow != Flow::normal) return -1;
    }
    return -1;
}

// Branchless dead-CSET byte-OR/AND fusion:
//   CSET w1, c1 ; CMP a, b ; CSET w2, c2 ; ORR/AND w3, w1, w2 ; CBZ/CBNZ/TBZ#0/TBNZ#0 w3
// When w1, w2, w3 are unpinned and observably dead after the branch and NZCV is
// dead after CSET2, skips both 32-bit zero-extensions (`mov r32, 0`), evaluates
// `setcc t1b; cmp; setcc t2b; or/test t1b, t2b` in two scratch bytes, and feeds
// the adjacent branch via cset_inst_ (consumed = 4).
unsigned RegionEmitter::try_fuse_cset_bool(const Inst& d) {
    if (flags_saved_ || bool_reader_ >= 0 || d.rd >= 31 || pinned(d.rd)) return 0;
    const auto n = static_cast<std::int32_t>(plan_.insts.size());
    if (index_ + 4 >= n) return 0;
    for (std::int32_t k = 1; k <= 4; ++k) {
        const auto& pk = plan_.insts[static_cast<std::size_t>(index_ + k)];
        if (pk.leader || pk.pc != cur_->pc + 4u * static_cast<unsigned>(k) || pk.may_fault || pk.conservative_writes ||
            pk.family >= 0 || pk.try_family || pk.inline_call || pk.inline_ret)
            return 0;
        if (k <= 3 && pk.flow != Flow::normal) return 0;
    }
    const auto& p1 = plan_.insts[static_cast<std::size_t>(index_ + 1)];
    const auto& p2 = plan_.insts[static_cast<std::size_t>(index_ + 2)];
    const auto& p3 = plan_.insts[static_cast<std::size_t>(index_ + 3)];
    const auto& p4 = plan_.insts[static_cast<std::size_t>(index_ + 4)];
    const auto& c1 = p1.inst;
    const bool cmp_imm = c1.kind == Kind::add_sub_imm && c1.op == 1 && c1.s && c1.rd == 31;
    const bool cmp_reg = c1.kind == Kind::add_sub_shift && c1.op == 1 && c1.s && c1.rd == 31 && c1.amount == 0;
    if (!cmp_imm && !cmp_reg) return 0;
    const unsigned w1 = d.rd;
    if ((p1.reads >> w1) & 1u) return 0;
    const auto& c2 = p2.inst;
    if (c2.kind != Kind::csel || c2.op != 1 || c2.rn != 31 || c2.rm != 31 || c2.cond >= 14 || c2.rd >= 31 ||
        c2.rd == w1 || pinned(c2.rd) || p2.flags_live_out)
        return 0;
    const unsigned w2 = c2.rd;
    const auto& c3 = p3.inst;
    if (c3.kind != Kind::logic_shift || (c3.op != 0 && c3.op != 1) || c3.invert || c3.amount != 0 || c3.s ||
        c3.rd >= 31 || pinned(c3.rd))
        return 0;
    if (!((c3.rn == w1 && c3.rm == w2) || (c3.rn == w2 && c3.rm == w1))) return 0;
    const unsigned w3 = c3.rd;
    const auto& c4 = p4.inst;
    const bool br_ok = (c4.kind == Kind::compare_branch && c4.rd == w3) ||
                       (c4.kind == Kind::test_branch && c4.rd == w3 && c4.amount == 0);
    if (!br_ok || p4.flags_live_in) return 0;
    if (!gpr_dead_after(index_ + 4, w1) || !gpr_dead_after(index_ + 4, w2) || !gpr_dead_after(index_ + 4, w3))
        return 0;

    const auto t1 = alloc();
    a_.setcc(x64::invert(condition(d.cond)), t1);
    const unsigned cbits = c1.sf ? 64 : 32;
    if (cmp_imm) {
        const auto cn = val(c1.rn, true);
        if (cn.is_reg()) a_.cmp(cbits, cn.r, static_cast<std::int32_t>(c1.imm));
        else if (const auto h = cached_gpr(cn.g); h != Gp::none) a_.cmp(cbits, h, static_cast<std::int32_t>(c1.imm));
        else a_.cmp(cbits, gmem(cn.g), static_cast<std::int32_t>(c1.imm));
    } else {
        compare(cbits, val(c1.rn), val(c1.rm));
    }
    set_flags(FlagsProducer::sub);
    const auto t2 = alloc();
    a_.setcc(x64::invert(condition(c2.cond)), t2);
    if (c3.op == 1) a_.alu(Alu::or_, 8, t1, t2);
    else a_.test(8, t1, t2);
    cache_drop_guest(gc_, w1);
    cache_drop_guest(gc_, w2);
    cache_drop_guest(gc_, w3);
    flags_ = FlagsState::ctx;
    cset_inst_ = index_ + 3;
    cset_guest_ = static_cast<std::uint8_t>(w3);
    cset_cond_ = Cond::ne;
    return 4;
}

unsigned RegionEmitter::e_csel(const Inst& d) {
    const unsigned bits = d.sf ? 64 : 32;
    const auto n = val(d.rn), m = val(d.rm);
    // Fused FCMP flags whose condition needs the parity flag as well: two cmovs.
    const bool pair = d.cond < 14 && is_fcmp(flags_) && fcmp_cond(d.cond).k != FcmpCond::K::single;
    if (d.cond < 14 && d.op == 1 && n.is_zero() && m.is_zero() && !pair) {
        if (const unsigned consumed = try_fuse_cset_bool(d); consumed != 0) return consumed;
    }
    const auto dd = dst(d.rd);
    if (d.cond >= 14) {
        load(bits, dd, n);
        return 1;
    }
    if (d.op == 1 && n.is_zero() && m.is_zero() && !pair) {   // CSET
        a_.mov_imm(dd, 0);
        const auto c = condition(d.cond);
        a_.setcc(x64::invert(c), dd);
        if (d.rd < 31 && !flags_saved_) {
            cset_inst_ = index_;
            cset_guest_ = static_cast<std::uint8_t>(d.rd);
            cset_cond_ = x64::invert(c);
        }
        return 1;
    }
    if (d.op == 2 && n.is_zero() && m.is_zero() && !pair) {   // CSETM
        const auto t = alloc();
        a_.mov_imm(dd, 0);
        mov_literal(t, ones(bits));
        const auto c = condition(d.cond);
        a_.cmov(x64::invert(c), bits, dd, t);
        if (d.rd < 31 && !flags_saved_) {
            cset_inst_ = index_;
            cset_guest_ = static_cast<std::uint8_t>(d.rd);
            cset_cond_ = x64::invert(c);
        }
        return 1;
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
            return 1;
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
        return 1;
    }
    if (dd_is_n) {
        const auto c = condition(d.cond);
        a_.cmov(x64::invert(c), bits, dd, x);
        return 1;
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
    return 1;
}

// RBIT Xd, Xn feeding CLZ Xm, Xd a few instructions later (the ARM64
// count-trailing-zeros idiom):
//  - When Xd is overwritten before any other read and nothing in between may
//    fault or leave the block (`elide_rbit = true`), RBIT emits nothing and
//    CLZ emits TZCNT Xm, Xn.
//  - Otherwise (`elide_rbit = false`), when Xd != Xn and neither is written
//    before the CLZ, RBIT still emits for exact architectural state (e.g. a
//    following faulting load), while CLZ emits TZCNT Xm, Xn directly on Xn to
//    break the RBIT latency chain.
std::int32_t RegionEmitter::rbit_clz_target(const Inst& d, bool& elide_rbit) const {
    elide_rbit = false;
    if (!plan_.rbit_fusion || !cpu_features().bmi1 || d.rd == 31) return -1;
    const auto n = static_cast<std::int32_t>(plan_.insts.size());
    const std::uint32_t xd = 1u << d.rd, xn = d.rn == 31 ? 0u : 1u << d.rn;
    std::int32_t clz = -1;
    bool can_elide = true;
    for (std::int32_t j = index_ + 1; j < n && j <= index_ + 16; ++j) {
        const auto& pj = plan_.insts[j];
        if (pj.leader || pj.flow != Flow::normal || pj.conservative_writes) break;
        if (pj.may_fault) {
            if (clz < 0) return -1;
            can_elide = false;
            break;
        }
        if (clz < 0) {
            if (pj.reads & xd) {
                const auto& c = pj.inst;
                if (c.kind != Kind::dp1 || c.op != dp1_clz || c.sf != d.sf || c.rn != d.rd) return -1;
                clz = j;
                if (c.rd == d.rd) {
                    elide_rbit = true;
                    return clz;   // the CLZ overwrites Xd itself
                }
                continue;
            }
            if (pj.writes & (xd | xn)) return -1;
        } else {
            if (pj.reads & xd) {
                can_elide = false;
                break;
            }
            if (pj.writes & xd) {
                elide_rbit = true;
                return clz;     // Xd overwritten: the RBIT value is dead
            }
        }
    }
    if (clz >= 0 && d.rd != d.rn) {
        (void)can_elide;
        elide_rbit = false;
        return clz;
    }
    return -1;
}

unsigned RegionEmitter::e_dp1(const PlanInst& pi) {
    const auto& d = pi.inst;
    const unsigned bits = d.sf ? 64 : 32;
    unsigned consumed = 1;
    Gp in_place_nr = Gp::none;
    if (d.op == dp1_rbit) {
        // One fusion at a time: a second RBIT before the first one's CLZ (two
        // ctz idioms interleaved) is emitted plainly, or it would take over
        // the pending CLZ and leave the first RBIT's register unwritten.
        bool elide_rbit = false;
        if (const auto j = tz_clz_ < 0 ? rbit_clz_target(d, elide_rbit) : -1; j >= 0) {
            tz_clz_ = j;   // emitted by the CLZ as TZCNT of Xn
            tz_src_ = static_cast<std::uint8_t>(d.rn);
            if (elide_rbit) return 1;
        } else if (plan_.rbit_fusion && cpu_features().bmi1 && tz_clz_ < 0 && d.rd != 31 && d.rd == d.rn &&
                   index_ + 1 < static_cast<std::int32_t>(plan_.insts.size())) {
            // In-place RBIT Xd, Xd immediately followed by CLZ Xm, Xd (Xm != Xd)
            // where RBIT could not be elided (e.g. a faulting load/store follows):
            // emit TZCNT Xm, Xd BEFORE RBIT overwrites Xd to break the dependency chain.
            const auto& next = plan_.insts[static_cast<std::size_t>(index_ + 1)];
            if (!next.leader && next.pc == pi.pc + 4 && next.inst.kind == Kind::dp1 && next.inst.op == dp1_clz &&
                next.inst.sf == d.sf && next.inst.rn == d.rd && next.inst.rd != d.rd && next.inst.rd != 31 &&
                !next.may_fault && next.flow == Flow::normal) {
                prepare_clobber();
                in_place_nr = in_reg(val(d.rn));
                const auto clz_dst = dst(next.inst.rd);
                a_.tzcnt(bits, clz_dst, in_place_nr);
                consumed = 2;
            }
        }
    } else if (d.op == dp1_clz && index_ == tz_clz_) {
        tz_clz_ = -1;
        const auto src = val(tz_src_);
        const auto dd = dst(d.rd);
        prepare_clobber();
        if (src.is_zero()) a_.mov_imm(dd, bits);
        else a_.tzcnt(bits, dd, in_reg(src));
        return 1;
    }
    const auto nv = val(d.rn);
    const auto dd = dst(d.rd);
    const auto& cpu = cpu_features();
    switch (d.op) {
    case dp1_rev:
        load(bits, dd, nv);
        a_.bswap(bits, dd);
        return 1;
    case dp1_rev32:   // 64-bit only
        load(64, dd, nv);
        a_.bswap(64, dd);
        if (cpu.bmi2) a_.rorx(64, dd, dd, 32);
        else { prepare_clobber(); a_.ror(64, dd, 32); }
        return 1;
    case dp1_rev16:
        if (bits == 32) {
            load(32, dd, nv);
            a_.bswap(32, dd);
            if (cpu.bmi2) a_.rorx(32, dd, dd, 16);
            else { prepare_clobber(); a_.ror(32, dd, 16); }
            return 1;
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
        return 1;
    case dp1_clz: {
        prepare_clobber();
        const auto nr = in_reg(nv);
        if (cpu.lzcnt) {
            a_.lzcnt(bits, dd, nr);
            return 1;
        }
        const auto t = alloc(), c = alloc();
        a_.mov_imm(c, ones(64));
        bsr(a_, bits, t, nr);
        a_.cmov(Cond::e, 64, t, c);
        a_.neg(64, t);
        a_.lea(bits, dd, ptr(t, static_cast<std::int32_t>(bits - 1)));
        return 1;
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
        return 1;
    }
    default: {   // RBIT
        if (cpu.gfni && cpu.avx && std::popcount(free_xmm_) >= 1) {
            // Reverse the bits of every byte with GF2P8AFFINEQB using a
            // per-region RIP-relative matrix (0x8040201008040201), then the
            // byte order with BSWAP. Flag-free, 1 XMM temp, 0 GPR temps.
            if (!rbit_const_label_.valid()) rbit_const_label_ = a_.new_label();
            const auto x = alloc_vec();
            const bool w = bits == 64;
            if (in_place_nr != Gp::none)
                a_.vex_rr({1, 1, 0x6e, w}, false, x64::id(x), 0, x64::id(in_place_nr));
            else if (nv.is_zero()) a_.vex_rr({1, 1, 0xef, false}, false, x64::id(x), x64::id(x), x64::id(x));   // vpxor
            else a_.vex_rr({1, 1, 0x6e, w}, false, x64::id(x), 0, x64::id(in_reg(nv)));                   // vmovq/d x, n
            rbit_sites_.push_back(static_cast<std::uint32_t>(a_.size()));
            a_.vex_rm({1, 3, 0xce, true}, false, x64::id(x), x64::id(x), x64::rip(rbit_const_label_), 1); // vgf2p8affineqb
            a_.db(0);
            a_.vex_rr({1, 1, 0x7e, w}, false, x64::id(x), 0, x64::id(dd));                                // vmovq/d dd, x
            a_.bswap(bits, dd);
            return consumed;
        }
        prepare_clobber();
        const auto t = alloc(), u = alloc();
        if (in_place_nr != Gp::none) a_.mov(bits, t, in_place_nr);
        else load(bits, t, nv);
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
        return consumed;
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
        } else if (av.is_zero() && !sub && isel_on(isel_no_zext)) {
            // MUL Xd/Wd, Xn/Wn, Xm/Wm: emit directly into destination (1 insn
            // in-place, 2 insns otherwise, 0 extra scratch GPRs).
            if (d.rd == 31) return;
            // Known-constant multiplier (e.g. Owen-scrambling hash constants):
            // 3-operand imul dst, src, imm32 eliminates memory loads & extra mov.
            if (isel_on(isel_no_fold)) {
                std::uint64_t cval = 0;
                unsigned base_g = d.rn;
                bool have_const = known_constant(plan_, index_, d.rm, cval);
                if (!have_const && known_constant(plan_, index_, d.rn, cval)) {
                    have_const = true;
                    base_g = d.rm;
                }
                if (have_const) {
                    const auto kval = cval & ones(bits);
                    if (fits_imm32(kval, bits)) {
                        const auto bv = val(base_g);
                        const auto i32 = static_cast<std::int32_t>(static_cast<std::uint32_t>(kval));
                        const Gp br = bv.is_reg() ? bv.r : (bv.is_mem() ? borrow_gpr(bv.g) : Gp::none);
                        const auto dd = dst(d.rd);
                        if (br != Gp::none) {
                            a_.imul(bits, dd, br, i32);
                        } else {
                            load(bits, dd, bv);
                            a_.imul(bits, dd, dd, i32);
                        }
                        return;
                    }
                }
            }
            if (pinned(d.rd)) {
                const auto dd = pin(d.rd);
                note_write(d.rd);
                if (nv.is_reg() && nv.r == dd) {
                    const Gp mh = mv.is_reg() ? mv.r : cached_gpr(mv.g);
                    if (mh != Gp::none) a_.imul(bits, dd, mh);
                    else a_.imul(bits, dd, gmem(mv.g));
                } else if (mv.is_reg() && mv.r == dd) {
                    const Gp nh = nv.is_reg() ? nv.r : cached_gpr(nv.g);
                    if (nh != Gp::none) a_.imul(bits, dd, nh);
                    else a_.imul(bits, dd, gmem(nv.g));
                } else {
                    load(bits, dd, nv);
                    const Gp mh = mv.is_reg() ? mv.r : cached_gpr(mv.g);
                    if (mh != Gp::none) a_.imul(bits, dd, mh);
                    else a_.imul(bits, dd, gmem(mv.g));
                }
                return;
            }
            const auto try_reuse = [&](unsigned g) -> Gp {
                if (!codegen_on(knob_no_gpr_reuse) || !may_cache()) return Gp::none;
                const auto h = gc_.host[g];
                const bool ok = (h >= 8 && h <= 11) || h == 1 || h == 2;
                if (!ok) return Gp::none;
                const auto r = x64::gp(static_cast<unsigned>(h));
                if (!is_free(r) || galias_of_[static_cast<unsigned>(h)] != 0) return Gp::none;
                if (pending_gpr_count_ >= std::size(pending_gpr_)) return Gp::none;
                free_gpr_ &= ~(1u << static_cast<unsigned>(h));
                pending_gpr_[pending_gpr_count_++] = {static_cast<std::uint8_t>(g), r};
                note_write(g);
                return r;
            };
            if (d.rd == d.rn || d.rd == d.rm) {
                if (const auto dd = try_reuse(d.rd); dd != Gp::none) {
                    const Val& other = d.rd == d.rn ? mv : nv;
                    const Gp oh = other.is_reg() ? other.r : cached_gpr(other.g);
                    if (oh != Gp::none) a_.imul(bits, dd, oh);
                    else a_.imul(bits, dd, gmem(other.g));
                    return;
                }
            }
            const Gp nh = nv.is_reg() ? nv.r : (nv.is_mem() ? borrow_gpr(nv.g) : Gp::none);
            const Gp mh = mv.is_reg() ? mv.r : (mv.is_mem() ? borrow_gpr(mv.g) : Gp::none);
            const auto dd = dst(d.rd);
            if (nh != Gp::none) a_.mov(bits, dd, nh);
            else a_.mov(bits, dd, gmem(nv.g));
            if (mh != Gp::none) a_.imul(bits, dd, mh);
            else a_.imul(bits, dd, gmem(mv.g));
            return;
        } else {
            // MADD/MSUB Xd, Xn, Xm, Xa with a pinned Xd: multiply in place when Xd aliases Xn or Xm.
            const bool n_alias = !sub && nv.is_reg() && pinned(d.rd) && d.rd != 31 && nv.r == pin(d.rd) &&
                                 !(av.is_reg() && av.r == nv.r);
            const bool m_alias = !n_alias && !sub && mv.is_reg() && pinned(d.rd) && d.rd != 31 && mv.r == pin(d.rd) &&
                                 !(av.is_reg() && av.r == mv.r);
            const Val& first = m_alias ? mv : nv;
            const Val& second = m_alias ? nv : mv;
            const bool in_place = n_alias || m_alias;
            p = in_place ? first.r : copy_of(first);
            if (in_place) note_write(d.rd);
            const Gp sh = second.is_reg() ? second.r : cached_gpr(second.g);
            if (sh != Gp::none) a_.imul(bits, p, sh);
            else a_.imul(bits, p, gmem(second.g));
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
