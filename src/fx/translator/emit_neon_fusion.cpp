// SIMD mask reductions (owner: neon). Embree and similar code turns a vector
// compare into a lane bit mask with
//   AND  Vd.16B, Vn.16B, Vm.16B      (Vm = {1,1,1,1})   or   USHR Vd.4S, Vn.4S, #31
//   USHL Vd.4S, Vd.4S, Vs.4S         (Vs = {0,1,2,3})
//   ADDV Sd, Vd.4S
//   [FMOV Wt, Sd]
// which x64 does with MOVMSKPS. Every intermediate write goes to Vd, so only
// the ADDV result (a scalar: Vd's other bits zero) and Wt are live after the
// span. The compare itself is not part of the span: its NaN and FPSR
// behaviour is the fp family's, unchanged.
//
// The short form needs Vs == {0,1,2,3} (and Vm == {1,1,1,1} for AND). The
// register values are never inferred from register numbers or addresses:
//  * EmitContext::vec_fact exact: the only reaching definition proves them
//    (MOVI for Vm); no check.
//  * flag: the only reaching definition is a 128-bit load (Embree loads Vs
//    once from .rdata); FamilyFacts::check ran right after it, so a test of
//    one flag selects the short form or the exact general path.
//  * otherwise both values are compared inline (the first lowering).
// The general path computes AND/USHR, USHL (signed low-byte counts: negative
// shifts right, magnitudes >= 32 give zero) and the wrapping ADDV exactly.
// If Vn holds lane masks (its only reaching definition is a vector compare),
// AND 1 keeps each lane's sign bit and MOVMSKPS reads Vn directly.
//
// TRANSMUTE_FX_NEON=nomaskfusion keeps the instructions separate;
// TRANSMUTE_FX_CORE=nofacts makes every span use the inline comparison.
#include "fx/translator/emit_neon_internal.hpp"

namespace transmute::fx::neon {

namespace {

using Value = EmitContext::VecValue;
constexpr Value kLaneIndex{0x0000000100000000ull, 0x0000000300000002ull};
constexpr Value kOnes32{0x0000000100000001ull, 0x0000000100000001ull};

struct MaskIdiom {
    bool ushr{};                 // USHR #31 form (else AND)
    unsigned rd{}, rn{}, rm{}, shifts{};
    bool extract{};              // FMOV Wt, Sd follows
    unsigned wt{};
};

bool match_mask_idiom(std::span<const std::uint32_t> words, MaskIdiom& m) {
    if (words.size() < 3) return false;
    const auto w0 = words[0], w1 = words[1], w2 = words[2];
    if ((w0 & 0xffe0fc00u) == 0x4e201c00u) {        // AND Vd.16B, Vn.16B, Vm.16B
        m.ushr = false;
        m.rm = (w0 >> 16) & 31u;
    } else if ((w0 & 0xfffffc00u) == 0x6f210400u) { // USHR Vd.4S, Vn.4S, #31
        m.ushr = true;
    } else {
        return false;
    }
    m.rd = w0 & 31u;
    m.rn = (w0 >> 5) & 31u;
    if ((w1 & 0xffe0fc00u) != 0x6ea04400u ||        // USHL Vd.4S, Vd.4S, Vs.4S
        (w2 & 0xfffffc00u) != 0x4eb1b800u)          // ADDV Sd, Vd.4S
        return false;
    m.shifts = (w1 >> 16) & 31u;
    // Every write is to Vd, and the shift counts are not the first write's.
    if ((w1 & 31u) != m.rd || ((w1 >> 5) & 31u) != m.rd || (w2 & 31u) != m.rd || ((w2 >> 5) & 31u) != m.rd ||
        m.shifts == m.rd)
        return false;
    m.extract = words.size() >= 4 && (words[3] & 0xfffffc00u) == 0x1e260000u && ((words[3] >> 5) & 31u) == m.rd;
    m.wt = m.extract ? (words[3] & 31u) : 0u;
    return true;
}

// FCMxx / FACxx / CMxx (register and zero forms) with 32- or 64-bit lanes:
// each 32-bit lane of the result is all ones or all zeros.
bool is_lane_mask_compare(std::uint32_t w) {
    static constexpr std::uint32_t kThree[] = {   // mask 0xffa0fc00 (bit 22: S/D)
        0x4e20e400u, 0x6e20e400u, 0x6ea0e400u, 0x6e20ec00u, 0x6ea0ec00u,   // FCMEQ FCMGE FCMGT FACGE FACGT
        0x4ea03400u, 0x4ea03c00u, 0x6ea03400u, 0x6ea03c00u, 0x6ea08c00u, 0x4ea08c00u,   // CMGT CMGE CMHI CMHS CMEQ CMTST
    };
    static constexpr std::uint32_t kZero[] = {    // mask 0xffbffc00 (bit 22: S/D)
        0x4ea0c800u, 0x6ea0c800u, 0x4ea0d800u, 0x6ea0d800u, 0x4ea0e800u,   // FCMGT FCMGE FCMEQ FCMLE FCMLT #0
        0x4ea08800u, 0x6ea08800u, 0x4ea09800u, 0x6ea09800u, 0x4ea0a800u,   // CMGT CMGE CMEQ CMLE CMLT #0
    };
    for (const auto e : kThree)
        if ((w & 0xffa0fc00u) == e) return true;
    for (const auto e : kZero)
        if ((w & 0xffbffc00u) == e) return true;
    return false;
}

// LDR Q (unsigned offset, unscaled, pre/post-index, register offset,
// literal) and LDP/LDNP Q: a full 128-bit load into V`vec`.
bool is_q_load(std::uint32_t w, unsigned vec) {
    const unsigned rt = w & 31u, rt2 = (w >> 10) & 31u;
    if ((w & 0xffc00000u) == 0x3dc00000u) return rt == vec;                       // LDR Q, [Xn, #uimm]
    if ((w & 0xffe00000u) == 0x3cc00000u && ((w >> 10) & 3u) != 2u) return rt == vec;   // LDUR / post / pre
    if ((w & 0xffe00c00u) == 0x3ce00800u) return rt == vec;                       // LDR Q, [Xn, Xm{, ext}]
    if ((w & 0xff000000u) == 0x9c000000u) return rt == vec;                       // LDR Q, literal
    if ((w & 0xffc00000u) == 0xad400000u || (w & 0xffc00000u) == 0xacc00000u ||   // LDP Q offset / post
        (w & 0xffc00000u) == 0xadc00000u || (w & 0xffc00000u) == 0xac400000u)     // LDP Q pre / LDNP Q
        return rt != rt2 && (rt == vec || rt2 == vec);
    return false;
}

// MOVI / MVNI (not the ORR/BIC forms, not FMOV): the written constant.
bool modified_immediate_value(std::uint32_t w, Value& value) {
    if ((w & 0x9ff80400u) != 0x0f000400u || bit(w, 11)) return false;
    const bool q = bit(w, 30), opb = bit(w, 29);
    const unsigned cmode = bits(w, 15, 12);
    if (cmode == 0xf) return false;
    const std::uint64_t imm8 = (bits(w, 18, 16) << 5) | bits(w, 9, 5);
    std::uint64_t imm64 = 0;
    bool invert = false;
    switch (cmode >> 1) {
    case 0: case 1: case 2: case 3:
        if (cmode & 1) return false;   // ORR / BIC
        imm64 = rep32(static_cast<std::uint32_t>(imm8 << (8 * (cmode >> 1))));
        invert = opb;
        break;
    case 4: case 5:
        if (cmode & 1) return false;
        imm64 = rep16(static_cast<std::uint16_t>(imm8 << (8 * ((cmode >> 1) & 1))));
        invert = opb;
        break;
    case 6:
        imm64 = rep32(cmode & 1 ? static_cast<std::uint32_t>((imm8 << 16) | 0xffff)
                                : static_cast<std::uint32_t>((imm8 << 8) | 0xff));
        invert = opb;
        break;
    default:   // cmode 1110
        if (!opb) {
            imm64 = rep8(static_cast<std::uint8_t>(imm8));
        } else {
            for (unsigned i = 0; i < 8; ++i)
                if (imm8 & (1ULL << i)) imm64 |= 0xffULL << (8 * i);
        }
        break;
    }
    const auto v = invert ? ~imm64 : imm64;
    value = {v, q ? v : 0};
    return true;
}

// t = (src == value) per 32-bit lane (all ones / zeros), src in a register or memory.
void lanes_equal(V& v, Xmm t, const Src& src, const Value& value) {
    if (!src.is_mem) {
        v.op(op::pcmpeqd, t, src.reg, v.k(Const128{value.lo, value.hi}));
        return;
    }
    v.a.movdqa(t, v.k(Const128{value.lo, value.hi}));
    v.op(op::pcmpeqd, t, t, src.mem);
}

} // namespace

// TBL (not TBX) with `count` table registers: its adjusted indices, one per
// table register (emit_neon_perm.cpp), may come precomputed (tier 2).
bool tbl_word(std::uint32_t w, unsigned& rm, unsigned& count) {
    if ((w & 0xbfe09c00u) != 0x0e000000u) return false;   // TBL/TBX (q, len, op), op = TBL
    rm = (w >> 16) & 31u;
    count = ((w >> 13) & 3u) + 1;
    return true;
}

unsigned mask_fact_want(std::span<const std::uint32_t> words, FamilyFactWant* out) {
    if (unsigned rm, count; tbl_word(words[0], rm, count) && neon_on(neon_no_tbl_derived)) {
        for (unsigned k = 0; k < count; ++k)
            out[k] = {FamilyFactWant::K::derived, static_cast<std::uint8_t>(rm), {k, 0}};
        return count;
    }
    MaskIdiom m;
    if (!neon_on(neon_no_mask_fusion) || !match_mask_idiom(words, m)) return 0;
    unsigned n = 0;
    out[n++] = {FamilyFactWant::K::value, static_cast<std::uint8_t>(m.shifts), kLaneIndex};
    if (!m.ushr) {
        out[n++] = {FamilyFactWant::K::value, static_cast<std::uint8_t>(m.rm), kOnes32};
        out[n++] = {FamilyFactWant::K::lane_masks, static_cast<std::uint8_t>(m.rn), {}};
    }
    return n;
}

FamilyFactDef mask_fact_def(std::uint32_t w, unsigned vec, Value& value) {
    if ((w & 31u) == vec && modified_immediate_value(w, value)) return FamilyFactDef::constant;
    if ((w & 31u) == vec && is_lane_mask_compare(w)) return FamilyFactDef::lane_masks;
    if (is_q_load(w, vec)) return FamilyFactDef::load128;
    return FamilyFactDef::unknown;
}

// flag = 0 iff all 128 bits equal `value`: pcmpeqd; pmovmskb (0xffff iff
// equal); lea +1 (low 16 bits zero iff equal); 16-bit store. No EFLAGS.
void mask_fact_check(EmitContext& ctx, Xmm reg, bool in_reg, const Mem& slot, const Value& value, const Mem& flag) {
    auto& as = ctx.as();
    begin_instruction_constants(as);
    V v(ctx);
    const Xmm t = ctx.temp_vec();
    const Gp r = ctx.temp_gpr();
    Src src;
    src.is_mem = !in_reg;
    src.reg = reg;
    src.mem = slot;
    lanes_equal(v, t, src, value);
    as.pmovmskb(r, t);
    as.lea(32, r, x64::ptr(r, 1));
    as.mov(16, flag, r);
    flush_instruction_constants(as);
}

// Derived value `kind` of TBL's index vector: idx - 16 * kind with bit 7
// set where it lies outside [0, 16) (pshufb then zeroes the lane).
void tbl_derive(EmitContext& ctx, Xmm src, unsigned kind, const Mem& out) {
    auto& as = ctx.as();
    begin_instruction_constants(as);
    V v(ctx);
    const Xmm t = ctx.temp_vec();
    if (kind == 0) {
        v.op(op::paddusb, t, src, splat8(0x70));
    } else {
        v.op(op::psubb, t, src, splat8(static_cast<std::uint8_t>(16 * kind)));
        v.op(op::paddusb, t, t, splat8(0x70));
    }
    as.movdqu(out, t);
    flush_instruction_constants(as);
}

unsigned emit_mask_reduce(EmitContext& ctx, std::span<const std::uint32_t> words) {
    MaskIdiom m;
    if (!neon_on(neon_no_mask_fusion) || !match_mask_idiom(words, m)) return 0;
    using K = EmitContext::VecFact::K;
    const auto counts_fact = ctx.vec_fact(m.shifts, kLaneIndex);
    const auto mask_fact = m.ushr ? EmitContext::VecFact{K::exact, {}} : ctx.vec_fact(m.rm, kOnes32);
    // AND 1 keeps bit 0; MOVMSKPS reads bit 31. Equal for lane masks.
    const bool sign_is_bit0 = m.ushr || ctx.vec_lane_masks(m.rn);
    // The guard: none (both exact), one checked flag, or inline comparisons.
    const bool exact = counts_fact.k == K::exact && mask_fact.k == K::exact;
    const EmitContext::VecFact* flag = nullptr;
    bool inline_counts = false, inline_mask = false;
    if (!exact) {
        if (counts_fact.k == K::flag && mask_fact.k == K::exact) flag = &counts_fact;
        else if (mask_fact.k == K::flag && counts_fact.k == K::exact) flag = &mask_fact;
        else {
            inline_counts = counts_fact.k != K::exact;
            inline_mask = mask_fact.k != K::exact;
        }
    }
    if (!exact && !features().avx2) return 0;   // the general path uses vpsllvd/vpsrlvd

    // A preceding compare often still feeds a later B.cond: a guard with RCX
    // (JRCXZ) keeps the host flags; otherwise the context saves live flags.
    Gp counter = Gp::none;
    if (!exact) {
        counter = neon_on(neon_mask_no_rcx) ? ctx.temp_gpr_prefer(Gp::rcx) : Gp::none;
        if (counter == Gp::none) ctx.clobber_host_flags();
    }
    V v(ctx);
    auto& a = v.a;
    // Rule 5: every operand and temp before the first label. The shift and
    // mask sources stay in memory when they are there: only the general
    // path (or an inline comparison) reads them.
    const Xmm n = in_reg(ctx, source(ctx, m.rn));
    // A source only the general path reads stays in its slot when that is
    // current (no load on the short path); else it is already in a register.
    const auto lazy = [&](unsigned index) {
        Src s;
        s.guest = index;
        if (ctx.vec_slot(index, s.mem)) s.is_mem = true;
        else s = source(ctx, index);
        return s;
    };
    Src counts{}, mask{};
    if (!exact) {
        counts = inline_counts ? source(ctx, m.shifts) : lazy(m.shifts);
        if (!m.ushr) mask = inline_mask ? source(ctx, m.rm) : lazy(m.rm);
    }
    const Xmm t0 = (!exact || !sign_is_bit0) ? ctx.temp_vec() : Xmm{};
    const Xmm t1 = !exact ? ctx.temp_vec() : Xmm{};
    const Xmm t2 = !exact ? ctx.temp_vec() : Xmm{};
    const Gp guard = !exact ? (counter != Gp::none ? counter : ctx.temp_gpr()) : Gp::none;
    const Gp out = m.extract ? ctx.write_gpr(m.wt) : ctx.temp_gpr();
    const auto fast = a.new_label(), done = a.new_label();

    if (!exact) {
        if (flag) {
            if (counter != Gp::none) a.movzx(32, counter, flag->flag, 16);
            else a.cmp(16, flag->flag, 0);
        } else {
            if (inline_counts) lanes_equal(v, t0, counts, kLaneIndex);
            if (inline_mask) {
                lanes_equal(v, t1, mask, kOnes32);
                if (inline_counts) v.pand(t0, t0, t1);
                else v.mov(t0, t1);
            }
            a.movmskps(guard, t0);
            if (counter != Gp::none) a.lea(64, counter, x64::ptr(counter, -15));
            else a.cmp(32, guard, 15);
        }
        if (counter != Gp::none) a.jrcxz_short(fast);
        else a.jcc_short(x64::Cond::e, fast);

        // General path. t2 = lanes in {bit 0 of AND, or n >> 31}.
        if (m.ushr) v.psrld(t2, n, 31);
        else v.op(op::pand, t2, n, mask);
        Xmm c = t1;   // signed low bytes of the counts
        if (counts.is_mem) a.movdqu(c, counts.mem);
        else c = counts.reg;
        v.pslld(t1, c, 24);
        v.psrad(t1, t1, 24);
        v.vex3(vop::vpsllvd, t0, t2, t1);          // counts 0..31 (larger and negative ones: 0)
        v.op(op::psignd, t1, t1, v.k(splat32(0xffffffffu)));   // -counts
        v.vex3(vop::vpsrlvd, t2, t2, t1);          // counts -1..-31 (others: 0, or count 0: x)
        v.por(t2, t2, t0);                          // count 0: x | x
        v.op(op::phaddd, t2, t2, t2);
        v.op(op::phaddd, t2, t2, t2);
        a.movd(out, t2);
        a.jmp_short(done);
        a.bind(fast);
    }
    // {0,1,2,3} shifts of single bits: the sum is the lane bit mask.
    if (sign_is_bit0) {
        a.movmskps(out, n);
    } else {
        v.pslld(t0, n, 31);
        a.movmskps(out, t0);
    }
    a.bind(done);
    const Xmm dst = ctx.write_vec(m.rd);   // every source is consumed
    a.movd(dst, out);
    ctx.note_vec_zero_above(m.rd, 32);
    return m.extract ? 4u : 3u;
}

} // namespace transmute::fx::neon

namespace transmute::fx {

const FamilyFacts* neon_family_facts() {
    static const FamilyFacts facts{&neon::mask_fact_want, &neon::mask_fact_def, &neon::mask_fact_check, &neon::tbl_derive};
    return &facts;
}

} // namespace transmute::fx
