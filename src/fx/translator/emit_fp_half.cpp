// fp family: half-precision (FEAT_FP16) through F16C (owner: fp).
//
// Halves are widened to singles exactly (vcvtph2ps), computed in single
// precision and narrowed with vcvtps2ph under MXCSR.RC. For + - * / and sqrt
// the double rounding is innocuous (24 >= 2*11 + 2); min/max/compare/round
// are exact. Fused operations go through double precision (the product is
// exact and, as analysed in NOTES_FP.md, so is the sum whenever it matters)
// and a round-to-odd narrowing to single, which makes the final rounding to
// half a single correct rounding. F16C implies AVX, so only VEX forms are
// used here. FPCR.FZ16 and FPCR.AHP are not modelled.
#include "fx/translator/emit_fp_internal.hpp"

namespace transmute::fx::fpx {

namespace {

const Shape kPS{32, false, true};
const Shape kSS{32, true, false};

// out = the four halves of 64-bit group g of src, as singles.
void widen(FpEmit& e, Xmm out, Xmm src, unsigned g) {
    if (g == 0) {
        e.cvtph2ps(out, src);
    } else {
        e.un_i(op::pshufd, out, src, 0xee);
        e.cvtph2ps(out, out);
    }
}

// Lane `index` of the halves of m broadcast as singles (vector) or in lane 0.
void widen_element(FpEmit& e, Xmm out, Xmm m, unsigned index, bool scalar) {
    widen(e, out, m, index / 4);
    const unsigned lane = index % 4;
    if (scalar && lane == 0) return;
    e.as.vex_rr(vop::vpermilps, false, x64::id(out), 0, x64::id(out));
    e.as.db(static_cast<std::uint8_t>(scalar ? lane : lane * 0x55));
}

void halves_mask(FpEmit& e, Xmm m, bool sign) {
    e.ones(m);
    e.shift(0x71, sign ? 6 : 2, m, m, sign ? 15 : 1);   // psllw 15 / psrlw 1
}

enum class Narrow { half, mask, half_abs, half_neg };

// t (singles) -> halves in the low 64 bits of out; scratch u.
void narrow(FpEmit& e, Narrow how, bool scalar, Xmm out, Xmm t, Xmm u) {
    if (how == Narrow::mask) {
        e.op(op::packssdw, out, t, t);
        return;
    }
    if (scalar) e.ins0(t, t);   // no spurious exceptions from other lanes
    e.cvtps2ph(out, t);
    if (how == Narrow::half_abs) {
        halves_mask(e, u, false);
        e.op(op::andps, out, out, u);
    } else if (how == Narrow::half_neg) {
        halves_mask(e, u, true);
        e.op(op::xorps, out, out, u);
    }
}

// Writes the halves in the low 64 bits of h into group g of d.
void place(FpEmit& e, Xmm d, Xmm h, unsigned g, unsigned groups) {
    if (groups == 1) e.movq(d, h);
    else if (g == 0) e.op(op::movsd, d, d, h);
    else e.op(op::movlhps, d, d, h);
}

using Kernel = void (*)(FpEmit& e, const Inst& in, Shape s, Xmm out, Xmm a, Xmm b, Xmm u);

void k_arith(FpEmit& e, const Inst& in, Shape s, Xmm out, Xmm a, Xmm b, Xmm) {
    const FOps& f = in.k == K::fadd ? fops::add : in.k == K::fsub ? fops::sub
                  : in.k == K::fdiv ? fops::div : in.k == K::fabd ? fops::sub : fops::mul;
    e.op(s.pick(f), out, a, b);
}
void k_minmax(FpEmit& e, const Inst& in, Shape s, Xmm out, Xmm a, Xmm b, Xmm u) {
    emit_minmax_scratch(e, in.k, s, out, a, b, u);
}
void k_compare(FpEmit& e, const Inst& in, Shape s, Xmm out, Xmm a, Xmm b, Xmm u) {
    if (in.k == K::facge || in.k == K::facgt) {
        e.abs_mask(32, u);
        e.op(op::andps, a, a, u);
        e.op(op::andps, b, b, u);
    }
    const auto pred = in.k == K::fcmeq ? pred_eq
                    : (in.k == K::fcmge || in.k == K::facge) ? pred_ge_os : pred_gt_os;
    e.op_i(s.pick(fops::cmp), out, a, b, pred);
}
void k_compare0(FpEmit& e, const Inst& in, Shape s, Xmm out, Xmm a, Xmm b, Xmm) {
    e.zero(b);
    Pred pred;
    switch (in.k) {
    case K::fcmeq0: pred = pred_eq; break;
    case K::fcmge0: pred = pred_ge_os; break;
    case K::fcmgt0: pred = pred_gt_os; break;
    case K::fcmle0: pred = pred_le; break;
    default: pred = pred_lt; break;
    }
    e.op_i(s.pick(fops::cmp), out, a, b, pred);
}
void k_sqrt(FpEmit& e, const Inst&, Shape s, Xmm out, Xmm a, Xmm, Xmm) {
    if (s.scalar) e.sc(op::sqrtss, out, a);
    else e.un(op::sqrtps, out, a);
}

// Two-operand (or one-operand, `unary`) driver over 4-half groups.
bool h_map(FpEmit& e, const Inst& in, Kernel k, Narrow how, bool unary) {
    const Shape s = in.sh;
    const Shape fs = s.scalar ? kSS : kPS;
    const unsigned groups = (!s.scalar && s.q) ? 2 : 1;
    const Xmm n = e.vec(in.rn);
    const Xmm m = unary ? n : e.vec(in.rm);
    const Xmm T0 = e.tmp(), T1 = e.tmp(), T2 = e.tmp(), T3 = e.tmp();
    if (in.index >= 0) widen_element(e, T1, m, static_cast<unsigned>(in.index), s.scalar);
    const Xmm d = e.dest(in.rd);
    for (unsigned g = 0; g < groups; ++g) {
        widen(e, T0, n, g);
        if (!unary && in.index < 0) widen(e, T1, m, g);
        k(e, in, fs, T2, T0, T1, T3);
        narrow(e, how, s.scalar, T0, T2, T3);
        if (s.scalar) {
            e.commit(in.rd, T0, 16);
            return true;
        }
        place(e, d, T0, g, groups);
    }
    return true;
}

// Pairwise (vector) and scalar pairwise / reductions.
bool h_pairwise(FpEmit& e, const Inst& in) {
    const Shape s = in.sh;
    const bool add = in.k == K::faddp;
    const Xmm T0 = e.tmp(), T1 = e.tmp(), T2 = e.tmp(), T3 = e.tmp();
    const auto combine = [&](Xmm out, Xmm a, Xmm b, Shape fs) {   // out = op(a, b); a, b clobbered
        if (add) e.op(fs.pick(fops::add), out, a, b);
        else emit_minmax_scratch(e, in.k, fs, out, a, b, T3);
    };
    if (s.scalar) {   // op(n0, n1)
        const Xmm n = e.vec(in.rn);
        widen(e, T0, n, 0);
        e.un(op::movshdup, T1, T0);
        combine(T2, T0, T1, kSS);
        narrow(e, Narrow::half, true, T0, T2, T3);
        e.commit(in.rd, T0, 16);
        return true;
    }
    const Xmm n = e.vec(in.rn), m = e.vec(in.rm);
    const Xmm d = e.dest(in.rd);
    if (!s.q) {   // [op(n0,n1) op(n2,n3) op(m0,m1) op(m2,m3)]
        widen(e, T0, n, 0);
        widen(e, T1, m, 0);
        e.op_i(op::shufps, T2, T0, T1, 0x88);
        e.op_i(op::shufps, T1, T0, T1, 0xdd);
        combine(T0, T2, T1, kPS);
        narrow(e, Narrow::half, false, T1, T0, T3);
        e.movq(d, T1);
        return true;
    }
    // Group 0 from n's eight lanes, group 1 from m's. Each group reads all of
    // its source, so the group whose source aliases Vd is computed first; if
    // both alias, the two groups are equal.
    if (in.rn == in.rd && in.rm == in.rd) {
        widen(e, T0, n, 0);
        widen(e, T1, n, 1);
        e.op_i(op::shufps, T2, T0, T1, 0x88);
        e.op_i(op::shufps, T1, T0, T1, 0xdd);
        combine(T0, T2, T1, kPS);
        narrow(e, Narrow::half, false, T1, T0, T3);
        e.op(op::punpcklqdq, d, T1, T1);
        return true;
    }
    const bool m_first = in.rm == in.rd;
    for (unsigned step = 0; step < 2; ++step) {
        const unsigned g = m_first ? 1 - step : step;
        const Xmm src = g == 0 ? n : m;
        widen(e, T0, src, 0);
        widen(e, T1, src, 1);
        e.op_i(op::shufps, T2, T0, T1, 0x88);
        e.op_i(op::shufps, T1, T0, T1, 0xdd);
        combine(T0, T2, T1, kPS);
        narrow(e, Narrow::half, false, T1, T0, T3);
        place(e, d, T1, g, 2);
    }
    return true;
}

bool h_reduce(FpEmit& e, const Inst& in) {
    const Shape s = in.sh;
    const Xmm n = e.vec(in.rn);
    const Xmm T0 = e.tmp(), T1 = e.tmp(), T2 = e.tmp(), T3 = e.tmp();
    widen(e, T0, n, 0);
    if (s.q) {   // op over the two groups first (order does not change the value)
        widen(e, T1, n, 1);
        emit_minmax_scratch(e, in.k, kPS, T2, T0, T1, T3);
        e.mov(T0, T2);
    }
    e.un(op::movshdup, T1, T0);
    emit_minmax_scratch(e, in.k, kPS, T2, T0, T1, T3);   // lanes 0, 2
    e.un_i(op::pshufd, T1, T2, 0xee);
    emit_minmax_scratch(e, in.k, kSS, T0, T2, T1, T3);
    narrow(e, Narrow::half, true, T1, T0, T3);
    e.commit(in.rd, T1, 16);
    return true;
}

// Fused multiply-add on half lanes through double precision.
//   acc + n * m (fmadd / fnmadd kinds as for S/D), one pair of lanes at a time.
void h_fma_pair(FpEmit& e, FmaKind kind, Xmm out_h, Xmm acc, Xmm n, Xmm m, unsigned pair, int element,
                const Xmm (&T)[4]) {
    const auto load = [&](Xmm t, Xmm src, unsigned dword) {
        e.un_i(op::pshufd, t, src, static_cast<std::uint8_t>(dword));
        e.cvtph2ps(t, t);
        e.un(op::cvtps2pd, t, t);
    };
    load(T[0], acc, pair);
    load(T[1], n, pair);
    if (element >= 0) {
        e.un_i(op::pshufd, T[2], m, static_cast<std::uint8_t>(element / 2));
        e.cvtph2ps(T[2], T[2]);
        e.as.vex_rr(vop::vpermilps, false, x64::id(T[2]), 0, x64::id(T[2]));
        e.as.db(static_cast<std::uint8_t>((element & 1) * 0x55));
        e.un(op::cvtps2pd, T[2], T[2]);
    } else {
        load(T[2], m, pair);
    }
    e.fma(kind, FmaForm::f231, true, false, T[0], T[1], T[2]);
    emit_narrow_round_odd(e, true, T[1], T[0], T[2], T[3]);
    e.cvtps2ph(out_h, T[1]);
}

} // namespace

bool emit_arith_h(FpEmit& e, const Inst& in) {
    switch (in.k) {
    case K::fadd: case K::fsub: case K::fmul: case K::fdiv:
        return h_map(e, in, k_arith, Narrow::half, false);
    case K::fabd:
        return h_map(e, in, k_arith, Narrow::half_abs, false);
    case K::fnmul:
        return h_map(e, in, k_arith, Narrow::half_neg, false);
    case K::fmax: case K::fmin: case K::fmaxnm: case K::fminnm:
        return h_map(e, in, k_minmax, Narrow::half, false);
    case K::fcmeq: case K::fcmge: case K::fcmgt: case K::facge: case K::facgt:
        return h_map(e, in, k_compare, Narrow::mask, false);
    case K::fcmeq0: case K::fcmge0: case K::fcmgt0: case K::fcmle0: case K::fcmlt0:
        return h_map(e, in, k_compare0, Narrow::mask, true);
    case K::faddp: case K::fmaxp: case K::fminp: case K::fmaxnmp: case K::fminnmp:
        return h_pairwise(e, in);
    case K::fmaxv: case K::fminv: case K::fmaxnmv: case K::fminnmv:
        return h_reduce(e, in);
    default:
        return false;   // FMULX (H) is left to the interpreter
    }
}

bool emit_fused_h(FpEmit& e, const Inst& in) {
    const Shape s = in.sh;
    const Xmm T[4] = {e.tmp(), e.tmp(), e.tmp(), e.tmp()};
    if (in.k == K::fmla || in.k == K::fmls) {
        const FmaKind kind = in.k == K::fmla ? FmaKind::fmadd : FmaKind::fnmadd;
        const Xmm acc = e.vec(in.rd), n = e.vec(in.rn), m = e.vec(in.rm);
        if (s.scalar) {
            h_fma_pair(e, kind, T[3], acc, n, m, 0, in.index, T);
            e.commit(in.rd, T[3], 16);
            return true;
        }
        const unsigned pairs = s.q ? 4 : 2;
        const Xmm d = e.dest(in.rd);
        // Results are inserted pair by pair into d (which may alias a source):
        // a pair's lanes are read before they are written, and the element's
        // pair (if m aliases d) is processed last.
        const int last = (in.index >= 0 && in.rm == in.rd) ? in.index / 2 : -1;
        unsigned order[4];
        unsigned count = 0;
        for (unsigned p = 0; p < pairs; ++p)
            if (static_cast<int>(p) != last) order[count++] = p;
        if (last >= 0) order[count++] = static_cast<unsigned>(last);
        for (unsigned step = 0; step < pairs; ++step) {
            const unsigned p = order[step];
            h_fma_pair(e, kind, T[3], acc, n, m, p, in.index, T);
            e.op_i(op::insertps, d, d, T[3], static_cast<std::uint8_t>(p << 4));
        }
        if (!s.q) e.movq(d, d);
        return true;
    }
    // FMADD/FMSUB/FNMADD/FNMSUB Hd, Hn, Hm, Ha
    FmaKind kind;
    switch (in.k) {
    case K::fmadd: kind = FmaKind::fmadd; break;
    case K::fmsub: kind = FmaKind::fnmadd; break;
    case K::fnmadd: kind = FmaKind::fnmsub; break;
    default: kind = FmaKind::fmsub; break;
    }
    const Xmm a = e.vec(in.ra), n = e.vec(in.rn), m = e.vec(in.rm);
    h_fma_pair(e, kind, T[3], a, n, m, 0, -1, T);
    e.commit(in.rd, T[3], 16);
    return true;
}

bool emit_unary_h(FpEmit& e, const Inst& in) {
    switch (in.k) {
    case K::fsqrt:
        return h_map(e, in, k_sqrt, Narrow::half, true);
    case K::frint: {
        const Shape s = in.sh;
        const Shape fs = s.scalar ? kSS : kPS;
        const unsigned groups = (!s.scalar && s.q) ? 2 : 1;
        const Xmm n = e.vec(in.rn);
        const Xmm T[4] = {e.tmp(), e.tmp(), e.tmp(), e.tmp()};
        const Xmm d = e.dest(in.rd);
        static constexpr std::uint8_t kImm[] = {0x08, 0x0a, 0x09, 0x0b, 0, 0x04, 0x0c};   // n p m z a x i
        for (unsigned g = 0; g < groups; ++g) {
            widen(e, T[3], n, g);
            if (in.rnd == Rnd::a) emit_round_away(e, fs, T[2], T[3], false, T);
            else e.round(fs, T[2], T[3], kImm[static_cast<unsigned>(in.rnd)]);
            narrow(e, Narrow::half, s.scalar, T[0], T[2], T[1]);
            if (s.scalar) {
                e.commit(in.rd, T[0], 16);
                return true;
            }
            place(e, d, T[0], g, groups);
        }
        return true;
    }
    default:
        return false;
    }
}

} // namespace transmute::fx::fpx
