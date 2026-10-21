// fp family: conversions between floating point, integers and fixed point,
// precision conversions and round-to-odd narrowing (owner: fp).
#include "fx/translator/emit_fp_internal.hpp"

namespace transmute::fx::fpx {

namespace {

std::uint64_t pow2_bits(unsigned es, int exponent) {
    if (es == 64) return static_cast<std::uint64_t>(1023 + exponent) << 52;
    return static_cast<std::uint64_t>(127 + exponent) << 23;
}

std::uint8_t round_imm(Rnd r) {
    switch (r) {
    case Rnd::n: return 0x00;
    case Rnd::m: return 0x01;
    case Rnd::p: return 0x02;
    default: return 0x03;
    }
}

void cvtt(FpEmit& e, bool dbl, Gp g, Xmm x, unsigned bits) {
    if (dbl) e.as.cvttsd2si(g, x, bits);
    else e.as.cvttss2si(g, x, bits);
}

// Sets MXCSR.IE (A64 IOC) without changing any register but `scratch`.
void raise_invalid(FpEmit& e, Xmm scratch) {
    e.ones(scratch);
    e.ucomi(false, scratch, scratch, true);
}

// g = A64 saturating truncation of lane 0 of x (single or double) to an
// `ibits` integer: NaN -> 0, out of range -> the nearest bound. Branches
// only off the in-range path. Emits labels: every operand must already be
// allocated. Clobbers EFLAGS (the caller has called clobber_host_flags()).
void cvtt_scalar(FpEmit& e, bool dbl, Xmm x, Gp g, unsigned ibits, bool uns, Gp gt, Xmm tc, Xmm tx) {
    auto& a = e.as;
    const Label done = a.new_label();
    if (!uns) {
        cvtt(e, dbl, g, x, ibits);
        // x64 returns the "integer indefinite" (INT_MIN) for NaN and overflow.
        if (ibits == 32) {
            a.cmp(32, g, static_cast<std::int32_t>(0x80000000u));
            a.jcc_short(Cond::ne, done);
        } else {
            a.cmp(64, g, 1);   // overflows only for INT64_MIN
            a.jcc_short(Cond::no, done);
        }
        // Negative overflow keeps INT_MIN, positive overflow -> INT_MAX, NaN -> 0.
        if (dbl) e.movq(gt, x);
        else { e.movd(gt, x); a.shl(64, gt, 32); }
        a.sar(64, gt, 63);
        a.not_(64, gt);
        a.xor_(ibits, g, gt);
        a.xor_(32, gt, gt);
        e.ucomi(dbl, x, x, false);
        a.cmov(Cond::p, ibits, g, gt);
        a.bind(done);
        return;
    }
    if (ibits == 32) {
        cvtt(e, dbl, g, x, 64);
        a.mov(32, gt, g);
        a.cmp(64, gt, g);
        a.jcc_short(Cond::e, done);
        // Out of [0, 2^32): positive -> UINT32_MAX, negative or NaN -> 0.
        e.zero(tc);
        e.ucomi(dbl, x, tc, false);
        a.mov_imm(g, 0xffffffffu);
        a.mov_imm(gt, 0);
        a.cmov(Cond::be, 32, g, gt);
        raise_invalid(e, tc);
        a.bind(done);
        return;
    }
    const Label zero_l = a.new_label(), sat_l = a.new_label(), raise_l = a.new_label();
    cvtt(e, dbl, g, x, 64);
    a.test(64, g, g);
    a.jcc_short(Cond::ns, done);
    // NaN, x <= -1 or x >= 2^63.
    a.mov_imm(gt, dbl ? 0x43e0000000000000ULL : 0x5f000000ULL);   // 2^63
    if (dbl) e.movq(tc, gt);
    else e.movd(tc, gt);
    e.ucomi(dbl, x, tc, false);
    a.jcc_short(Cond::b, zero_l);   // below 2^63 or unordered
    e.op(dbl ? op::subsd : op::subss, tx, x, tc);   // exact
    cvtt(e, dbl, g, tx, 64);
    a.test(64, g, g);
    a.jcc_short(Cond::s, sat_l);
    a.btc(64, g, 63);
    a.jmp_short(done);
    a.bind(sat_l);
    a.mov_imm(g, ~0ULL);
    a.jmp_short(raise_l);
    a.bind(zero_l);
    a.mov_imm(g, 0);
    a.bind(raise_l);
    raise_invalid(e, tc);
    a.bind(done);
}

// d[0] = (float/double) r as an unsigned `ibits` integer (upper lanes of d
// must already be zero). Emits labels for 64-bit sources.
// `zext`: r already holds the 32-bit source zero-extended (movd from a vector).
void cvt_u2f_scalar(FpEmit& e, bool dbl, Xmm d, Gp r, unsigned ibits, Gp t, Gp u, bool zext = false) {
    auto& a = e.as;
    if (ibits == 32) {
        if (zext) {
            e.cvtsi2s(dbl, d, r, 64);
            return;
        }
        a.mov(32, t, r);
        e.cvtsi2s(dbl, d, t, 64);
        return;
    }
    // Common path: the value fits int64 and the branch skips the fix-up.
    const Label small = a.new_label(), done = a.new_label();
    a.test(64, r, r);
    a.jcc_short(Cond::ns, small);
    // >= 2^63: halve with a sticky bit (round-to-odd), convert, double.
    a.mov(64, t, r);
    a.shr(64, t, 1);
    a.mov(32, u, r);
    a.and_(32, u, 1);
    a.or_(64, t, u);
    e.cvtsi2s(dbl, d, t, 64);
    e.op(dbl ? op::addsd : op::addss, d, d, d);
    a.jmp_short(done);
    a.bind(small);
    e.cvtsi2s(dbl, d, r, 64);
    a.bind(done);
}

// Prepares the source of a float->int conversion: fixed-point scaling and
// rounding. Uses T[0] (constants) and T[2] (result); FCVTA* uses all four.
// x may be T[3]. Returns the register to truncate.
Xmm prepare_f2i(FpEmit& e, const Inst& in, Shape s, Xmm x, const Xmm (&T)[4]) {
    if (in.fbits) {
        if (s.scalar) e.scalar_const(s.es, T[0], pow2_bits(s.es, in.fbits));
        else e.splat(s.es, T[0], pow2_bits(s.es, in.fbits));
        e.op(s.pick(fops::mul), T[2], x, T[0]);
        x = T[2];
    }
    switch (in.rnd) {
    case Rnd::z:
        return x;
    case Rnd::a:
        emit_round_away(e, s, T[2], x, true, T);
        return T[2];
    default:
        e.round(s, T[2], x, round_imm(in.rnd));
        return T[2];
    }
}

// Scalar float -> integer (general register or SIMD scalar destination).
bool f2i_scalar(FpEmit& e, const Inst& in) {
    auto& a = e.as;
    Shape s = in.sh;
    Xmm x = e.vec(in.rn);
    const Xmm T[4] = {e.tmp(), e.tmp(), e.tmp(), e.tmp()};
    e.const_gpr();   // before internal labels
    if (s.es == 16) {
        e.cvtph2ps(T[3], x);
        x = T[3];
        s.es = 32;
    }
    x = prepare_f2i(e, in, s, x, T);
    const unsigned ibits = in.gpr ? in.ibits : (in.sh.es == 16 ? 32 : in.sh.es);
    const Gp gt = e.gtmp();
    Gp g;
    Xmm d{};
    if (in.gpr) {
        g = e.gdest(in.rd);
    } else {
        g = e.gtmp();
        d = e.dest(in.rd);
    }
    e.ctx.clobber_host_flags();
    cvtt_scalar(e, s.dbl(), x, g, ibits, in.is_unsigned, gt, T[0], T[1]);
    if (in.gpr) return true;
    if (in.sh.es == 16) {   // saturate to 16 bits
        if (in.is_unsigned) {
            a.mov_imm(gt, 0xffff);
            a.cmp(32, g, gt);
            a.cmov(Cond::a, 32, g, gt);
        } else {
            a.mov_imm(gt, 0x7fff);
            a.cmp(32, g, gt);
            a.cmov(Cond::g, 32, g, gt);
            a.mov_imm(gt, 0xffff8000u);
            a.cmp(32, g, gt);
            a.cmov(Cond::l, 32, g, gt);
            a.movzx(32, g, g, 16);
        }
        e.movd(d, g);
    } else if (in.sh.es == 64) {
        e.movq(d, g);
    } else {
        e.movd(d, g);
    }
    return true;
}

// Vector float -> integer for S and D elements.
bool f2i_vector(FpEmit& e, const Inst& in) {
    const Shape s = in.sh;
    // A 64-bit vector whose source is zero above bit 64: those lanes convert
    // to 0 without flags, so neither the source nor the result is zeroed.
    const bool zsrc = !s.q && s.es == 32 && e.zero_above(in.rn, 64) && fp_on(fp_old_cvt);
    const unsigned w = zsrc ? 128 : s.width();
    Xmm x = e.vec(in.rn);
    const Xmm T[4] = {e.tmp(), e.tmp(), e.tmp(), e.tmp()};
    e.const_gpr();   // before internal labels
    if (!s.q && !zsrc) {
        e.movq(T[3], x);   // clean upper lanes: no spurious exceptions
        x = T[3];
    }
    x = prepare_f2i(e, in, s, x, T);
    if (s.es == 64) {
        const Xmm hi = T[3];
        e.un_i(op::pshufd, hi, x, 0xee);
        const Gp g0 = e.gtmp(), g1 = e.gtmp(), gt = e.gtmp();
        const Xmm d = e.dest(in.rd);
        e.ctx.clobber_host_flags();
        cvtt_scalar(e, true, x, g0, 64, in.is_unsigned, gt, T[0], T[1]);
        cvtt_scalar(e, true, hi, g1, 64, in.is_unsigned, gt, T[0], T[1]);
        e.movq(d, g0);
        if (e.avx()) {
            e.as.vex_rr(VexOp{1, 3, 0x22, true}, false, x64::id(d), x64::id(d), x64::id(g1));
            e.as.db(1);
        } else {
            e.as.pinsr(64, d, g1, 1);
        }
        return true;
    }
    constexpr std::uint32_t two31 = 0x4f000000u;
    if (!in.is_unsigned && e.avx()) {
        // cvttps2dq gives INT_MIN for NaN and out-of-range lanes: flip the
        // lanes >= 2^31 (or NaN) to INT_MAX, then clear the NaN lanes.
        e.un(op::cvttps2dq, T[0], x);
        e.op_i(op::cmpps, T[1], x, Opnd::at(e.ksplat(32, two31)), pred_nlt);
        e.op(op::pxor, T[0], T[0], T[1]);
        e.op_i(op::cmpps, T[1], x, x, pred_ord);
        final_op(e, in.rd, w, op::pand, T[0], T[1], true);
        return true;
    }
    if (!in.is_unsigned) {
        e.un(op::cvttps2dq, T[0], x);
        e.splat(32, T[1], two31);
        e.op_i(op::cmpps, T[1], T[1], x, pred_le);   // x >= 2^31 -> INT_MAX
        e.op(op::pxor, T[0], T[0], T[1]);
        e.op_i(op::cmpps, T[1], x, x, pred_ord);    // NaN -> 0
        final_op(e, in.rd, w, op::pand, T[0], T[1], true);
        return true;
    }
    // Unsigned: clamp below at 0 (NaN -> 0), bias values >= 2^31.
    const Xmm A = T[1], C = T[0], B = T[2], R = T[3];
    if (e.avx()) {   // the 2^31 constant folded from the pool
        const Opnd k31 = Opnd::at(e.ksplat(32, two31));
        e.zero(C);
        e.op(op::maxps, A, x, C);
        e.op_i(op::cmpps, B, A, k31, pred_nlt);   // big lanes (A is not NaN)
        e.op(op::andps, C, B, k31);
        e.op(op::subps, A, A, C);
        e.un(op::cvttps2dq, R, A);
        e.pslld(B, B, 31);
        e.op(op::pxor, R, R, B);
        e.op_i(op::cmpps, C, A, k31, pred_nlt);   // >= 2^32 -> UINT32_MAX
        final_op(e, in.rd, w, op::por, R, C, true);
        return true;
    }
    e.zero(C);
    e.op(op::maxps, A, x, C);                 // x dead from here
    e.splat(32, C, two31);
    e.op_i(op::cmpps, B, C, A, pred_le);      // big lanes
    e.op(op::andps, C, C, B);
    e.op(op::subps, A, A, C);
    e.un(op::cvttps2dq, R, A);
    e.pslld(B, B, 31);
    e.op(op::pxor, R, R, B);
    e.splat(32, C, two31);
    e.op_i(op::cmpps, C, C, A, pred_le);      // >= 2^32 -> UINT32_MAX
    final_op(e, in.rd, w, op::por, R, C, true);
    return true;
}

// Vector half -> int16 (FP16): widen to single, convert with saturation to
// int32, then saturate to 16 bits (signed pack for FCVT*S, unsigned for FCVT*U;
// both use the signed int32 conversion: |half| <= 65504).
bool f2i_vector_h(FpEmit& e, const Inst& in) {
    const Shape s = in.sh;
    const Shape fs{32, false, true};
    const unsigned groups = s.q ? 2 : 1;
    const Xmm n = e.vec(in.rn);
    const Xmm T[4] = {e.tmp(), e.tmp(), e.tmp(), e.tmp()};
    e.const_gpr();
    const Xmm d = e.dest(in.rd);
    for (unsigned g = 0; g < groups; ++g) {
        if (g == 0) e.cvtph2ps(T[3], n);
        else { e.un_i(op::pshufd, T[3], n, 0xee); e.cvtph2ps(T[3], T[3]); }
        const Xmm x = prepare_f2i(e, in, fs, T[3], T);
        e.un(op::cvttps2dq, T[0], x);
        e.splat(32, T[1], 0x4f000000u);
        e.op_i(op::cmpps, T[1], T[1], x, pred_le);
        e.op(op::pxor, T[0], T[0], T[1]);
        e.op_i(op::cmpps, T[1], x, x, pred_ord);
        e.op(op::pand, T[0], T[0], T[1]);
        e.op(in.is_unsigned ? op::packusdw : op::packssdw, T[0], T[0], T[0]);
        if (groups == 1) e.movq(d, T[0]);
        else if (g == 0) e.op(op::movsd, d, d, T[0]);
        else e.op(op::movlhps, d, d, T[0]);
    }
    return true;
}

// Vector int16 -> half (FP16): exact through single, one rounding.
bool i2f_vector_h(FpEmit& e, const Inst& in) {
    const Shape s = in.sh;
    const unsigned groups = s.q ? 2 : 1;
    const Xmm n = e.vec(in.rn);
    const Xmm T0 = e.tmp(), T1 = e.tmp();
    const Xmm d = e.dest(in.rd);
    for (unsigned g = 0; g < groups; ++g) {
        if (g == 0) e.mov(T0, n);
        else e.un_i(op::pshufd, T0, n, 0xee);
        e.un(in.is_unsigned ? op::pmovzxwd : op::pmovsxwd, T0, T0);
        e.un(op::cvtdq2ps, T0, T0);
        if (in.fbits) {
            e.splat(32, T1, pow2_bits(32, -static_cast<int>(in.fbits)));
            e.op(op::mulps, T0, T0, T1);
        }
        e.cvtps2ph(T1, T0);
        if (groups == 1) e.movq(d, T1);
        else if (g == 0) e.op(op::movsd, d, d, T1);
        else e.op(op::movlhps, d, d, T1);
    }
    return true;
}

// Scalar integer -> half. int16 (SIMD scalar) and int32 sources are exact in
// double; a 64-bit source is clamped to +-2^20 (same overflow result) when
// fbits == 0 and declined otherwise. Narrowing: round-to-odd to single,
// then one rounding to half.
bool i2f_scalar_h(FpEmit& e, const Inst& in) {
    auto& a = e.as;
    if (in.gpr && in.ibits == 64 && in.fbits) return false;
    Gp r;
    if (in.gpr) {
        r = e.gpr(in.rn);
    } else {
        const Xmm n = e.vec(in.rn);
        r = e.gtmp();
        a.pextr(16, r, n, 0);
    }
    const Gp t = e.gtmp(), c = e.gtmp();
    const Xmm D = e.tmp(), F = e.tmp(), B = e.tmp(), M = e.tmp();
    e.const_gpr();
    e.ctx.clobber_host_flags();
    const unsigned bits = in.gpr ? in.ibits : 16;
    if (bits == 16) {
        if (in.is_unsigned) a.movzx(32, t, r, 16);
        else a.movsx(64, t, r, 16);
    } else if (bits == 32) {
        if (in.is_unsigned) a.mov(32, t, r);
        else a.movsx(64, t, r, 32);
    } else {
        a.mov(64, t, r);
        if (in.is_unsigned) {
            a.mov_imm(c, 1ULL << 20);
            a.cmp(64, t, c);
            a.cmov(Cond::a, 64, t, c);
        } else {
            a.mov_imm(c, 1ULL << 20);
            a.cmp(64, t, c);
            a.cmov(Cond::g, 64, t, c);
            a.mov_imm(c, static_cast<std::uint64_t>(-(1LL << 20)));
            a.cmp(64, t, c);
            a.cmov(Cond::l, 64, t, c);
        }
    }
    e.zero(D);
    e.cvtsi2s(true, D, t, 64);   // exact
    if (in.fbits) {
        e.scalar_const(64, F, pow2_bits(64, -static_cast<int>(in.fbits)));
        e.op(op::mulsd, D, D, F);
    }
    emit_narrow_round_odd(e, false, F, D, B, M);
    e.ins0(F, F);
    const Xmm dst = e.dest(in.rd);
    e.cvtps2ph(dst, F);
    return true;
}

// Scalar integer -> float.
bool i2f_scalar(FpEmit& e, const Inst& in) {
    const Shape s = in.sh;
    if (s.es == 16) return i2f_scalar_h(e, in);
    const bool dbl = s.dbl();
    if (in.gpr && !in.is_unsigned && !in.fbits && e.avx()) {   // SCVTF from a register in memory
        const auto g = e.ctx.gsrc(in.rn);
        if (g.is_mem) {
            const Xmm d = e.dest(in.rd);
            e.zero(d);
            e.as.vex_rm(VexOp{static_cast<std::uint8_t>(dbl ? 3 : 2), 1, 0x2a, in.ibits == 64}, false, x64::id(d),
                        x64::id(d), g.mem);
            return true;
        }
    }
    if (!in.gpr && !dbl && !in.is_unsigned && !in.fbits && e.avx() && fp_on(fp_old_cvt)) {
        // SCVTF Sd, Sn in the vector domain: cvtdq2ps of {n[0], 0, 0, 0}
        // (zero lanes convert to +0 exactly: no flags, zero upper result).
        const Opnd n = e.src(in.rn);
        Opnd x = n;
        if (n.is_mem) {
            x = Opnd::of(load_opnd(e, n, s));   // movss: upper lanes zero
        } else if (!e.zero_above(in.rn, 32)) {
            const Xmm t = e.tmp();
            e.ins0(t, n.reg);
            x = Opnd::of(t);
        }
        const Xmm d = e.dest(in.rd);
        e.un(op::cvtdq2ps, d, x);
        return true;
    }
    Gp r;
    unsigned ibits;
    if (in.gpr) {
        r = e.gpr(in.rn);
        ibits = in.ibits;
    } else {
        const Xmm n = e.vec(in.rn);
        r = e.gtmp();
        if (dbl) e.movq(r, n);
        else e.movd(r, n);
        ibits = s.es;
    }
    const Gp t = e.gtmp(), u = e.gtmp();
    const Xmm c = in.fbits ? e.tmp() : Xmm{};
    if (in.fbits) e.const_gpr();   // before internal labels
    const Xmm d = e.dest(in.rd);
    e.zero(d);
    if (!in.is_unsigned) {
        e.cvtsi2s(dbl, d, r, ibits);
    } else {
        if (ibits == 64) e.ctx.clobber_host_flags();
        cvt_u2f_scalar(e, dbl, d, r, ibits, t, u, !in.gpr && fp_on(fp_old_cvt));
    }
    if (in.fbits) {
        e.scalar_const(s.es, c, pow2_bits(s.es, -static_cast<int>(in.fbits)));
        e.op(s.pick(fops::mul), d, d, c);   // exact
    }
    return true;
}

// Vector integer -> float.
bool i2f_vector(FpEmit& e, const Inst& in) {
    const Shape s = in.sh;
    const unsigned w = s.width();
    if (s.es == 32 && !in.is_unsigned && !in.fbits && s.q) {   // hot: one instruction
        const Opnd n = e.src(in.rn);
        const Xmm d = e.dest(in.rd);
        e.un(op::cvtdq2ps, d, n);
        return true;
    }
    Xmm x = e.vec(in.rn);
    if (s.es == 64) {
        const Gp g0 = e.gtmp(), g1 = e.gtmp(), t = e.gtmp(), u = e.gtmp();
        const Xmm t0 = e.tmp(), t1 = e.tmp();
        const Xmm c = in.fbits ? e.tmp() : Xmm{};
        if (in.fbits) e.const_gpr();   // before internal labels
        e.movq(g0, x);
        e.as.pextr(64, g1, x, 1);
        e.zero(t0);
        e.zero(t1);
        const Xmm d = e.dest(in.rd);
        if (!in.is_unsigned) {
            e.cvtsi2s(true, t0, g0, 64);
            e.cvtsi2s(true, t1, g1, 64);
        } else {
            e.ctx.clobber_host_flags();
            cvt_u2f_scalar(e, true, t0, g0, 64, t, u);
            cvt_u2f_scalar(e, true, t1, g1, 64, t, u);
        }
        e.op(op::unpcklpd, d, t0, t1);
        if (in.fbits) {
            e.splat(64, c, pow2_bits(64, -static_cast<int>(in.fbits)));
            e.op(op::mulpd, d, d, c);
        }
        return true;
    }
    if (!in.is_unsigned && !in.fbits && s.q) {   // hot: one instruction
        const Xmm d = e.dest(in.rd);
        e.un(op::cvtdq2ps, d, x);
        return true;
    }
    // 64-bit vector whose source is zero above bit 64: the upper lanes convert
    // to +0 (exactly, no flags), so neither the source nor the result needs
    // zeroing.
    const bool zsrc = !s.q && e.zero_above(in.rn, 64) && fp_on(fp_old_cvt);
    if (zsrc && !in.is_unsigned && !in.fbits) {
        const Xmm d = e.dest(in.rd);
        e.un(op::cvtdq2ps, d, x);
        return true;
    }
    if (in.is_unsigned && !in.fbits && e.avx() && e.feat.fma && fp_on(fp_old_cvt)) {
        // u32 -> single: hi = x >> 16 and lo = x & 0xffff convert exactly;
        // hi * 65536 + lo in one FMA is a single correct rounding (FPCR mode;
        // inexact sets IXC only then). 64-bit vectors convert zeroed upper
        // lanes (+0, no flags), so the result needs no zeroing.
        const Xmm lo = e.tmp();
        Xmm src = x;
        if (!s.q && !zsrc) {
            const Xmm c = e.tmp();
            e.movq(c, x);
            src = c;
        }
        e.op(op::pand, lo, src, Opnd::at(e.ksplat(32, 0xffff)));
        const Xmm d = e.dest(in.rd);
        e.psrld(d, src, 16);
        e.un(op::cvtdq2ps, d, d);
        e.un(op::cvtdq2ps, lo, lo);
        e.fma(FmaKind::fmadd, FmaForm::f132, false, false, d, lo, Opnd::at(e.ksplat(32, 0x47800000u)));   // d * 2^16 + lo
        return true;
    }
    const Xmm T0 = e.tmp(), T1 = e.tmp();
    if (!s.q) {
        e.movq(T1, x);
        x = T1;
    }
    Xmm r;
    if (!in.is_unsigned) {
        r = T0;
        e.un(op::cvtdq2ps, r, x);
    } else {
        // hi = (x >> 16) << 15 converts exactly; doubled it is x & 0xffff0000.
        // lo = x & 0xffff. hi + lo rounds once.
        const Xmm h = T0, l = e.tmp();
        e.psrld(h, x, 16);
        e.pslld(h, h, 15);
        if (e.avx()) {
            e.op(op::pand, l, x, Opnd::at(e.ksplat(32, 0xffff)));
        } else {
            e.pslld(l, x, 16);
            e.psrld(l, l, 16);
        }
        e.un(op::cvtdq2ps, h, h);
        e.un(op::cvtdq2ps, l, l);
        e.op(op::addps, h, h, h);
        e.op(op::addps, h, h, l);
        r = h;
    }
    if (in.fbits) {
        const Xmm c = T1;
        e.splat(32, c, pow2_bits(32, -static_cast<int>(in.fbits)));
        e.op(op::mulps, r, r, c);
    }
    e.commit(in.rd, r, w);
    return true;
}

// FJCVTZS Wd, Dn: truncate toward zero, wrap modulo 2^32; NZCV = 0Z00 with
// Z set iff the conversion was exact and in int32 range (and not -0).
bool emit_fjcvtzs(FpEmit& e, const Inst& in) {
    auto& a = e.as;
    const Xmm x = e.vec(in.rn);
    const Xmm T0 = e.tmp(), T1 = e.tmp();
    const Gp gx = e.gtmp(), t = e.gtmp(), z = e.gtmp();
    e.const_gpr();
    const Gp r = e.gdest(in.rd);
    const Label big = a.new_label(), raise = a.new_label(), done = a.new_label();
    e.ctx.clobber_host_flags();
    e.movq(gx, x);
    a.cvttsd2si(r, x, 64);
    a.movsx(64, t, r, 32);
    a.cmp(64, t, r);
    a.jcc(Cond::ne, big);
    // In range: Z = (double)result has the bits of x (excludes -0, inexact,
    // and denormals flushed by FZ).
    e.zero(T0);
    e.cvtsi2s(true, T0, r, 64);
    e.movq(t, T0);
    a.xor_(32, z, z);
    a.cmp(64, t, gx);
    a.setcc(Cond::e, z);
    a.shl(32, z, 30);
    a.jmp(done);
    a.bind(big);
    a.mov_imm(z, 0);
    a.cmp(64, r, 1);
    a.jcc(Cond::no, raise);   // |x| < 2^63: the 64-bit truncation is exact
    // |x| >= 2^63, inf or NaN: x - 2^32 * trunc(x / 2^32) is exact for a
    // finite x and NaN otherwise (converting to 0 below).
    e.scalar_const(64, T0, 0x3df0000000000000ULL);   // 2^-32
    e.op(op::mulsd, T1, x, T0);
    e.sc_i(op::roundsd, T1, T1, 0x0b);
    e.scalar_const(64, T0, 0x41f0000000000000ULL);   // 2^32
    e.op(op::mulsd, T1, T1, T0);
    e.op(op::subsd, T0, x, T1);
    a.cvttsd2si(r, T0, 64);
    a.bind(raise);
    e.ones(T1);
    e.ucomi(false, T1, T1, true);   // IOC: out of int32 range
    a.bind(done);
    a.mov(32, r, r);
    e.ctx.set_nzcv(z);
    return true;
}

} // namespace

// Round-to-odd narrowing: r = cvtpd2ps/cvtsd2ss(x) under the current
// rounding, then step back toward zero if it rounded away and set the low
// bit if inexact. Rounding r to fewer than 23 bits afterwards is a single
// correct rounding of x.
void emit_narrow_round_odd(FpEmit& e, bool packed, Xmm out, Xmm x, Xmm b, Xmm m) {
    if (packed) {
        e.un(op::cvtpd2ps, out, x);
        e.un(op::cvtps2pd, b, out);
    } else {
        e.sc(op::cvtsd2ss, out, x);
        e.sc(op::cvtss2sd, b, out);
    }
    // |b| > |x| (same signs): signed 64-bit compare of the bit patterns.
    e.op(op::pcmpgtq, m, b, x);
    if (packed) e.un_i(op::pshufd, m, m, 0x08);
    e.op(op::paddd, out, out, m);                 // -1 where rounded away
    e.op_i(packed ? op::cmppd : op::cmpsd, m, b, x, pred_neq);   // inexact (or NaN)
    if (packed) e.un_i(op::pshufd, m, m, 0x08);
    e.psrld(m, m, 31);
    e.op(op::por, out, out, m);
}

bool emit_convert(FpEmit& e, const Inst& in) {
    const Shape s = in.sh;
    switch (in.k) {
    case K::cvt_f2i:
        if (s.scalar) return f2i_scalar(e, in);
        if (s.es == 16) return f2i_vector_h(e, in);
        return f2i_vector(e, in);
    case K::cvt_i2f:
        if (s.scalar) return i2f_scalar(e, in);
        if (s.es == 16) return i2f_vector_h(e, in);
        return i2f_vector(e, in);
    case K::fcvt: {
        const unsigned from = s.es, to = in.dst_es;
        if (e.avx() && ((from == 32 && to == 64) || (from == 64 && to == 32))) {
            // d[0] = cvt(n[0]); the upper lanes come from the first source:
            // n itself when it has zeros above the result, else a zeroed temp.
            const Opnd n = e.src(in.rn);
            Xmm upper;
            if (!n.is_mem && e.zero_above(in.rn, to)) {
                upper = n.reg;
            } else {
                upper = e.tmp();
                e.zero(upper);
            }
            const Xmm d = e.dest(in.rd);
            e.sc_u(to == 64 ? op::cvtss2sd : op::cvtsd2ss, d, upper, n);
            return true;
        }
        const Xmm n = e.vec(in.rn);
        if (from == 32 && to == 64) {
            const Xmm d = e.dest(in.rd);
            e.sc(op::cvtss2sd, d, n);
            e.zero_upper(d, 64);
        } else if (from == 64 && to == 32) {
            const Xmm d = e.dest(in.rd);
            e.sc(op::cvtsd2ss, d, n);
            e.zero_upper(d, 32);
        } else if (from == 16 && to == 32) {
            const Xmm d = e.dest(in.rd);
            e.cvtph2ps(d, n);
            e.zero_upper(d, 32);
        } else if (from == 16 && to == 64) {
            const Xmm t = e.tmp();
            e.cvtph2ps(t, n);
            const Xmm d = e.dest(in.rd);
            e.sc(op::cvtss2sd, d, t);
            e.zero_upper(d, 64);
        } else if (from == 32 && to == 16) {
            const Xmm t = e.tmp();
            e.ins0(t, n);
            const Xmm d = e.dest(in.rd);
            e.cvtps2ph(d, t);
        } else if (from == 64 && to == 16) {
            // FTZ would flush the single intermediate of a normal double below
            // 2^-126, but FZ does not apply to a half result: any such value
            // rounds to half like +-2^-100, which is a normal single.
            auto& a = e.as;
            const Xmm t = e.tmp(), b = e.tmp(), m = e.tmp(), w = e.tmp();
            const Gp g = e.gtmp(), h = e.gtmp(), u = e.gtmp(), c = e.gtmp();
            e.ctx.clobber_host_flags();
            e.movq(g, n);
            a.mov(64, h, g);
            a.btr(64, h, 63);                          // |x|
            a.mov(64, u, h);
            a.mov_imm(c, 0x0010000000000000ULL);       // smallest normal double
            a.sub(64, u, c);
            a.mov_imm(c, 0x3800000000000000ULL);       // 2^-126 - smallest normal
            a.cmp(64, u, c);
            a.mov_imm(c, 0x39b0000000000000ULL);       // 2^-100
            a.cmov(Cond::b, 64, h, c);
            a.mov(64, u, g);
            a.shr(64, u, 63);
            a.shl(64, u, 63);
            a.or_(64, h, u);
            e.movq(m, h);
            emit_narrow_round_odd(e, false, t, m, b, w);
            e.ins0(t, t);
            const Xmm d = e.dest(in.rd);
            e.cvtps2ph(d, t);
        } else {
            return false;
        }
        return true;
    }
    case K::fcvtl: {   // widen (lower or upper half)
        const Xmm n = e.vec(in.rn);
        Xmm src = n;
        if (in.upper) {
            src = e.tmp();
            e.un_i(op::pshufd, src, n, 0xee);
        }
        const Xmm d = e.dest(in.rd);
        if (s.es == 32) e.un(op::cvtps2pd, d, src);
        else e.cvtph2ps(d, src);
        return true;
    }
    case K::fcvtn: case K::fcvtxn: {
        const Xmm n = e.vec(in.rn);
        const Xmm acc = in.upper ? e.vec(in.rd) : Xmm{};
        const Xmm t = e.tmp();
        if (in.k == K::fcvtxn) {
            const Xmm b = e.tmp(), m = e.tmp();
            // With FZ, x64 flushes a tiny result to zero before the sticky
            // bit is set; multiplying by 1.0 re-flushes exactly when DAZ is on.
            if (s.scalar) {
                emit_narrow_round_odd(e, false, t, n, b, m);
                e.scalar_const(32, b, 0x3f800000u);
                e.op(op::mulss, t, t, b);
                e.commit(in.rd, t, 32);
                return true;
            }
            emit_narrow_round_odd(e, true, t, n, b, m);
            e.movq(t, t);
            e.splat(32, b, 0x3f800000u);
            e.op(op::mulps, t, t, b);
        } else if (s.es == 64) {
            e.un(op::cvtpd2ps, t, n);
        } else {
            e.cvtps2ph(t, n);
        }
        const Xmm d = e.dest(in.rd);
        if (in.upper) e.op(op::movlhps, d, acc, t);
        else e.mov(d, t);
        return true;
    }
    case K::fjcvtzs:
        return emit_fjcvtzs(e, in);
    default:
        return false;
    }
}

} // namespace transmute::fx::fpx
