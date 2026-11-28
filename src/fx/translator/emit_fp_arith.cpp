// fp family: arithmetic, min/max, compares, pairwise, reductions, fused
// multiply-add, unary operations, selects and moves (owner: fp).
#include "fx/translator/emit_fp_internal.hpp"
#include "fx/common/cpu_features.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <span>

namespace transmute::fx::fpx {

namespace {

K base_minmax(K k) {
    switch (k) {
    case K::fmaxp: case K::fmaxv: return K::fmax;
    case K::fminp: case K::fminv: return K::fmin;
    case K::fmaxnmp: case K::fmaxnmv: return K::fmaxnm;
    case K::fminnmp: case K::fminnmv: return K::fminnm;
    default: return k;
    }
}

bool is_nm(K k) { return k == K::fmaxnm || k == K::fminnm; }

std::uint64_t abs_bits(unsigned es) {
    return es == 64 ? 0x7fffffffffffffffULL : es == 32 ? 0x7fffffffULL : 0x7fffULL;
}

std::uint8_t frint_imm(Rnd r) {
    switch (r) {
    case Rnd::n: return 0x08;
    case Rnd::m: return 0x09;
    case Rnd::p: return 0x0a;
    case Rnd::z: return 0x0b;
    case Rnd::x: return 0x04;
    case Rnd::i: return 0x0c;
    default: return 0x0b;
    }
}

} // namespace

Xmm element(FpEmit& e, Shape s, unsigned rm, int index) {
    const Xmm m = e.vec(rm);
    if (s.scalar) {
        if (index == 0) return m;
        // S and D: the element alone, upper lanes zero (a zero-upper first
        // source for scalar ops).
        const Xmm t = e.tmp();
        if (s.es == 64) e.psrldq(t, m, 8);
        else if (s.es == 32) e.op_i(op::insertps, t, m, m, static_cast<std::uint8_t>((index << 6) | 0x0e));
        else e.psrldq(t, m, static_cast<std::uint8_t>(index * 2));
        return t;
    }
    const Xmm t = e.tmp();
    if (s.es == 64) {
        if (index == 0) e.un(op::movddup, t, m);
        else e.un_i(op::pshufd, t, m, 0xee);
    } else {
        const auto imm = static_cast<std::uint8_t>(index * 0x55);
        if (e.avx()) {
            e.as.vex_rr(vop::vpermilps, false, x64::id(t), 0, x64::id(m));
            e.as.db(imm);
        } else {
            e.un_i(op::pshufd, t, m, imm);
        }
    }
    return t;
}

Opnd element_op(FpEmit& e, Shape s, unsigned rm, int index) {
    // Tier 2: precomputed broadcast of a loop-invariant or 3+-use element
    // (fp_family_facts / EmitContext::vec_derived): direct memory operand in
    // FMUL/FMLA/FMLS with zero Port-5 shuffles and zero scratch XMM pressure.
    Mem slot;
    if (e.avx() && !s.scalar && s.q && (s.es == 32 || s.es == 64) && fp_on(fp_no_slot)) {
        const unsigned dkind = (s.es == 32 ? 24u : 28u) + static_cast<unsigned>(index);
        if (e.ctx.vec_derived(rm, dkind, slot)) return Opnd::at(slot);
    }
    // An element other than a scalar's lane 0 comes straight from a current
    // context slot (one load: movss/movsd/vbroadcastss/movddup) even when a
    // clean copy is in a register (one shuffle saved; EmitContext::vec_slot).
    const bool from_slot = e.avx() && fp_on(fp_no_slot) && (!s.scalar || index > 0) && e.ctx.vec_slot(rm, slot);
    const Opnd m = from_slot ? Opnd::at(slot) : e.src(rm);
    if (!m.is_mem) {
        Opnd r = Opnd::of(element(e, s, rm, index));
        r.zero_upper = s.scalar && index > 0;   // see element()
        return r;
    }
    Mem at = m.mem;
    at.disp += static_cast<std::int32_t>(s.es / 8 * static_cast<unsigned>(index));
    if (s.scalar) return Opnd::at(at);   // ss/sd forms read only the element
    const Xmm t = e.tmp();
    if (s.es == 64) e.un(op::movddup, t, Opnd::at(at));
    else if (e.avx()) e.as.vex_rm(vop::vbroadcastss, false, x64::id(t), 0, at);
    else e.un_i(op::pshufd, t, m, static_cast<std::uint8_t>(index * 0x55));   // slots are 16-byte aligned
    return Opnd::of(t);
}

Xmm load_opnd(FpEmit& e, const Opnd& o, Shape s) {
    if (!o.is_mem) return o.reg;
    const Xmm t = e.tmp();
    if (s.scalar && s.es == 64) e.un(op::movsd, t, o);
    else if (s.scalar && s.es == 32) e.un(op::movss, t, o);
    else e.un(op::movups, t, o);
    return t;
}

void guest_binary(FpEmit& e, const Inst& in, SseOp o, bool commutative, int imm, bool zero_safe) {
    const Shape s = in.sh;
    const unsigned w = s.width();
    const Opnd n = e.src(in.rn);
    const Opnd m = in.index >= 0 ? element_op(e, s, in.rm, in.index) : e.src(in.rm);
    // Guest register behind each operand when it is that register itself.
    const int ng = in.rn;
    const int mg = in.index < 0 || (s.scalar && in.index == 0) ? in.rm : -1;
    // A scalar result needs no zeroing when its first source has zeros above
    // the scalar: a register known to, or a memory operand loaded with
    // movss/movsd. Prefer an operand already in a register as the first.
    // (Vector shapes compute every lane: a 64-bit result is always zeroed.)
    const auto zero_first = [&](const Opnd& x, int g) {
        return s.scalar && (x.is_mem || x.zero_upper || (g >= 0 && e.zero_above(static_cast<unsigned>(g), w)));
    };
    // A load costs an instruction; zeroing costs one on the dependency chain.
    const auto cost = [&](const Opnd& x, int g) {
        return (x.is_mem ? 2 : 0) + (s.scalar && !zero_first(x, g) ? 3 : 0);
    };
    const bool swap = commutative && e.avx() && cost(m, mg) < cost(n, ng);
    const Opnd& a = swap ? m : n;
    const Opnd& b = swap ? n : m;
    // 64-bit vectors: when both sources are zero above bit 64 and 0 OP 0 is
    // +0 without exceptions (FADD, FMUL), the computed upper lanes are zero.
    const bool vec_zero = !s.scalar && w == 64 && zero_safe && in.index < 0 && e.zero_above(in.rn, 64) &&
                          e.zero_above(in.rm, 64);
    const bool zero = w == 128 || vec_zero || (e.avx() && zero_first(a, swap ? mg : ng));
    const Xmm ar = load_opnd(e, a, s);
    const Xmm d = e.dest(in.rd);
    if (e.avx() || b.is_mem || d != b.reg || d == ar) {
        if (imm >= 0) e.op_i(o, d, ar, b, static_cast<std::uint8_t>(imm));
        else e.op(o, d, ar, b);
    } else if (commutative) {   // SSE, d aliases b
        if (imm >= 0) e.op_i(o, d, d, Opnd::of(ar), static_cast<std::uint8_t>(imm));
        else e.op(o, d, d, ar);
    } else {
        const Xmm t = e.tmp();
        if (imm >= 0) e.op_i(o, t, ar, b, static_cast<std::uint8_t>(imm));
        else e.op(o, t, ar, b);
        e.mov(d, t);
    }
    if (!zero) e.zero_upper(d, w);
}

void final_op(FpEmit& e, unsigned rd, unsigned width, SseOp o, Xmm a, Xmm b, bool commutative, int imm) {
    const auto emit = [&](Xmm d, Xmm x, Xmm y) {
        if (imm >= 0) e.op_i(o, d, x, y, static_cast<std::uint8_t>(imm));
        else e.op(o, d, x, y);
    };
    if (e.avx()) {
        const Xmm d = e.dest(rd);
        emit(d, a, b);
        e.zero_upper(d, width);
        return;
    }
    const Xmm d = e.dest(rd);
    if (d == a || d != b) {
        emit(d, a, b);
    } else if (commutative) {
        emit(d, b, a);
    } else {
        const Xmm t = e.tmp();
        emit(t, a, b);
        e.mov(d, t);
    }
    e.zero_upper(d, width);
}

MinMaxTemps minmax_temps(FpEmit& e, K kind) {
    MinMaxTemps t;
    t.u = e.tmp();
    t.v = base_minmax(kind) == K::fmin ? t.u : e.tmp();
    return t;
}

bool minmax_may_alias(const FpEmit& e, K kind) { return e.avx() && base_minmax(kind) != K::fminnm; }

void emit_minmax(FpEmit& e, K kind, Shape s, Xmm out, Xmm a, Xmm b, Xmm u, Xmm v) {
    kind = base_minmax(kind);
    const SseOp max = s.pick(fops::max), min = s.pick(fops::min), cmp = s.pick(fops::cmp);
    const SseOp and_ = s.pick(fops::and_), or_ = s.pick(fops::or_), andn = s.pick(fops::andn);
    // x64 min/max return the second operand (DAZ-flushed) on NaN or
    // equality. Computing both operand orders: t1 = op(a, b), t2 = op(b, a).
    // Without NaNs t1 == t2 except for +-0, where max wants AND (+0 wins) and
    // min wants OR (-0 wins). With one NaN operand one of t1/t2 is that NaN
    // and the other the flushed number. t1 is computed last so that `out`
    // may alias a or b (except FMINNM).
    switch (kind) {
    case K::fmax:   // (t1 & t2) | unord(a, b)
        e.op(max, u, b, a);
        e.op_i(cmp, v, a, b, pred_unord);
        e.op(max, out, a, b);
        e.op(and_, out, out, u);
        e.op(or_, out, out, v);
        return;
    case K::fmin:   // t1 | t2 (a NaN stays a NaN under OR)
        e.op(min, u, b, a);
        e.op(min, out, a, b);
        e.op(or_, out, out, u);
        return;
    case K::fmaxnm:   // (t1 | nan(b)) & (t2 | nan(a)): a NaN lane takes the other operand
        e.op(max, u, b, a);                   // t2 (= a where either is NaN)
        e.op_i(cmp, v, a, a, pred_unord);
        e.op(or_, u, u, v);
        e.op_i(cmp, v, b, b, pred_unord);
        e.op(max, out, a, b);                 // t1 (= b where either is NaN)
        e.op(or_, out, out, v);
        e.op(and_, out, out, u);
        return;
    default:   // fminnm: (t1 & ~(nan(b) & ~nan(a))) | (t2 & ~nan(a)); out != a, b
        // A NaN lane of t1 (b NaN) is cleared unless a is NaN too; t2's NaN
        // lanes (a NaN) are cleared; both NaN keeps t1's NaN.
        if (e.avx()) {
            e.op(min, out, a, b);             // t1 (= b where either is NaN)
            e.op_i(cmp, u, b, b, pred_unord);
            e.op_i(cmp, v, a, a, pred_unord);
            e.op(andn, u, v, u);              // nan(b) & ~nan(a)
            e.op(andn, out, u, out);
            e.op(min, u, b, a);               // t2 (= a where either is NaN)
            e.op(andn, u, v, u);
            e.op(or_, out, out, u);
            return;
        }
        // SSE: andn only in place (d = ~d & s).
        e.op(min, out, a, b);
        e.op_i(cmp, u, a, a, pred_unord);
        e.op_i(cmp, v, b, b, pred_unord);
        e.op(andn, u, u, v);                  // nan(b) & ~nan(a)
        e.op(andn, u, u, out);                // t1 masked
        e.op_i(cmp, v, a, a, pred_unord);
        e.op(min, out, b, a);
        e.op(andn, v, v, out);                // t2 masked
        e.op(or_, out, u, v);
        return;
    }
}

// FMAX/FMIN/FMAXNM/FMINNM where one operand holds a constant known at
// translation time (EmitContext::vec_fp_const):
//  - scalar finite normal constant or FMAX(x,+0)/FMIN(x,-0)/FMAXNM(x,+0)/FMINNM(x,-0)
//    + quiet x (dead flags, cold defer): `ucomis x, x; jp cold/done; vmax/vmin d, x, c`
//    (2-3 insns hot, never feeds a quiet NaN to vmax/vmin so MXCSR.IE stays clear).
//  - finite normal or +-0.0 constant + ANY x (dead flags, cold defer):
//    check NaN on x alone -> 1 single x64 min/max (ordered so +-0 tie yields
//    A64's exact sign) -> deferred bit-exact FPProcessNaNs cold stub.
// TRANSMUTE_FX_FP=noconstminmax.
bool emit_minmax_const(FpEmit& e, const Inst& in, Shape s, unsigned w) {
    const unsigned es = s.es;
    const bool nm = in.k == K::fmaxnm || in.k == K::fminnm;
    const bool is_max = in.k == K::fmax || in.k == K::fmaxnm;
    const bool dbl = s.dbl();
    const std::uint64_t sign_bit = FpEmit::sign_bits(es);
    const auto normal = [es](std::uint64_t v) {
        const unsigned exp = es == 32 ? static_cast<unsigned>((v >> 23) & 0xffu) : static_cast<unsigned>((v >> 52) & 0x7ffu);
        return exp != 0 && exp != (es == 32 ? 0xffu : 0x7ffu);
    };
    const auto exact_zero = [is_max, sign_bit](std::uint64_t v) {
        return is_max ? (v == 0) : (v == sign_bit);
    };
    const auto any_zero = [sign_bit](std::uint64_t v) { return v == 0 || v == sign_bit; };
    const unsigned span = s.scalar ? es : w;
    enum class Match { none, one_insn, scalar_quiet_nan, deferred_single };
    std::uint64_t matched_c = 0;
    const auto classify = [&](unsigned c_reg, unsigned x_reg) -> Match {
        std::uint64_t c = 0;
        if (!e.ctx.vec_fp_const(c_reg, es, span, c)) return Match::none;
        const bool is_norm = normal(c);
        if (!is_norm && !any_zero(c)) return Match::none;
        matched_c = c;
        if (e.ctx.vec_fp_non_nan(x_reg, es)) return Match::one_insn;
        if (s.scalar && e.ctx.vec_fp_quiet(x_reg, es) && (is_norm || exact_zero(c)) &&
            e.ctx.can_defer_cold() && !e.ctx.host_flags_live())
            return Match::scalar_quiet_nan;
        if (fp_on(fp_old_minmax) && e.ctx.can_defer_cold() && !e.ctx.host_flags_live()) return Match::deferred_single;
        return Match::none;
    };
    unsigned cr = in.rm, xr = in.rn;
    Match match = classify(in.rm, in.rn);
    if (match != Match::one_insn && in.rn != in.rm) {
        const std::uint64_t first_c = matched_c;
        const Match alt = classify(in.rn, in.rm);
        if (alt != Match::none && (match == Match::none || static_cast<int>(alt) < static_cast<int>(match))) {
            match = alt;
            cr = in.rn;
            xr = in.rm;
        } else {
            matched_c = first_c;
        }
    }
    if (match == Match::none) return false;
    const Xmm x = e.vec(xr), cv = e.vec(cr);
    if (match == Match::one_insn) {
        // Both x and c are proven non-NaN: a single x64 vmax/vmin is 100%
        // bit-exact and never sets MXCSR.IE. Order (first, second) so a +-0 tie
        // returns A64's exact signed zero (x64 returns the 2nd operand on tie).
        const bool c_second = normal(matched_c) || exact_zero(matched_c);
        const bool zero = s.scalar ? e.zero_above(c_second ? xr : cr, w) : w == 128;
        const Xmm d = e.dest(in.rd);
        e.op(s.pick(is_max ? fops::max : fops::min), d, c_second ? x : cv, c_second ? cv : x);
        if (!zero) e.zero_upper(d, w);
        return true;
    }
    if (match == Match::scalar_quiet_nan) {
        e.ctx.clobber_host_flags();
        const Xmm nan_val = nm ? cv : x;
        const bool zero = e.zero_above(xr, w) && (!nm || e.zero_above(cr, w));
        const Xmm d = e.dest(in.rd);
        const Label done = e.as.new_label();
        e.ucomi(dbl, x, x, false);
        if (d == nan_val) {
            e.as.jcc_short(Cond::p, done);
            e.op(s.pick(is_max ? fops::max : fops::min), d, x, cv);
            e.as.bind(done);
        } else {
            const Label cold = e.as.new_label();
            e.as.jcc(Cond::p, cold);
            e.op(s.pick(is_max ? fops::max : fops::min), d, x, cv);
            e.as.bind(done);
            x64::Assembler* as = &e.as;
            const bool avx = e.avx();
            e.ctx.defer_cold([as, avx, d, nan_val, cold, done] {
                as->bind(cold);
                if (avx) as->vex_rr(vex_of(op::movaps), false, x64::id(d), 0, x64::id(nan_val));
                else as->movaps(d, nan_val);
                as->jmp(done);
            });
        }
        if (!zero) e.zero_upper(d, w);
        return true;
    }
    if (match == Match::deferred_single) {
        e.ctx.clobber_host_flags();
        const Xmm a = (xr == in.rn) ? x : cv;
        const Xmm b = (xr == in.rm) ? x : cv;
        const Xmm t0 = e.tmp();
        const Gp g = (!s.scalar && w == 64) ? e.gtmp() : Gp::none;
        const Xmm d = e.dest(in.rd);
        const SseOp mm = s.pick(is_max ? fops::max : fops::min), cmp = s.pick(fops::cmp);
        const SseOp and_ = s.pick(fops::and_), or_ = s.pick(fops::or_), andn = s.pick(fops::andn);
        const Label cold = e.as.new_label(), done = e.as.new_label();
        // c is known finite (never NaN): check NaN on x alone.
        if (s.scalar) {
            e.ucomi(dbl, x, x, false);
            e.as.jcc(Cond::p, cold);
        } else if (w == 128) {
            e.op_i(cmp, t0, x, x, pred_unord);
            e.as.vex_rr(vex_of(op::ptest), false, x64::id(t0), 0, x64::id(t0));
            e.as.jcc(Cond::ne, cold);
        } else {
            e.op_i(cmp, t0, x, x, pred_unord);
            e.movq(g, t0);
            e.as.test(64, g, g);
            e.as.jcc(Cond::ne, cold);
        }
        // x is guaranteed non-NaN here. Order (first, second) so that a +-0 tie
        // returns the A64-exact signed zero (x64 returns the 2nd operand on tie).
        const bool c_second = normal(matched_c) || exact_zero(matched_c);
        e.op(mm, d, c_second ? x : cv, c_second ? cv : x);
        e.as.bind(done);
        const bool zero = s.scalar ? (e.zero_above(in.rn, w) && e.zero_above(in.rm, w)) : w == 128;
        if (!zero) e.zero_upper(d, w);

        const Mem quiet = e.ksplat(es, dbl ? 0x0008000000000000ull : 0x00400000ull);
        const Mem one = nm ? e.ksplat(es, dbl ? 0x3ff0000000000000ull : 0x3f800000ull) : Mem{};
        const bool alias = d == a || d == b;
        std::uint32_t used = (1u << x64::id(a)) | (1u << x64::id(b)) | (1u << x64::id(d)) | (1u << x64::id(t0));
        const auto pick_free = [&]() {
            for (unsigned r = 0; r < 16; ++r) {
                if (!((used >> r) & 1u)) {
                    used |= 1u << r;
                    return x64::xmm(r);
                }
            }
            throw std::logic_error("fp: no free xmm for cold const minmax stub");
        };
        const Xmm t1 = pick_free(), t2 = pick_free();
        const Xmm T = alias ? pick_free() : d;
        const Xmm t3 = !s.scalar ? pick_free() : Xmm{};
        auto* const as = &e.as;
        const std::uint8_t shift = dbl ? 12 : 9;
        const SseOp mul_op = s.pick(fops::mul);

        e.ctx.defer_cold([=]() {
            const auto vspill = [](unsigned slot) {
                return x64::ptr(Gp::r15, ctx_disp(offsetof(GuestContext, vspill) + 16u * slot));
            };
            const auto vop_rr = [as](SseOp o, Xmm dst, Xmm s1, Xmm s2) {
                as->vex_rr(vex_of(o), false, x64::id(dst), x64::id(s1), x64::id(s2));
            };
            const auto vop_rm = [as](SseOp o, Xmm dst, Xmm s1, const Mem& s2) {
                as->vex_rm(vex_of(o), false, x64::id(dst), x64::id(s1), s2);
            };
            const auto vop_rri = [as](SseOp o, Xmm dst, Xmm s1, Xmm s2, std::uint8_t imm) {
                as->vex_rr(vex_of(o), false, x64::id(dst), x64::id(s1), x64::id(s2));
                as->db(imm);
            };
            const auto vshl = [as, dbl, shift](Xmm dst, Xmm src) {
                as->vex_rr(VexOp{1, 1, static_cast<std::uint8_t>(dbl ? 0x73 : 0x72), false}, false, 6, x64::id(dst),
                           x64::id(src));
                as->db(shift);
            };

            as->bind(cold);
            as->movups(vspill(28), t1);
            as->movups(vspill(29), t2);
            if (alias) as->movups(vspill(30), T);
            if (!s.scalar) as->movups(vspill(31), t3);

            vop_rri(cmp, t0, a, a, pred_unord);
            vop_rri(cmp, t1, b, b, pred_unord);
            vshl(t2, b);
            vop_rr(andn, t2, t2, t1);
            vop_rr(andn, t2, t2, nm ? t1 : t0);
            vshl(T, a);
            vop_rr(andn, T, T, t0);
            vop_rr(or_, t2, t2, T);
            vop_rm(and_, T, t0, quiet);
            vop_rr(or_, T, T, a);
            vop_rr(or_, t0, t0, t1);
            vop_rm(and_, t1, t1, quiet);
            vop_rr(or_, t1, t1, b);
            as->vex_is4(dbl ? vop::vblendvpd : vop::vblendvps, t1, t1, T, t2);
            // Multiply by 1.0 instead of vmax(t1, t1) to flush denormals under
            // DAZ/FTZ without raising MXCSR.IE on a quiet NaN.
            if (nm) vop_rm(mul_op, t1, t1, one);
            if (s.scalar) {
                as->vex_rr(vex_of(op::movaps), false, x64::id(d), 0, x64::id(t1));
            } else {
                // Mask NaN lanes to +0.0 so vmax/vmin never sees a quiet NaN.
                vop_rr(andn, T, t0, a);
                vop_rr(andn, t2, t0, b);
                vop_rr(mm, t3, t2, T);
                vop_rr(mm, T, T, t2);
                vop_rr(is_max ? and_ : or_, T, T, t3);
                as->vex_is4(dbl ? vop::vblendvpd : vop::vblendvps, T, T, t1, t0);
                if (alias) as->vex_rr(vex_of(op::movaps), false, x64::id(d), 0, x64::id(T));
            }

            if (!s.scalar) as->movups(t3, vspill(31));
            if (alias) as->movups(T, vspill(30));
            as->movups(t2, vspill(29));
            as->movups(t1, vspill(28));
            as->jmp(done);
        });
        return true;
    }
    return false;
}

// FMAX/FMIN/FMAXNM/FMINNM bit-exact with FPCR.DN = 0, NaNs included (AVX).
// The numeric result (both operand orders, AND for max: +0 wins, OR for min:
// -0 wins) is exact in every lane without a NaN. One unordered compare finds
// NaN lanes; only then (JRCXZ over the fixup: no host flags, or a flag-saving
// compare when RCX is not free) each NaN lane takes A64's FPProcessNaNs
// result: a signalling first operand, else a signalling second, quieted; else
// for FMAX/FMIN the first NaN, for FMAXNM/FMINNM the operand that is not a
// lone quiet NaN (the number), or the first when both are quiet NaNs. With
// sa/sb = signalling, na/nb = NaN: a wins where sa | (~sb & na) (FMAX/FMIN),
// sa | (~sb & nb) (NM); the winner is quieted only if it is a NaN.
// TRANSMUTE_FX_FP=oldminmax keeps the earlier lowering (NaN payloads as host
// policy).
LaneGuard lane_guard(FpEmit& e) {
    const Gp rcx = e.ctx.temp_gpr_prefer(Gp::rcx);
    if (rcx != Gp::none) return {rcx, true};
    e.ctx.clobber_host_flags();
    return {e.gtmp(), false};
}

// host_flags_live() answers for the instruction's family as a whole: in a
// multi-word family it describes the first word's flag reads and writes. Do
// not use rare_guard inside a family that both writes and reads NZCV (none
// does today): a later word's read would not be seen.
LaneGuard rare_guard(FpEmit& e) {
    if (e.ctx.host_flags_live()) return lane_guard(e);
    e.ctx.clobber_host_flags();   // dead: nothing to save
    return {e.gtmp(), false, true};
}

// ---- tininess before rounding (knob tinybefore) ----------------------------------------------
// A64 FPRound signals Underflow when the exponent is too small before rounding
// and the result is inexact; x64 tests the result rounded to the destination
// precision with an unbounded exponent. The two differ only when a value just
// below min_normal rounds up in magnitude to min_normal: A64 sets UFC and IXC,
// x64 PE alone. Rounded toward zero, a result is below min_normal exactly when
// the unrounded value is, and is inexact exactly when the original was, so the
// same operation run again toward zero raises UE precisely for A64's UFC. Its
// other flags are a subset of the original's (overflow toward zero needs a
// larger value; IE, ZE and DE depend on the operands alone). Sums and
// differences are exact near min_normal (every operand is a multiple of the
// smallest denormal). Not covered: FMULX, FRECPS/FRSQRTS and half precision.
// A result of exactly +-min_normal is rare: the common path is an AND and a
// compare into one vector temp, then PTEST/JCC when the host flags are dead,
// else MOVMSKPS and JRCXZ over a near JMP (rare_guard); the slow path
// rewrites MXCSR twice.
void tiny_before_fixup(FpEmit& e, unsigned es, bool scalar, unsigned width, Xmm r, Xmm t0,
                       const LaneGuard& guard, const std::function<void(Xmm)>& compute) {
    auto& a = e.as;
    const bool dbl = es == 64;
    const std::uint64_t mag = FpEmit::abs_bits(es), min_normal = dbl ? 0x0010000000000000ull : 0x00800000ull;
    // |r| in the lanes that matter, zero elsewhere (never equal to min_normal).
    const std::uint64_t mag64 = dbl ? mag : mag | mag << 32;
    const Mem lanes = scalar ? e.kscalar(es, mag) : width == 64 ? e.kconst(mag64, 0) : e.ksplat(es, mag);
    e.op(dbl ? op::andpd : op::andps, t0, r, Opnd::at(lanes));
    e.op(dbl ? op::pcmpeqq : op::pcmpeqd, t0, t0, Opnd::at(e.ksplat(es, min_normal)));
    // The slow path can exceed rel8 (memory operands, pooled constants): near
    // branches, and JRCXZ (host flags live) over a near jmp.
    const auto done = a.new_label();
    if (guard.ptest) {
        e.un(op::ptest, t0, t0);
        a.jcc(x64::Cond::e, done);
    } else {
        a.movmskps(guard.reg, t0);
        if (guard.jrcxz) {
            const auto fast = a.new_label(), slow = a.new_label();
            a.jrcxz_short(fast);
            a.jmp_short(slow);
            a.bind(fast);
            a.jmp(done);
            a.bind(slow);
        } else {
            a.test(32, guard.reg, guard.reg);
            a.jcc(x64::Cond::e, done);
        }
    }
    // FPCR and FPSR (adjacent words) are the MXCSR scratch, kept in the guard
    // GPR: the low word is ldmxcsr's operand, the high one keeps the original
    // MXCSR (A) across the rerun. Only moves and vector ops below: the host
    // flags stay as they are.
    const Mem slot = ctx_mem(kFpcrDisp);
    a.mov(64, guard.reg, slot);
    a.stmxcsr(slot);
    e.un(op::movss, t0, Opnd::at(slot));
    e.un_i(op::pshufd, t0, t0, 0x00);                                   // A in every lane
    e.op(op::orps, t0, t0, Opnd::at(e.kconst(0x6000, 0)));              // lane 0: RC = toward zero
    a.movq(slot, t0);                                                   // [A | RZ, A]
    a.ldmxcsr(slot);
    compute(t0);
    a.stmxcsr(slot);                                                    // [B, A]
    e.un(op::movsd, t0, Opnd::at(slot));
    e.op(op::andps, t0, t0, Opnd::at(e.kconst(0xffffffef00000010ull, 0)));   // B & UE, A & ~UE
    e.op(op::phaddd, t0, t0, t0);                                       // lane 0: A | (B & UE)
    a.movq(slot, t0);
    a.ldmxcsr(slot);
    a.mov(64, slot, guard.reg);
    a.bind(done);
}

bool emit_arith_tiny_before(FpEmit& e, const Inst& in) {
    const Shape s = in.sh;
    if (s.es != 32 && s.es != 64) return false;
    const bool fused = in.k == K::fmla || in.k == K::fmls || in.k == K::fmadd || in.k == K::fmsub ||
                       in.k == K::fnmadd || in.k == K::fnmsub;
    if (!fused && in.k != K::fmul && in.k != K::fdiv && in.k != K::fnmul) return false;
    if (fused && !e.feat.fma) return false;
    const bool dbl = s.dbl();
    const unsigned w = s.width();
    const Xmm a = e.vec(in.rn);
    // The second factor or divisor may stay in memory (the rerun reads it
    // again), unless it is also the destination: after dest() its guest
    // register reads as written.
    Opnd b = in.index >= 0 ? element_op(e, s, in.rm, in.index) : e.src(in.rm);
    if (b.is_mem && in.rm == in.rd) b = Opnd::of(in.index >= 0 ? element(e, s, in.rm, in.index) : e.vec(in.rm));
    const bool acc = in.k == K::fmla || in.k == K::fmls;
    const Xmm c = !fused ? Xmm{} : e.vec(acc ? in.rd : in.ra);
    // Every register before the first label: the guard GPR, the temp, Vd.
    const auto guard = rare_guard(e);
    const Xmm t0 = e.tmp();
    const Xmm d = e.dest(in.rd);
    // The sources must survive the rerun.
    const bool alias = d == a || (!b.is_mem && d == b.reg) || (fused && d == c);
    const Xmm T = alias ? e.tmp() : d;
    FmaKind kind = FmaKind::fmadd;   // FMADD a+n*m, FMSUB a-n*m, FNMADD -a-n*m, FNMSUB -a+n*m
    switch (in.k) {
    case K::fmls: case K::fmsub: kind = FmaKind::fnmadd; break;
    case K::fnmadd: kind = FmaKind::fnmsub; break;
    case K::fnmsub: kind = FmaKind::fmsub; break;
    default: break;
    }
    const auto compute = [&](Xmm out) {
        switch (in.k) {
        case K::fmul: e.op(s.pick(fops::mul), out, a, b); break;
        case K::fdiv: e.op(s.pick(fops::div), out, a, b); break;
        case K::fnmul:
            e.op(s.pick(fops::mul), out, a, b);
            e.op(s.pick(fops::xor_), out, out, Opnd::at(e.kscalar(s.es, FpEmit::sign_bits(s.es))));
            break;
        default:
            e.mov(out, c);
            e.fma(kind, FmaForm::f231, dbl, s.scalar, out, a, b);
            break;
        }
    };
    compute(T);
    tiny_before_fixup(e, s.es, s.scalar, w, T, t0, guard, compute);
    if (alias) e.mov(d, T);
    // Scalar results take their upper lanes from the first source (the
    // addend for the fused forms); 64-bit vectors compute the upper lanes.
    const bool zero = s.scalar && e.zero_above(fused ? (acc ? in.rd : in.ra) : in.rn, w);
    if (w != 128 && !zero) e.zero_upper(d, w);
    return true;
}

// T = op(a, b) (base kind), bit-exact. T, t0, t1, t2 are distinct and differ
// from a and b; a and b are unchanged. Only vector ops and the guard's
// MOVD/MOVMSKPS between the label and its target (no host flags written).
// `width`: the bits that matter (scalar: the element; vector: 64 or 128).
void minmax_exact_core(FpEmit& e, K kind, Shape s, unsigned width, Xmm T, Xmm a, Xmm b, Xmm t0, Xmm t1, Xmm t2,
                       const LaneGuard& guard) {
    const bool nm = kind == K::fmaxnm || kind == K::fminnm;
    const bool is_max = kind == K::fmax || kind == K::fmaxnm;
    const bool dbl = s.dbl();
    const SseOp mm = s.pick(is_max ? fops::max : fops::min), cmp = s.pick(fops::cmp);
    const SseOp and_ = s.pick(fops::and_), or_ = s.pick(fops::or_), andn = s.pick(fops::andn);
    const auto numeric = [&](Xmm out, Xmm scratch, Xmm x, Xmm y) {
        e.op(mm, out, x, y);
        e.op(mm, scratch, y, x);
        e.op(is_max ? and_ : or_, out, out, scratch);
    };
    const auto emit_nan_fixup = [&]() {
        const std::uint8_t shift = dbl ? 12 : 9;   // the quiet bit to the sign position
        const Mem quiet = e.ksplat(s.es, dbl ? 0x0008000000000000ull : 0x00400000ull);
        const auto shl = [&](Xmm dst, Xmm src) {
            if (dbl) e.psllq(dst, src, shift);
            else e.pslld(dst, src, shift);
        };
        e.op_i(cmp, t0, a, a, pred_unord);   // na
        e.op_i(cmp, t1, b, b, pred_unord);   // nb
        shl(t2, b);
        e.op(andn, t2, t2, t1);              // sb (sign bit)
        e.op(andn, t2, t2, nm ? t1 : t0);    // ~sb & nb (NM) / ~sb & na
        shl(T, a);                           // T is a temp of the fixup until the end
        e.op(andn, T, T, t0);                // sa
        e.op(or_, t2, t2, T);                // a wins
        e.op(and_, T, t0, Opnd::at(quiet));
        e.op(or_, T, T, a);                  // a, quieted if NaN
        e.op(or_, t0, t0, t1);               // NaN lanes
        e.op(and_, t1, t1, Opnd::at(quiet));
        e.op(or_, t1, t1, b);                // b, quieted if NaN
        e.blend(dbl, t1, t1, T, t2, t2);     // the NaN lanes' result
        // NM can return the number of a lone quiet NaN pair: with FZ (MXCSR.DAZ)
        // A64 flushes it. Multiply by 1.0 instead of max(x, x) so quiet NaNs
        // do not raise a spurious MXCSR.IE.
        if (nm) e.op(s.pick(fops::mul), t1, t1, Opnd::at(e.ksplat(s.es, dbl ? 0x3ff0000000000000ull : 0x3f800000ull)));
        if (s.scalar) {
            e.mov(T, t1);
        } else {
            // Mask NaN lanes to +0.0 before vmax/vmin so quiet NaNs never raise MXCSR.IE.
            const Mem spill = x64::ptr(Gp::r15, ctx_disp(offsetof(GuestContext, vspill) + 16u * 31u));
            e.as.movups(spill, t1);
            e.op(andn, T, t0, a);
            e.op(andn, t2, t0, b);
            e.op(mm, t1, t2, T);
            e.op(mm, T, T, t2);
            e.op(is_max ? and_ : or_, T, T, t1);
            e.as.movups(t1, spill);
            e.blend(dbl, T, T, t1, t0, t0);
        }
    };

    // Test NaNs BEFORE running vmax/vmin so quiet NaNs never raise MXCSR.IE.
    e.op_i(cmp, t0, a, b, pred_unord);
    if (s.scalar) {
        if (dbl) e.movq(guard.reg, t0);
        else e.movd(guard.reg, t0);
    } else if (width == 64) {
        e.movq(guard.reg, t0);
    } else {
        e.as.movmskps(guard.reg, t0);
    }
    const auto fast = e.as.new_label(), done = e.as.new_label();
    if (guard.jrcxz) {
        const auto fixup = e.as.new_label();
        e.as.jrcxz_short(fast);
        e.as.jmp(fixup);
        e.as.bind(fast);
        numeric(T, t1, a, b);
        e.as.jmp(done);
        e.as.bind(fixup);
        emit_nan_fixup();
        e.as.bind(done);
    } else {
        e.as.test(64, guard.reg, guard.reg);   // MOVQ: lane 1 is in bits 63:32
        e.as.jcc(x64::Cond::e, fast);
        emit_nan_fixup();
        e.as.jmp_short(done);
        e.as.bind(fast);
        numeric(T, t1, a, b);
        e.as.bind(done);
    }
}

// Out-of-line NaN fixup for AVX bit-exact min/max (when ctx.can_defer_cold()):
// Fast path checks NaN first (leaving a, b intact) and runs numeric min/max
// using ONLY 1 XMM temp (t0), 0 alias moves, 0 taken branches, and 0 GPRs
// when host flags are dead (scalar: vucomiss/sd + jp; 128-bit vector: vptest
// + jnz). Cold stub saves/restores 2-3 XMMs to GuestContext::vspill[28..30].
void minmax_exact_deferred(FpEmit& e, K kind, Shape s, unsigned width, Xmm d, Xmm a, Xmm b, Xmm t0,
                           bool flags_live, const LaneGuard& guard, bool sz_dead = false) {
    const bool nm = kind == K::fmaxnm || kind == K::fminnm;
    const bool is_max = kind == K::fmax || kind == K::fmaxnm;
    const bool dbl = s.dbl();
    const SseOp mm = s.pick(is_max ? fops::max : fops::min), cmp = s.pick(fops::cmp);
    const SseOp and_ = s.pick(fops::and_), or_ = s.pick(fops::or_), andn = s.pick(fops::andn);

    const auto cold = e.as.new_label(), done = e.as.new_label();
    if (!flags_live) {
        if (s.scalar) {
            e.ucomi(dbl, a, b, false);
            e.as.jcc(x64::Cond::p, cold);
        } else if (width == 128) {
            e.op_i(cmp, t0, a, b, pred_unord);
            e.as.vex_rr(vex_of(op::ptest), false, x64::id(t0), 0, x64::id(t0));
            e.as.jcc(x64::Cond::ne, cold);
        } else {
            e.op_i(cmp, t0, a, b, pred_unord);
            e.movq(guard.reg, t0);
            e.as.test(64, guard.reg, guard.reg);
            e.as.jcc(x64::Cond::ne, cold);
        }
    } else {
        e.op_i(cmp, t0, a, b, pred_unord);
        if (s.scalar) {
            if (dbl) e.movq(guard.reg, t0);
            else e.movd(guard.reg, t0);
        } else if (width == 64) {
            e.movq(guard.reg, t0);
        } else {
            e.as.movmskps(guard.reg, t0);
        }
        if (guard.jrcxz) {
            const auto fast = e.as.new_label();
            e.as.jrcxz_short(fast);
            e.as.jmp(cold);
            e.as.bind(fast);
        } else {
            e.as.test(64, guard.reg, guard.reg);
            e.as.jcc(x64::Cond::ne, cold);
        }
    }

    // Numeric fast path: safe even when d == a or d == b (t0 != a, b, d).
    if (sz_dead) {
        e.op(mm, d, a, b);
    } else {
        e.op(mm, t0, b, a);
        e.op(mm, d, a, b);
        e.op(is_max ? and_ : or_, d, d, t0);
    }
    e.as.bind(done);

    const Mem quiet = e.ksplat(s.es, dbl ? 0x0008000000000000ull : 0x00400000ull);
    const Mem one = nm ? e.ksplat(s.es, dbl ? 0x3ff0000000000000ull : 0x3f800000ull) : Mem{};
    const bool alias = d == a || d == b;
    std::uint32_t used = (1u << x64::id(a)) | (1u << x64::id(b)) | (1u << x64::id(d)) | (1u << x64::id(t0));
    const auto pick_free = [&]() {
        for (unsigned r = 0; r < 16; ++r) {
            if (!((used >> r) & 1u)) {
                used |= 1u << r;
                return x64::xmm(r);
            }
        }
        throw std::logic_error("fp: no free xmm for cold minmax stub");
    };
    const Xmm t1 = pick_free(), t2 = pick_free();
    const Xmm T = alias ? pick_free() : d;
    const Xmm t3 = !s.scalar ? pick_free() : Xmm{};
    auto* const as = &e.as;
    const std::uint8_t shift = dbl ? 12 : 9;
    const SseOp mul_op = s.pick(fops::mul);

    e.ctx.defer_cold([=]() {
        const auto vspill = [](unsigned slot) {
            return x64::ptr(Gp::r15, ctx_disp(offsetof(GuestContext, vspill) + 16u * slot));
        };
        const auto vop_rr = [as](SseOp o, Xmm dst, Xmm s1, Xmm s2) {
            as->vex_rr(vex_of(o), false, x64::id(dst), x64::id(s1), x64::id(s2));
        };
        const auto vop_rm = [as](SseOp o, Xmm dst, Xmm s1, const Mem& s2) {
            as->vex_rm(vex_of(o), false, x64::id(dst), x64::id(s1), s2);
        };
        const auto vop_rri = [as](SseOp o, Xmm dst, Xmm s1, Xmm s2, std::uint8_t imm) {
            as->vex_rr(vex_of(o), false, x64::id(dst), x64::id(s1), x64::id(s2));
            as->db(imm);
        };
        const auto vshl = [as, dbl, shift](Xmm dst, Xmm src) {
            as->vex_rr(VexOp{1, 1, static_cast<std::uint8_t>(dbl ? 0x73 : 0x72), false}, false, 6, x64::id(dst), x64::id(src));
            as->db(shift);
        };

        as->bind(cold);
        as->movups(vspill(28), t1);
        as->movups(vspill(29), t2);
        if (alias) as->movups(vspill(30), T);
        if (!s.scalar) as->movups(vspill(31), t3);

        vop_rri(cmp, t0, a, a, pred_unord);   // na
        vop_rri(cmp, t1, b, b, pred_unord);   // nb
        vshl(t2, b);
        vop_rr(andn, t2, t2, t1);             // sb
        vop_rr(andn, t2, t2, nm ? t1 : t0);   // ~sb & nb (NM) / ~sb & na
        vshl(T, a);
        vop_rr(andn, T, T, t0);               // sa
        vop_rr(or_, t2, t2, T);               // a wins
        vop_rm(and_, T, t0, quiet);
        vop_rr(or_, T, T, a);                 // a, quieted if NaN
        vop_rr(or_, t0, t0, t1);              // NaN lanes
        vop_rm(and_, t1, t1, quiet);
        vop_rr(or_, t1, t1, b);               // b, quieted if NaN
        as->vex_is4(dbl ? vop::vblendvpd : vop::vblendvps, t1, t1, T, t2);
        // Multiply by 1.0 instead of vmax(t1, t1) to flush denormals under
        // DAZ/FTZ without raising MXCSR.IE on a quiet NaN.
        if (nm) vop_rm(mul_op, t1, t1, one);
        if (s.scalar) {
            as->vex_rr(vex_of(op::movaps), false, x64::id(d), 0, x64::id(t1));
        } else {
            // Mask NaN lanes to +0.0 so vmax/vmin never sees a quiet NaN.
            vop_rr(andn, T, t0, a);
            vop_rr(andn, t2, t0, b);
            vop_rr(mm, t3, t2, T);
            vop_rr(mm, T, T, t2);
            vop_rr(is_max ? and_ : or_, T, T, t3);
            as->vex_is4(dbl ? vop::vblendvpd : vop::vblendvps, T, T, t1, t0);
            if (alias) as->vex_rr(vex_of(op::movaps), false, x64::id(d), 0, x64::id(T));
        }

        if (!s.scalar) as->movups(t3, vspill(31));
        if (alias) as->movups(T, vspill(30));
        as->movups(t2, vspill(29));
        as->movups(t1, vspill(28));
        as->jmp(done);
    });
}

void emit_minmax_exact(FpEmit& e, const Inst& in, Shape s, unsigned w) {
    const Xmm a = e.vec(in.rn), b = e.vec(in.rm);
    const bool zero = s.scalar ? e.zero_above(in.rn, w) && e.zero_above(in.rm, w) : w == 128;
    if (e.ctx.can_defer_cold()) {
        const bool flags_live = e.ctx.host_flags_live();
        LaneGuard guard{};
        if (flags_live) guard = lane_guard(e);
        else {
            e.ctx.clobber_host_flags();
            if (!s.scalar && w == 64) guard = {e.gtmp(), false};
        }
        const Xmm t0 = e.tmp();
        const Xmm d = e.dest(in.rd);
        const bool sz_dead = !s.scalar && (s.es == 32 || s.es == 64) && e.ctx.vec_sz_dead_after(1, in.rd, s.es);
        minmax_exact_deferred(e, in.k, s, w, d, a, b, t0, flags_live, guard, sz_dead);
        if (!zero) e.zero_upper(d, w);
        return;
    }
    // Every register before the first label: the guard GPR, the temps, Vd.
    const auto guard = lane_guard(e);
    const Xmm t0 = e.tmp(), t1 = e.tmp(), t2 = e.tmp();
    const Xmm d = e.dest(in.rd);
    const bool alias = d == a || d == b;   // a and b must survive until the fixup
    const Xmm T = alias ? e.tmp() : d;
    minmax_exact_core(e, in.k, s, w, T, a, b, t0, t1, t2, guard);
    if (alias) e.mov(d, T);
    if (!zero) e.zero_upper(d, w);
}

// Batched NaN check for 2..8 consecutive 128-bit FMAX/FMIN/FMAXNM/FMINNM ops
// of the same element size (AVX, cold deferral, dead host flags), optionally
// bridged by pure 128-bit FNEG/FABS/FCMEQ/FCMGE/FCMGT ops whose destinations
// do not feed a later minmax step (e.g. min3 -> fneg/fcmge -> max3 on the same
// 3 inputs). Read-only context-slot inputs stay in memory when scratch XMMs
// are tight so 6-op BVH8 slab chains fuse under a single vptest+jne instead of
// splitting. If any minmax input is NaN, the cold stub replays every step in
// exact program order with bit-exact FPProcessNaNs on the intact inputs.
unsigned emit_minmax_chain(FpEmit& e, std::span<const std::uint32_t> words) {
    if (words.size() < 2 || !e.avx() || fp_tiny_before_on() || !fp_on(fp_old_minmax) || !e.ctx.can_defer_cold() ||
        e.ctx.host_flags_live())
        return 0;
    const auto is_vec128_mm = [](const Inst& in) {
        return (in.k == K::fmax || in.k == K::fmin || in.k == K::fmaxnm || in.k == K::fminnm) && !in.sh.scalar &&
               in.sh.q && (in.sh.es == 32 || in.sh.es == 64);
    };
    const auto is_vec128_pass = [](const Inst& in, unsigned es) {
        return (in.k == K::fneg || in.k == K::fabs || in.k == K::fcmeq || in.k == K::fcmge || in.k == K::fcmgt) &&
               !in.sh.scalar && in.sh.q && in.sh.es == es;
    };
    constexpr unsigned kMaxChain = 16;
    Inst chain[kMaxChain];
    if (!decode(words[0], chain[0]) || !is_vec128_mm(chain[0])) return 0;
    const Shape s = chain[0].sh;
    const unsigned es = s.es;
    const bool dbl = s.dbl();

    const unsigned avail = e.ctx.vec_scratch_avail();
    const auto has_two_mem = [&](unsigned cur_len, std::uint32_t mem_mask) -> bool {
        if (mem_mask == 0) return false;
        std::uint32_t w = 0;
        for (unsigned k = 0; k < cur_len; ++k) {
            const bool unary = chain[k].k == K::fneg || chain[k].k == K::fabs;
            if (!unary) {
                const bool a_mem = ((mem_mask & ~w) >> chain[k].rn) & 1u;
                const bool b_mem = ((mem_mask & ~w) >> chain[k].rm) & 1u;
                if (a_mem && b_mem) return true;
            }
            w |= 1u << chain[k].rd;
        }
        return false;
    };
    const auto min_scratch = [&](unsigned cur_len, std::uint32_t in_mask, std::uint32_t nan_mask,
                                 std::uint32_t dst_mask, unsigned& unpinned_dests) -> unsigned {
        unpinned_dests = 0;
        unsigned sc = 0;
        std::uint32_t slot_mask = 0;
        Mem dummy{};
        for (std::uint32_t m = dst_mask; m; m &= m - 1) {
            const auto g = static_cast<unsigned>(std::countr_zero(m));
            if (e.ctx.vec_pinned(g)) continue;
            ++unpinned_dests;
            ++sc;
            if (((in_mask >> g) & 1u) && e.ctx.vec_write_deferred(g)) ++sc;
        }
        for (std::uint32_t m = in_mask & ~dst_mask; m; m &= m - 1) {
            const auto g = static_cast<unsigned>(std::countr_zero(m));
            if (e.ctx.vec_pinned(g)) continue;
            if (e.ctx.vec_slot(g, dummy)) slot_mask |= 1u << g;
            else ++sc;
        }
        // During the NaN check (before any step writes), any pure-destination
        // register (dst_mask & ~in_mask) can double as t1_nan. A dedicated t1
        // scratch register is only required for a 2-memory-operand step, or
        // when >2 NaN inputs exist with no pure destination to borrow.
        const bool need_dedicated_t1 =
            has_two_mem(cur_len, slot_mask) || (std::popcount(nan_mask) > 2 && (dst_mask & ~in_mask) == 0);
        sc += need_dedicated_t1 ? 2u : 1u;   // t0 [, t1]
        return sc;
    };

    std::uint32_t all_inputs = (1u << chain[0].rn) | (1u << chain[0].rm);
    std::uint32_t nan_inputs = all_inputs;
    std::uint32_t all_dests = 1u << chain[0].rd;
    std::uint32_t pass_dests = 0;
    unsigned cur_unpinned_dests = 0;
    if (min_scratch(1, all_inputs, nan_inputs, all_dests, cur_unpinned_dests) > avail || cur_unpinned_dests > 8)
        return 0;

    unsigned best_len = 1, best_mm = 1;
    std::uint32_t best_all_in = all_inputs, best_nan_in = nan_inputs, best_dests = all_dests;
    unsigned num_mm = 1;

    const auto max_len = std::min<std::size_t>(words.size(), kMaxChain);
    for (unsigned i = 1; i < max_len; ++i) {
        Inst in;
        if (!decode(words[i], in)) break;
        const bool is_mm = is_vec128_mm(in) && in.sh.es == es;
        const bool is_pass = !is_mm && is_vec128_pass(in, es);
        if (!is_mm && !is_pass) break;
        const bool unary = in.k == K::fneg || in.k == K::fabs;
        const std::uint32_t reads = (1u << in.rn) | (unary ? 0u : (1u << in.rm));
        if (is_mm && (reads & pass_dests) != 0) break;
        const std::uint32_t new_in = reads & ~all_dests;
        const std::uint32_t cand_all_in = all_inputs | new_in;
        const std::uint32_t cand_nan_in = nan_inputs | (is_mm ? new_in : 0u);
        const std::uint32_t cand_dests = all_dests | (1u << in.rd);
        chain[i] = in;
        unsigned ud = 0;
        if (std::popcount(cand_all_in) > 16 ||
            min_scratch(i + 1, cand_all_in, cand_nan_in, cand_dests, ud) > avail || ud > 8)
            break;
        all_inputs = cand_all_in;
        nan_inputs = cand_nan_in;
        all_dests = cand_dests;
        if (is_mm) {
            pass_dests &= ~(1u << in.rd);
            ++num_mm;
            best_len = i + 1;
            best_mm = num_mm;
            best_all_in = all_inputs;
            best_nan_in = nan_inputs;
            best_dests = all_dests;
        } else {
            pass_dests |= 1u << in.rd;
            // Absorb a trailing compare (FCMEQ/FCMGE/FCMGT) consuming a chain
            // minmax result so sign-of-zero-dead analysis can see it.
            if (num_mm >= 2 && (in.k == K::fcmeq || in.k == K::fcmge || in.k == K::fcmgt) &&
                (reads & all_dests) != 0) {
                best_len = i + 1;
                best_mm = num_mm;
                best_all_in = all_inputs;
                best_nan_in = nan_inputs;
                best_dests = all_dests;
            }
        }
    }
    if (best_mm < 2) return 0;

    const unsigned len = best_len;
    all_inputs = best_all_in;
    nan_inputs = best_nan_in;
    all_dests = best_dests;
    // Sign-of-zero deadness for each minmax step: on non-NaN inputs, a single
    // x64 vmin/vmax differs from A64 FMIN/FMAX only in the sign of zero when
    // pairing +0.0 and -0.0. If every consumer of step i's destination is
    // sign-of-zero insensitive (FCMEQ/FCMGE/FCMGT/FABS, or another sz_dead
    // minmax step) and the destination is overwritten before escaping (or
    // dead immediately after the chain), one vmin/vmax is bit-exact.
    bool sz_dead[kMaxChain]{};
    for (unsigned i = len; i-- > 0;) {
        if (!is_vec128_mm(chain[i])) continue;
        const unsigned g = chain[i].rd;
        bool dead = true;
        bool overwritten = false;
        for (unsigned j = i + 1; j < len; ++j) {
            const auto& cj = chain[j];
            const bool unary = cj.k == K::fneg || cj.k == K::fabs;
            if (cj.rn == g || (!unary && cj.rm == g)) {
                const bool insens = cj.k == K::fcmeq || cj.k == K::fcmge || cj.k == K::fcmgt || cj.k == K::fabs ||
                                    (is_vec128_mm(cj) && sz_dead[j]);
                if (!insens) {
                    dead = false;
                    break;
                }
            }
            if (cj.rd == g) {
                overwritten = true;
                break;
            }
        }
        if (dead && !overwritten) dead = e.ctx.vec_sz_dead_after(len, g, es);
        sz_dead[i] = dead;
    }
    // Drop inputs proven non-NaN (from SCVTF/UCVTF, FMAXNM/FMINNM, or a non-NaN constant).
    for (std::uint32_t m = nan_inputs; m; m &= m - 1) {
        const auto g = static_cast<unsigned>(std::countr_zero(m));
        if (e.ctx.vec_fp_non_nan(g, es)) {
            nan_inputs &= ~(1u << g);
            continue;
        }
        std::uint64_t cbits = 0;
        if (e.ctx.vec_fp_const(g, es, 128, cbits)) {
            const bool is_nan =
                dbl ? ((cbits & 0x7ff0000000000000ull) == 0x7ff0000000000000ull && (cbits & 0x000fffffffffffffull) != 0)
                    : ((cbits & 0x7f800000u) == 0x7f800000u && (cbits & 0x007fffffu) != 0);
            if (!is_nan) nan_inputs &= ~(1u << g);
        }
    }

    // Count how many times each initial input is read before being overwritten.
    unsigned read_uses[32]{};
    {
        std::uint32_t written = 0;
        for (unsigned i = 0; i < len; ++i) {
            const bool unary = chain[i].k == K::fneg || chain[i].k == K::fabs;
            if (!((written >> chain[i].rn) & 1u)) ++read_uses[chain[i].rn];
            if (!unary && chain[i].rm != chain[i].rn && !((written >> chain[i].rm) & 1u)) ++read_uses[chain[i].rm];
            written |= 1u << chain[i].rd;
        }
    }

    // Decide which read-only vec_slot inputs stay in memory vs promote to regs.
    std::uint32_t reg_inputs = 0;
    std::uint32_t mem_cand = 0;
    unsigned base_sc = 0;
    Mem dummy_slot{};
    for (std::uint32_t m = all_dests; m; m &= m - 1) {
        const auto g = static_cast<unsigned>(std::countr_zero(m));
        if (e.ctx.vec_pinned(g)) continue;
        ++base_sc;
        if (((all_inputs >> g) & 1u) && e.ctx.vec_write_deferred(g)) ++base_sc;
    }
    for (std::uint32_t m = all_inputs; m; m &= m - 1) {
        const auto g = static_cast<unsigned>(std::countr_zero(m));
        if (e.ctx.vec_pinned(g) || ((all_dests >> g) & 1u)) {
            reg_inputs |= 1u << g;
        } else if (e.ctx.vec_slot(g, dummy_slot)) {
            mem_cand |= 1u << g;
        } else {
            reg_inputs |= 1u << g;
            ++base_sc;
        }
    }
    const unsigned n_nan_total = static_cast<unsigned>(std::popcount(nan_inputs));
    const std::uint32_t pure_dests = all_dests & ~all_inputs;
    const unsigned temp_base = (n_nan_total > 2 && pure_dests == 0) ? 2u : 1u;
    // Try promoting ALL mem_cand to registers if they fit without needing t1 for mem.
    if (mem_cand != 0 && base_sc + temp_base + static_cast<unsigned>(std::popcount(mem_cand)) <= avail) {
        reg_inputs |= mem_cand;
        mem_cand = 0;
    } else if (mem_cand != 0) {
        // Some inputs will stay in memory: promote hottest first while budget allows.
        const unsigned temp_need = (has_two_mem(len, mem_cand) || (n_nan_total > 2 && pure_dests == 0)) ? 2u : 1u;
        unsigned spare = avail > (base_sc + temp_need) ? (avail - (base_sc + temp_need)) : 0u;
        for (unsigned pass = 0; pass < 2 && spare > 0; ++pass) {
            for (std::uint32_t m = mem_cand; m && spare > 0; m &= m - 1) {
                const auto g = static_cast<unsigned>(std::countr_zero(m));
                const bool hot = read_uses[g] >= 2 || ((nan_inputs >> g) & 1u);
                if ((pass == 0) == hot) {
                    reg_inputs |= 1u << g;
                    mem_cand &= ~(1u << g);
                    --spare;
                }
            }
        }
    }
    const bool need_t1 = has_two_mem(len, mem_cand) || (n_nan_total > 2 && pure_dests == 0);

    if (n_nan_total > 0) e.ctx.clobber_host_flags();

    // Allocate in order: read_vec -> write_vec -> t0/t1 -> mem slots.
    // Reading inputs before allocating temps avoids evicting an input that
    // read_vec is about to borrow.
    Opnd in_op[32]{};
    for (std::uint32_t m = reg_inputs; m; m &= m - 1) {
        const auto g = static_cast<unsigned>(std::countr_zero(m));
        in_op[g] = Opnd::of(e.ctx.read_vec(g));
    }
    Xmm dst_xmm[32]{};
    for (std::uint32_t m = all_dests; m; m &= m - 1) {
        const auto g = static_cast<unsigned>(std::countr_zero(m));
        dst_xmm[g] = e.ctx.write_vec(g);
    }
    const Xmm t0 = e.tmp();
    const Xmm t1 = need_t1 ? e.tmp() : (pure_dests != 0 ? dst_xmm[std::countr_zero(pure_dests)] : Xmm{});
    for (std::uint32_t m = mem_cand; m; m &= m - 1) {
        const auto g = static_cast<unsigned>(std::countr_zero(m));
        Mem slot = x64::ptr(Gp::r15, v_disp(g));
        in_op[g] = Opnd::at(slot);
    }

    Opnd nan_list[16]{};
    unsigned n_nan = 0;
    for (std::uint32_t m = nan_inputs; m; m &= m - 1) {
        const auto g = static_cast<unsigned>(std::countr_zero(m));
        nan_list[n_nan++] = in_op[g];
    }

    struct Step {
        K kind;
        Opnd a, b;
        Xmm d;
    };
    Step steps[kMaxChain]{};
    Opnd cur_op[32];
    for (unsigned g = 0; g < 32; ++g) cur_op[g] = in_op[g];
    bool need_sign = false, need_abs = false;
    for (unsigned i = 0; i < len; ++i) {
        const bool unary = chain[i].k == K::fneg || chain[i].k == K::fabs;
        if (chain[i].k == K::fneg) need_sign = true;
        else if (chain[i].k == K::fabs) need_abs = true;
        steps[i] = {chain[i].k, cur_op[chain[i].rn], unary ? Opnd{} : cur_op[chain[i].rm], dst_xmm[chain[i].rd]};
        cur_op[chain[i].rd] = Opnd::of(dst_xmm[chain[i].rd]);
    }

    const SseOp cmp = s.pick(fops::cmp), and_ = s.pick(fops::and_), or_ = s.pick(fops::or_), andn = s.pick(fops::andn);
    const SseOp xor_ = s.pick(fops::xor_), max_op = s.pick(fops::max), min_op = s.pick(fops::min);
    const Mem sign_mem = need_sign ? e.ksplat(es, FpEmit::sign_bits(es)) : Mem{};
    const Mem abs_mem = need_abs ? e.ksplat(es, abs_bits(es)) : Mem{};
    const auto cold = e.as.new_label(), done = e.as.new_label();

    // Batched NaN check over all unique minmax initial inputs BEFORE any write.
    if (n_nan > 0) {
        const auto emit_unord_pair = [&](Xmm dst, const Opnd& x, const Opnd& y) {
            if (!x.is_mem) {
                e.op_i(cmp, dst, x.reg, y, pred_unord);
            } else if (!y.is_mem) {
                e.op_i(cmp, dst, y.reg, x, pred_unord);
            } else {
                e.un(op::movups, dst, x);
                e.op_i(cmp, dst, dst, y, pred_unord);
            }
        };
        emit_unord_pair(t0, nan_list[0], n_nan > 1 ? nan_list[1] : nan_list[0]);
        for (unsigned p = 2; p < n_nan; p += 2) {
            emit_unord_pair(t1, nan_list[p], p + 1 < n_nan ? nan_list[p + 1] : nan_list[p]);
            e.op(or_, t0, t0, t1);
        }
        e.as.vex_rr(vex_of(op::ptest), false, x64::id(t0), 0, x64::id(t0));
        e.as.jcc(x64::Cond::ne, cold);
    }

    // Numeric fast path (zero NaNs guaranteed across every minmax step).
    for (unsigned i = 0; i < len; ++i) {
        const auto& st = steps[i];
        if (st.kind == K::fneg) {
            if (st.a.is_mem) {
                e.un(op::movups, t0, st.a);
                e.op(xor_, st.d, t0, Opnd::at(sign_mem));
            } else {
                e.op(xor_, st.d, st.a.reg, Opnd::at(sign_mem));
            }
            continue;
        }
        if (st.kind == K::fabs) {
            if (st.a.is_mem) {
                e.un(op::movups, t0, st.a);
                e.op(and_, st.d, t0, Opnd::at(abs_mem));
            } else {
                e.op(and_, st.d, st.a.reg, Opnd::at(abs_mem));
            }
            continue;
        }
        if (st.kind == K::fcmeq || st.kind == K::fcmge || st.kind == K::fcmgt) {
            const auto pred = st.kind == K::fcmeq ? pred_eq : st.kind == K::fcmge ? pred_ge_os : pred_gt_os;
            if (st.a.is_mem) {
                e.un(op::movups, t0, st.a);
                e.op_i(cmp, st.d, t0, st.b, pred);
            } else {
                e.op_i(cmp, st.d, st.a.reg, st.b, pred);
            }
            continue;
        }
        const bool is_max = st.kind == K::fmax || st.kind == K::fmaxnm;
        const SseOp mm = is_max ? max_op : min_op;
        if (sz_dead[i]) {
            if (!st.a.is_mem) {
                e.op(mm, st.d, st.a.reg, st.b);
            } else if (!st.b.is_mem) {
                e.op(mm, st.d, st.b.reg, st.a);
            } else {
                e.un(op::movups, t0, st.a);
                e.op(mm, st.d, t0, st.b);
            }
            continue;
        }
        if (!st.a.is_mem && !st.b.is_mem) {
            e.op(mm, t0, st.b.reg, st.a.reg);
            e.op(mm, st.d, st.a.reg, st.b.reg);
            e.op(is_max ? and_ : or_, st.d, st.d, t0);
        } else if (!st.a.is_mem) {
            e.un(op::movups, t0, st.b);
            e.op(mm, t0, t0, st.a.reg);
            e.op(mm, st.d, st.a.reg, st.b);
            e.op(is_max ? and_ : or_, st.d, st.d, t0);
        } else if (!st.b.is_mem) {
            e.un(op::movups, t0, st.a);
            e.op(mm, t0, t0, st.b.reg);
            e.op(mm, st.d, st.b.reg, st.a);
            e.op(is_max ? and_ : or_, st.d, st.d, t0);
        } else {
            e.un(op::movups, t1, st.a);
            e.un(op::movups, t0, st.b);
            e.op(mm, t0, t0, t1);
            e.op(mm, st.d, t1, st.b);
            e.op(is_max ? and_ : or_, st.d, st.d, t0);
        }
    }
    if (n_nan == 0) return len;
    e.as.bind(done);

    const Mem quiet = e.ksplat(es, dbl ? 0x0008000000000000ull : 0x00400000ull);
    const Mem one = e.ksplat(es, dbl ? 0x3ff0000000000000ull : 0x3f800000ull);
    const SseOp mul_op = s.pick(fops::mul);
    auto* const as = &e.as;
    const std::uint8_t shift = dbl ? 12 : 9;
    std::array<Step, kMaxChain> cold_steps{};
    for (unsigned i = 0; i < len; ++i) cold_steps[i] = steps[i];

    e.ctx.defer_cold([=]() {
        const auto vspill = [](unsigned slot) {
            return x64::ptr(Gp::r15, ctx_disp(offsetof(GuestContext, vspill) + 16u * slot));
        };
        const auto vop_rr = [as](SseOp o, Xmm dst, Xmm s1, Xmm s2) {
            as->vex_rr(vex_of(o), false, x64::id(dst), x64::id(s1), x64::id(s2));
        };
        const auto vop_rm = [as](SseOp o, Xmm dst, Xmm s1, const Mem& s2) {
            as->vex_rm(vex_of(o), false, x64::id(dst), x64::id(s1), s2);
        };
        const auto vop_rri = [as](SseOp o, Xmm dst, Xmm s1, Xmm s2, std::uint8_t imm) {
            as->vex_rr(vex_of(o), false, x64::id(dst), x64::id(s1), x64::id(s2));
            as->db(imm);
        };
        const auto vop_rmi = [as](SseOp o, Xmm dst, Xmm s1, const Mem& s2, std::uint8_t imm) {
            as->vex_rm(vex_of(o), false, x64::id(dst), x64::id(s1), s2);
            as->db(imm);
        };
        const auto vload = [as](Xmm dst, const Mem& m) {
            as->vex_rm(vex_of(op::movups), false, x64::id(dst), 0, m);
        };
        const auto vshl = [as, dbl, shift](Xmm dst, Xmm src) {
            as->vex_rr(VexOp{1, 1, static_cast<std::uint8_t>(dbl ? 0x73 : 0x72), false}, false, 6, x64::id(dst),
                       x64::id(src));
            as->db(shift);
        };

        as->bind(cold);
        for (unsigned i = 0; i < len; ++i) {
            const auto& st = cold_steps[i];
            if (st.kind == K::fneg) {
                if (st.a.is_mem) {
                    vload(t0, st.a.mem);
                    vop_rm(xor_, st.d, t0, sign_mem);
                } else {
                    vop_rm(xor_, st.d, st.a.reg, sign_mem);
                }
                continue;
            }
            if (st.kind == K::fabs) {
                if (st.a.is_mem) {
                    vload(t0, st.a.mem);
                    vop_rm(and_, st.d, t0, abs_mem);
                } else {
                    vop_rm(and_, st.d, st.a.reg, abs_mem);
                }
                continue;
            }
            if (st.kind == K::fcmeq || st.kind == K::fcmge || st.kind == K::fcmgt) {
                const auto pred = st.kind == K::fcmeq ? pred_eq : st.kind == K::fcmge ? pred_ge_os : pred_gt_os;
                Xmm lhs = st.a.reg;
                if (st.a.is_mem) {
                    vload(t0, st.a.mem);
                    lhs = t0;
                }
                if (st.b.is_mem) vop_rmi(cmp, st.d, lhs, st.b.mem, pred);
                else vop_rri(cmp, st.d, lhs, st.b.reg, pred);
                continue;
            }
            const bool nm = st.kind == K::fmaxnm || st.kind == K::fminnm;
            const bool is_max = st.kind == K::fmax || st.kind == K::fmaxnm;
            const SseOp mm = is_max ? max_op : min_op;
            const bool alias = (!st.a.is_mem && st.d == st.a.reg) || (!st.b.is_mem && st.d == st.b.reg);
            std::uint32_t used = (1u << x64::id(st.d)) | (1u << x64::id(t0));
            if (!st.a.is_mem) used |= 1u << x64::id(st.a.reg);
            if (!st.b.is_mem) used |= 1u << x64::id(st.b.reg);
            const auto pick_free = [&]() {
                for (unsigned r = 0; r < 16; ++r) {
                    if (!((used >> r) & 1u)) {
                        used |= 1u << r;
                        return x64::xmm(r);
                    }
                }
                throw std::logic_error("fp: no free xmm for cold minmax chain");
            };
            const Xmm s1 = pick_free(), s2 = pick_free(), s3 = pick_free();
            const Xmm T = alias ? pick_free() : st.d;
            const Xmm ra = st.a.is_mem ? pick_free() : st.a.reg;
            const Xmm rb = st.b.is_mem ? pick_free() : st.b.reg;
            as->movups(vspill(28), s1);
            as->movups(vspill(29), s2);
            as->movups(vspill(31), s3);
            if (alias) as->movups(vspill(30), T);
            if (st.a.is_mem) {
                as->movups(vspill(27), ra);
                vload(ra, st.a.mem);
            }
            if (st.b.is_mem) {
                as->movups(vspill(26), rb);
                vload(rb, st.b.mem);
            }

            vop_rri(cmp, t0, ra, ra, pred_unord);   // na
            vop_rri(cmp, s1, rb, rb, pred_unord);   // nb
            vshl(s2, rb);
            vop_rr(andn, s2, s2, s1);               // sb
            vop_rr(andn, s2, s2, nm ? s1 : t0);     // ~sb & nb (NM) / ~sb & na
            vshl(T, ra);
            vop_rr(andn, T, T, t0);                 // sa
            vop_rr(or_, s2, s2, T);                 // a wins
            vop_rm(and_, T, t0, quiet);
            vop_rr(or_, T, T, ra);                  // a, quieted if NaN
            vop_rr(or_, t0, t0, s1);                // NaN lanes
            vop_rm(and_, s1, s1, quiet);
            vop_rr(or_, s1, s1, rb);                // b, quieted if NaN
            as->vex_is4(dbl ? vop::vblendvpd : vop::vblendvps, s1, s1, T, s2);
            // Multiply by 1.0 instead of vmax(s1, s1) to flush denormals under
            // DAZ/FTZ without raising MXCSR.IE on a quiet NaN.
            if (nm) vop_rm(mul_op, s1, s1, one);
            // Mask NaN lanes to +0.0 so vmax/vmin never sees a quiet NaN.
            vop_rr(andn, T, t0, ra);
            vop_rr(andn, s2, t0, rb);
            vop_rr(mm, s3, s2, T);
            vop_rr(mm, T, T, s2);
            vop_rr(is_max ? and_ : or_, T, T, s3);
            as->vex_is4(dbl ? vop::vblendvpd : vop::vblendvps, T, T, s1, t0);
            if (alias) as->vex_rr(vex_of(op::movaps), false, x64::id(st.d), 0, x64::id(T));

            if (st.b.is_mem) as->movups(rb, vspill(26));
            if (st.a.is_mem) as->movups(ra, vspill(27));
            if (alias) as->movups(T, vspill(30));
            as->movups(s3, vspill(31));
            as->movups(s2, vspill(29));
            as->movups(s1, vspill(28));
        }
        as->jmp(done);
    });
    return len;
}

void emit_minmax_scratch(FpEmit& e, K kind, Shape s, Xmm out, Xmm a, Xmm b, Xmm u) {
    if (!e.avx()) throw std::logic_error("fp: emit_minmax_scratch needs AVX");
    kind = base_minmax(kind);
    if (kind == K::fmin) {
        emit_minmax(e, kind, s, out, a, b, u, u);
        return;
    }
    if (kind == K::fmax) {   // out != a, b: (t1 & t2) | unord(a, b) with one temp
        e.op(s.pick(fops::max), out, a, b);
        e.op(s.pick(fops::max), u, b, a);
        e.op(s.pick(fops::and_), out, out, u);
        e.op_i(s.pick(fops::cmp), u, a, b, pred_unord);
        e.op(s.pick(fops::or_), out, out, u);
        return;
    }
    const bool is_max = kind == K::fmaxnm;
    const SseOp mm = s.pick(is_max ? fops::max : fops::min), cmp = s.pick(fops::cmp);
    e.op(mm, out, a, b);   // t1: NaN iff b is NaN
    e.op(mm, u, b, a);     // t2: NaN iff a is NaN; a and b are dead from here
    if (is_max) {          // NaN lanes -> all-ones, then AND
        e.op_i(cmp, a, out, out, pred_unord);
        e.op(s.pick(fops::or_), out, out, a);
        e.op_i(cmp, a, u, u, pred_unord);
        e.op(s.pick(fops::or_), u, u, a);
        e.op(s.pick(fops::and_), out, out, u);
        return;
    }
    // min: t1 & ~nan(b), t2 & ~(nan(a) & ~nan(b)), then OR (both NaN stays NaN).
    e.op_i(cmp, b, out, out, pred_unord);
    e.op_i(cmp, a, u, u, pred_unord);
    e.op(s.pick(fops::andn), out, b, out);
    e.op(s.pick(fops::andn), a, b, a);
    e.op(s.pick(fops::andn), u, a, u);
    e.op(s.pick(fops::or_), out, out, u);
}

void emit_round_away(FpEmit& e, Shape s, Xmm out, Xmm x, bool exact, const Xmm (&T)[4]) {
    // r = trunc(|x|); f = trunc(2 * (|x| - r)) (1 iff the fraction is >= 0.5);
    // result = copysign(r + f, x). Every step is exact.
    const unsigned es = s.es;
    const Xmm m = T[0], a = T[1], t = T[2], d = T[3];
    e.abs_mask(es, m);
    e.op(s.pick(fops::and_), a, x, m);
    e.op(s.pick(fops::andn), m, m, x);            // m = sign(x)
    e.round(s, t, a, exact ? 0x03 : 0x0b);
    e.op(s.pick(fops::sub), d, a, t);
    e.op(s.pick(fops::add), d, d, d);
    e.zero(a);
    e.op(s.pick(fops::max), d, d, a);             // inf - inf = NaN -> 0
    e.round(s, d, d, 0x0b);
    e.op(s.pick(fops::add), t, t, d);
    e.op(s.pick(fops::or_), out, t, m);
}

// ---- arithmetic -----------------------------------------------------------------------------

bool emit_arith_h(FpEmit& e, const Inst& in);

bool emit_arith(FpEmit& e, const Inst& in) {
    const Shape s = in.sh;
    if (s.es == 16) return emit_arith_h(e, in);
    const unsigned w = s.width();
    const bool dbl = s.dbl();
    if (fp_tiny_before_on() && emit_arith_tiny_before(e, in)) return true;
    switch (in.k) {
    case K::fadd: case K::fsub: case K::fmul: case K::fdiv: {
        const FOps& f = in.k == K::fadd ? fops::add : in.k == K::fsub ? fops::sub
                      : in.k == K::fmul ? fops::mul : fops::div;
        const bool comm = in.k == K::fadd || in.k == K::fmul;
        guest_binary(e, in, s.pick(f), comm, -1, comm);
        return true;
    }
    case K::fnmul: {
        if (e.avx() && s.scalar && fp_on(fp_old_mask)) {
            // -(n * m) (FNMUL is scalar only): the product, then XOR with a
            // pooled sign mask whose upper lanes are zero. The product takes
            // its upper lanes from the first source: one with zeros there goes
            // first (FMUL is commutative up to the NaN payload, which is host
            // policy), a memory operand is loaded with movss/movsd (zeroing).
            const Opnd n = e.src(in.rn), m = e.src(in.rm);
            const bool zn = n.is_mem || e.zero_above(in.rn, w);
            const bool zm = !m.is_mem && e.zero_above(in.rm, w);
            const bool swap = !zn && zm;
            const Xmm first = load_opnd(e, swap ? m : n, s);
            const Opnd& second = swap ? n : m;
            const Xmm d = e.dest(in.rd);
            e.op(s.pick(fops::mul), d, first, second);
            e.op(op::xorps, d, d, Opnd::at(e.kscalar(s.es, FpEmit::sign_bits(s.es))));
            if (!zn && !zm) e.zero_upper(d, w);
            return true;
        }
        const Xmm a = e.vec(in.rn), b = e.vec(in.rm);
        const Xmm t = e.tmp(), m = e.tmp();
        e.op(s.pick(fops::mul), t, a, b);
        e.sign_mask(s.es, m);
        e.op(s.pick(fops::xor_), t, t, m);
        e.commit(in.rd, t, w);
        return true;
    }
    case K::fabd: {
        if (e.avx() && fp_on(fp_old_mask)) {
            // |n - m|: AND with a pooled magnitude mask (scalar: upper lanes
            // zero, so the result's upper lanes are zero too). A 64-bit
            // vector's upper lanes are |0 - 0| = +0 when both sources are
            // zero above bit 64.
            const Xmm a = e.vec(in.rn);
            const Opnd b = e.src(in.rm);
            const Xmm t = e.tmp();
            const std::uint64_t mag = abs_bits(s.es);
            const bool zero = s.scalar || w == 128 || (e.zero_above(in.rn, 64) && e.zero_above(in.rm, 64));
            e.op(s.pick(fops::sub), t, a, b);
            const Xmm d = e.dest(in.rd);
            e.op(s.pick(fops::and_), d, t, Opnd::at(s.scalar ? e.kscalar(s.es, mag) : e.ksplat(s.es, mag)));
            if (!zero) e.zero_upper(d, w);
            return true;
        }
        const Xmm a = e.vec(in.rn), b = e.vec(in.rm);
        const Xmm t = e.tmp(), m = e.tmp();
        e.op(s.pick(fops::sub), t, a, b);
        if (s.scalar) {
            e.scalar_const(s.es, m, abs_bits(s.es));   // upper lanes zero
            const Xmm d = e.dest(in.rd);
            e.op(s.pick(fops::and_), d, t, m);
        } else {
            e.abs_mask(s.es, m);
            const Xmm d = e.dest(in.rd);
            e.op(s.pick(fops::and_), d, t, m);
            e.zero_upper(d, w);
        }
        return true;
    }
    case K::fmulx: {
        const Xmm a = e.vec(in.rn);
        const Xmm m_reg = e.vec(in.rm);
        const Xmm b = in.index >= 0 ? element(e, s, in.rm, in.index) : m_reg;
        const bool b_temp = b != m_reg && b != a;
        const Xmm p = e.tmp(), f = e.tmp(), x = e.tmp();
        e.op(s.pick(fops::mul), p, a, b);
        e.op_i(s.pick(fops::cmp), f, p, p, pred_unord);   // NaN products
        e.op_i(s.pick(fops::cmp), x, a, b, pred_unord);   // NaN inputs
        e.op(s.pick(fops::andn), x, x, f);                 // 0 * inf lanes
        e.op(s.pick(fops::xor_), f, a, b);
        const Xmm m = b_temp ? b : e.tmp();                // b is dead from here
        e.sign_mask(s.es, m);
        e.op(s.pick(fops::and_), f, f, m);                 // sign(a) ^ sign(b)
        if (dbl) e.psrlq(m, m, 1); else e.psrld(m, m, 1);  // 2.0
        e.op(s.pick(fops::or_), f, f, m);
        e.blend(dbl, p, p, f, x, m);
        e.commit(in.rd, p, w);
        return true;
    }
    case K::fmax: case K::fmin: case K::fmaxnm: case K::fminnm: {
        if (e.avx() && fp_on(fp_no_const_minmax) && emit_minmax_const(e, in, s, w)) return true;
        if (e.avx() && fp_on(fp_old_minmax)) {
            emit_minmax_exact(e, in, s, w);
            return true;
        }
        // Both operand orders are computed: both sources in registers.
        Xmm a = e.vec(in.rn), b = e.vec(in.rm);
        // Scalar results take their upper lanes from a (FMAX) or from a | b
        // (the others); FMAX is symmetric up to the NaN payload, so an
        // operand with zeros above the scalar goes first.
        bool zero = w == 128;
        if (s.scalar) {
            const bool za = e.zero_above(in.rn, w), zb = e.zero_above(in.rm, w);
            if (in.k == K::fmax) {
                if (!za && zb) std::swap(a, b);
                zero = za || zb;
            } else {
                zero = za && zb;
            }
        }
        const auto t = minmax_temps(e, in.k);
        const Xmm d = e.dest(in.rd);
        if (!minmax_may_alias(e, in.k) && (d == a || d == b)) {
            const Xmm o = e.tmp();
            emit_minmax(e, in.k, s, o, a, b, t.u, t.v);
            if (zero) e.mov(d, o);
            else e.commit(in.rd, o, w);
        } else {
            emit_minmax(e, in.k, s, d, a, b, t.u, t.v);
            if (!zero) e.zero_upper(d, w);
        }
        return true;
    }
    case K::fcmeq: case K::fcmge: case K::fcmgt:
        if (e.avx()) {
            const auto pred = in.k == K::fcmeq ? pred_eq : in.k == K::fcmge ? pred_ge_os : pred_gt_os;
            guest_binary(e, in, s.pick(fops::cmp), in.k == K::fcmeq, pred);
            return true;
        }
        [[fallthrough]];
    case K::facge: case K::facgt: {
        if (e.avx() && fp_on(fp_old_mask) && (in.k == K::facge || in.k == K::facgt)) {
            // |n| >= |m| / |n| > |m| with pooled magnitude masks (scalar masks
            // have zero upper lanes: cmpss then leaves the upper lanes zero).
            const std::uint64_t mag = abs_bits(s.es);
            const Mem mask = s.scalar ? e.kscalar(s.es, mag) : e.ksplat(s.es, mag);
            const Xmm a = e.vec(in.rn);
            const Opnd b = e.src(in.rm);
            const Xmm t = e.tmp(), u = e.tmp();
            e.op(s.pick(fops::and_), t, a, Opnd::at(mask));
            if (b.is_mem) {
                e.un(op::movups, u, Opnd::at(mask));
                e.op(s.pick(fops::and_), u, u, b);
            } else {
                e.op(s.pick(fops::and_), u, b.reg, Opnd::at(mask));
            }
            const Xmm d = e.dest(in.rd);
            e.op_i(s.pick(fops::cmp), d, t, u, in.k == K::facge ? pred_ge_os : pred_gt_os);
            if (!s.scalar) e.zero_upper(d, w);
            return true;
        }
        Xmm a = e.vec(in.rn), b = e.vec(in.rm);
        if (in.k == K::facge || in.k == K::facgt) {
            const Xmm m = e.tmp(), t = e.tmp();
            e.abs_mask(s.es, m);
            e.op(s.pick(fops::and_), t, a, m);
            e.op(s.pick(fops::and_), m, m, b);
            a = t;
            b = m;
        }
        const SseOp c = s.pick(fops::cmp);
        if (in.k == K::fcmeq) {
            final_op(e, in.rd, w, c, a, b, true, pred_eq);
        } else {
            const bool ge = in.k == K::fcmge || in.k == K::facge;
            if (e.avx()) final_op(e, in.rd, w, c, a, b, false, ge ? pred_ge_os : pred_gt_os);
            else final_op(e, in.rd, w, c, b, a, false, ge ? pred_le : pred_lt);
        }
        return true;
    }
    case K::fcmeq0: case K::fcmge0: case K::fcmgt0: case K::fcmle0: case K::fcmlt0: {
        const SseOp c = s.pick(fops::cmp);
        if (e.avx()) {   // cmp(0, n): a scalar result takes its zero upper lanes from 0
            Pred pred;
            switch (in.k) {
            case K::fcmeq0: pred = pred_eq; break;
            case K::fcmge0: pred = pred_le; break;      // 0 <= n
            case K::fcmgt0: pred = pred_lt; break;      // 0 < n
            case K::fcmle0: pred = pred_ge_os; break;   // 0 >= n
            default: pred = pred_gt_os; break;          // 0 > n
            }
            const Opnd n = e.src(in.rn);
            const Xmm z = e.tmp();
            e.zero(z);
            const Xmm d = e.dest(in.rd);
            e.op_i(c, d, z, n, pred);
            if (!s.scalar) e.zero_upper(d, w);
            return true;
        }
        const Xmm a = e.vec(in.rn);
        const Xmm z = e.tmp();
        e.zero(z);
        switch (in.k) {
        case K::fcmeq0: final_op(e, in.rd, w, c, a, z, true, pred_eq); break;
        case K::fcmle0: final_op(e, in.rd, w, c, a, z, false, pred_le); break;
        case K::fcmlt0: final_op(e, in.rd, w, c, a, z, false, pred_lt); break;
        default: {
            const auto pred = in.k == K::fcmge0 ? pred_le : pred_lt;   // 0 <= a, 0 < a
            if (e.avx()) {
                final_op(e, in.rd, w, c, z, a, false, pred);
            } else {
                e.op_i(c, z, z, a, pred);
                e.commit(in.rd, z, w);
            }
        }
        }
        return true;
    }
    case K::faddp: case K::fmaxp: case K::fminp: case K::fmaxnmp: case K::fminnmp: {
        if (in.k != K::faddp && e.avx() && fp_on(fp_old_minmax)) {
            // NaN-exact: op(n0, n1) / op(even lanes, odd lanes) in A64's operand order.
            e.allow_vec_temps(5);
            const K kind = base_minmax(in.k);
            if (s.scalar) {
                const Xmm n = e.vec(in.rn);
                if (e.ctx.can_defer_cold()) {
                    const bool flags_live = e.ctx.host_flags_live();
                    LaneGuard guard{};
                    if (flags_live) guard = lane_guard(e);
                    else e.ctx.clobber_host_flags();
                    const Xmm o = e.tmp(), t0 = e.tmp();
                    const Xmm d = e.dest(in.rd);
                    if (dbl) e.psrldq(o, n, 8);
                    else e.op_i(op::insertps, o, n, n, 0x4e);
                    minmax_exact_deferred(e, kind, s, w, d, n, o, t0, flags_live, guard);
                    e.zero_upper(d, w);
                    return true;
                }
                const auto guard = lane_guard(e);
                const Xmm o = e.tmp(), t0 = e.tmp(), t1 = e.tmp(), t2 = e.tmp();
                const Xmm d = e.dest(in.rd);
                const Xmm T = d == n ? e.tmp() : d;
                if (dbl) e.psrldq(o, n, 8);
                else e.op_i(op::insertps, o, n, n, 0x4e);
                minmax_exact_core(e, kind, s, w, T, n, o, t0, t1, t2, guard);
                if (T != d) e.mov(d, T);
                e.zero_upper(d, w);
                return true;
            }
            const Xmm a = e.vec(in.rn), b = e.vec(in.rm);
            const Xmm ev = e.tmp(), od = e.tmp();
            if (dbl) {
                e.op(op::unpcklpd, ev, a, b);
                e.op(op::unpckhpd, od, a, b);
            } else if (s.q) {
                e.op_i(op::shufps, ev, a, b, 0x88);
                e.op_i(op::shufps, od, a, b, 0xdd);
            } else {
                e.op(op::unpcklpd, od, a, b);   // [a0 a1 b0 b1]
                e.op_i(op::shufps, ev, od, od, 0x88);
                e.op_i(op::shufps, od, od, od, 0xdd);
            }
            if (e.ctx.can_defer_cold()) {
                const bool flags_live = e.ctx.host_flags_live();
                LaneGuard guard{};
                if (flags_live) guard = lane_guard(e);
                else {
                    e.ctx.clobber_host_flags();
                    if (w == 64) guard = {e.gtmp(), false};
                }
                const Xmm t0 = e.tmp();
                const Xmm d = e.dest(in.rd);
                minmax_exact_deferred(e, kind, s, w, d, ev, od, t0, flags_live, guard);
                e.zero_upper(d, w);
                return true;
            }
            const auto guard = lane_guard(e);
            const Xmm t0 = e.tmp(), t1 = e.tmp(), t2 = e.tmp();
            const Xmm d = e.dest(in.rd);
            minmax_exact_core(e, kind, s, w, d, ev, od, t0, t1, t2, guard);
            e.zero_upper(d, w);
            return true;
        }
        if (s.scalar) {
            // o = {n[1], 0...}: as the first source it gives a zero upper
            // part (op(n1, n0) equals op(n0, n1) up to the NaN payload).
            const Xmm o = e.tmp();
            const Opnd nsrc = e.src(in.rn);
            if (in.k == K::faddp && nsrc.is_mem) {
                Mem hi = nsrc.mem;
                hi.disp += dbl ? 8 : 4;
                e.un(dbl ? op::movsd : op::movss, o, Opnd::at(hi));
                const Xmm d = e.dest(in.rd);
                e.op(s.pick(fops::add), d, o, nsrc);
                return true;
            }
            const Xmm n = e.vec(in.rn);
            if (dbl) e.psrldq(o, n, 8);
            else e.op_i(op::insertps, o, n, n, 0x4e);
            if (in.k == K::faddp) {
                const Xmm d = e.dest(in.rd);
                if (e.avx() || d != n) e.op(s.pick(fops::add), d, o, n);
                else { e.op(s.pick(fops::add), o, o, n); e.mov(d, o); }
                return true;
            }
            const auto t = minmax_temps(e, in.k);
            const bool zero = in.k == K::fmaxp;   // FMAX: upper lanes from the first source
            const Xmm d = e.dest(in.rd);
            if (!minmax_may_alias(e, in.k) && d == n) {
                const Xmm r = e.tmp();
                emit_minmax(e, in.k, s, r, o, n, t.u, t.v);
                e.commit(in.rd, r, w);
            } else {
                emit_minmax(e, in.k, s, d, o, n, t.u, t.v);
                if (!zero) e.zero_upper(d, w);
            }
            return true;
        }
        const Xmm a = e.vec(in.rn), b = e.vec(in.rm);
        if (in.k == K::faddp && s.q) {
            final_op(e, in.rd, w, dbl ? op::haddpd : op::haddps, a, b, false);
            return true;
        }
        Xmm ev = e.tmp(), od = e.tmp();
        if (dbl) {
            e.op(op::unpcklpd, ev, a, b);
            e.op(op::unpckhpd, od, a, b);
        } else if (s.q) {
            e.op_i(op::shufps, ev, a, b, 0x88);
            e.op_i(op::shufps, od, a, b, 0xdd);
        } else {
            e.op(op::unpcklpd, od, a, b);   // [a0 a1 b0 b1]
            if (in.k == K::faddp) {
                const Xmm d = e.dest(in.rd);
                e.op(op::haddps, d, od, od);
                e.zero_upper(d, w);
                return true;
            }
            e.op_i(op::shufps, ev, od, od, 0x88);
            e.op_i(op::shufps, od, od, od, 0xdd);
        }
        const auto t = minmax_temps(e, in.k);
        const Xmm d = e.dest(in.rd);
        if (in.k == K::faddp) e.op(s.pick(fops::add), d, ev, od);
        else emit_minmax(e, in.k, s, d, ev, od, t.u, t.v);
        e.zero_upper(d, w);
        return true;
    }
    case K::fmaxv: case K::fminv: case K::fmaxnmv: case K::fminnmv: {
        // Reduce(op, Vn.4S) = op(op(n0, n1), op(n2, n3)).
        if (e.avx() && fp_on(fp_old_minmax)) {   // NaN-exact, A64's operand order
            const K kind = base_minmax(in.k);
            const Xmm n = e.vec(in.rn);
            if (e.ctx.can_defer_cold()) {
                const bool flags_live = e.ctx.host_flags_live();
                LaneGuard guard{};
                if (flags_live) guard = lane_guard(e);
                else e.ctx.clobber_host_flags();
                const Xmm o = e.tmp(), r = e.tmp(), t0 = e.tmp();
                const Xmm d = e.dest(in.rd);
                e.un(op::movshdup, o, n);
                Shape packed = s;
                packed.scalar = false;
                packed.q = true;
                minmax_exact_deferred(e, kind, packed, 128, r, n, o, t0, flags_live, guard);   // r0, r2
                e.op_i(op::insertps, o, r, r, 0x8e);                                           // {r[2], 0, 0, 0}
                Shape scalar = s;
                scalar.scalar = true;
                minmax_exact_deferred(e, kind, scalar, 32, d, r, o, t0, flags_live, guard);
                e.zero_upper(d, 32);
                return true;
            }
            e.allow_vec_temps(5);
            const auto guard = lane_guard(e);
            const Xmm o = e.tmp(), r = e.tmp(), t0 = e.tmp(), t1 = e.tmp(), t2 = e.tmp();
            const Xmm d = e.dest(in.rd);
            e.un(op::movshdup, o, n);
            Shape packed = s;
            packed.scalar = false;
            packed.q = true;
            minmax_exact_core(e, kind, packed, 128, r, n, o, t0, t1, t2, guard);   // r0, r2
            e.op_i(op::insertps, o, r, r, 0x8e);                                   // {r[2], 0, 0, 0}
            Shape scalar = s;
            scalar.scalar = true;
            minmax_exact_core(e, kind, scalar, 32, d, r, o, t0, t1, t2, guard);
            e.zero_upper(d, 32);
            return true;
        }
        const Xmm n = e.vec(in.rn);
        const Xmm o = e.tmp(), r = e.tmp();
        const auto t = minmax_temps(e, in.k);
        e.un(op::movshdup, o, n);
        Shape packed = s;
        packed.scalar = false;
        packed.q = true;
        emit_minmax(e, in.k, packed, r, n, o, t.u, t.v);
        e.op_i(op::insertps, o, r, r, 0x8e);   // {r[2], 0, 0, 0}: zero upper lanes for FMAX
        Shape scalar = s;
        scalar.scalar = true;
        const Xmm d = e.dest(in.rd);
        emit_minmax(e, in.k, scalar, d, o, r, t.u, t.v);
        if (in.k != K::fmaxv) e.zero_upper(d, 32);
        return true;
    }
    default:
        return false;
    }
}

// ---- fused multiply-add -----------------------------------------------------------------------

bool emit_fused_h(FpEmit& e, const Inst& in);

// Pairwise FP sum reduction:
//   FADDP Vd.4S, Vn.4S, Vn.4S
//   FADDP Sd2, Vd.2S
// when Sd2 == Vd or Vd is dead after the pair. Splits Vn into even/odd lane
// pairs [n0,n2,n0,n2] and [n1,n3,n1,n3] via 1-cycle vpermilps so vaddps
// performs the exact same (n0+n1) and (n2+n3) additions with identical IEEE
// rounding and zero spurious MXCSR exceptions, avoiding slow 3-uop vhaddps.
// FMOV Vd.4S/.2D, #imm + FADD/FSUB/FMUL/FDIV Vd2.4S/.2D, Vn, Vm -> direct
// VEX memory operand from the constant pool when Vd is dead after the pair.
bool emit_fmov_fparith(FpEmit& e, const Inst& in0, const Inst& in1) {
    if (!e.avx() || !fp_on(fp_old_imm)) return false;
    const Shape s = in0.sh;
    if (s.scalar || !s.q || (s.es != 32 && s.es != 64)) return false;
    if (in1.sh.scalar || !in1.sh.q || in1.sh.es != s.es) return false;
    const bool is_add = in1.k == K::fadd, is_sub = in1.k == K::fsub;
    const bool is_mul = in1.k == K::fmul, is_div = in1.k == K::fdiv;
    if (!is_add && !is_sub && !is_mul && !is_div) return false;
    if ((is_mul || is_div) && fp_tiny_before_on()) return false;
    if (in1.rn == in1.rm) return false;
    const unsigned rd0 = in0.rd;
    unsigned src_g = 0;
    if (in1.rm == rd0) {
        src_g = in1.rn;
    } else if ((is_add || is_mul) && in1.rn == rd0) {
        src_g = in1.rm;   // expand_fp_imm is always finite (never NaN)
    } else {
        return false;
    }
    if (in1.rd != rd0 && !e.ctx.vec_dead_after(2, rd0)) return false;
    const Xmm src = e.vec(src_g);
    const Xmm d = e.dest(in1.rd);
    const FOps& f = is_add ? fops::add : is_sub ? fops::sub : is_mul ? fops::mul : fops::div;
    e.op(s.pick(f), d, src, Opnd::at(e.ksplat(s.es, in0.imm)));
    return true;
}

bool emit_faddp_reduce(FpEmit& e, const Inst& in0, const Inst& in1) {
    if (!e.avx() || fp_tiny_before_on()) return false;
    if (in1.k != K::faddp || !in1.sh.scalar || in1.sh.es != 32 || in1.rn != in0.rd) return false;
    if (in1.rd != in0.rd && !e.ctx.vec_dead_after(2, in0.rd)) return false;
    const Xmm n = e.vec(in0.rn);
    const Xmm ev = e.tmp(), od = e.tmp();
    const Xmm d = e.dest(in1.rd);
    e.as.vex_rr(vop::vpermilps, false, x64::id(ev), 0, x64::id(n));
    e.as.db(0x88);   // [n0, n2, n0, n2]
    e.as.vex_rr(vop::vpermilps, false, x64::id(od), 0, x64::id(n));
    e.as.db(0xdd);   // [n1, n3, n1, n3]
    e.op(op::addps, ev, ev, od);             // [n0+n1, n2+n3, n0+n1, n2+n3]
    e.op_i(op::insertps, od, ev, ev, 0x4e);  // [n2+n3, 0, 0, 0]
    e.op(op::addss, d, od, ev);              // [(n2+n3)+(n0+n1), 0, 0, 0]
    return true;
}

namespace {

unsigned fp_fact_want(std::span<const std::uint32_t> words, FamilyFactWant* out) {
    if (words.empty() || !fp_on(fp_no_slot)) return 0;
    Inst in;
    if (!decode(words[0], in) || in.index < 0 || in.sh.scalar || !in.sh.q) return 0;
    if (in.sh.es != 32 && in.sh.es != 64) return 0;
    if (in.k != K::fmul && in.k != K::fmulx && in.k != K::fmla && in.k != K::fmls) return 0;
    const unsigned kind = (in.sh.es == 32 ? 24u : 28u) + static_cast<unsigned>(in.index);
    out[0] = {FamilyFactWant::K::derived, in.rm, {kind, 0}};
    return 1;
}

void fp_fact_derive(EmitContext& ctx, Xmm src, unsigned kind, const Mem& out) {
    FpEmit e(ctx);
    const Xmm t = e.tmp();
    if (kind >= 24 && kind <= 27) {
        const auto imm = static_cast<std::uint8_t>((kind - 24) * 0x55);
        if (e.avx()) {
            e.as.vex_rr(vop::vpermilps, false, x64::id(t), 0, x64::id(src));
            e.as.db(imm);
        } else {
            e.un_i(op::pshufd, t, src, imm);
        }
    } else if (kind == 28) {
        e.un(op::movddup, t, src);
    } else if (kind == 29) {
        e.un_i(op::pshufd, t, src, 0xee);
    } else {
        return;
    }
    e.as.movups(out, t);
}

} // namespace

const FamilyFacts* fp_family_facts() {
    static const FamilyFacts facts{&fp_fact_want, nullptr, nullptr, &fp_fact_derive};
    return &facts;
}

bool emit_elem_pair(FpEmit& e, const Inst& in0, const Inst& in1) {
    if (!e.avx() || fp_tiny_before_on()) return false;
    const auto ok_kind = [&](K k) { return k == K::fmul || ((k == K::fmla || k == K::fmls) && e.feat.fma); };
    if (!ok_kind(in0.k) || !ok_kind(in1.k)) return false;
    const Shape s = in0.sh;
    if (s.scalar || !s.q || (s.es != 32 && s.es != 64) || in0.index < 0) return false;
    if (in1.sh.scalar || !in1.sh.q || in1.sh.es != s.es || in1.rm != in0.rm || in1.index != in0.index) return false;
    // Require no RAW, WAR, or WAW hazards between in0 and in1 (self-update rd == rn is allowed).
    if (in0.rd == in0.rm || in1.rd == in0.rm || in0.rd == in1.rd || in0.rd == in1.rn || in1.rd == in0.rn) return false;
    // If Tier 2 precomputed the broadcast element, let each op take the direct
    // memory operand (2 instructions, 0 shuffles, 0 temp XMMs vs 3 instructions).
    if (Mem dummy{}; fp_on(fp_no_slot) &&
        e.ctx.vec_derived(in0.rm, (s.es == 32 ? 24u : 28u) + static_cast<unsigned>(in0.index), dummy))
        return false;
    const Opnd b = element_op(e, s, in0.rm, in0.index);
    const bool dbl = s.dbl();
    const auto emit_one = [&](const Inst& in) {
        if (in.k == K::fmul) {
            const Opnd n = e.src(in.rn);
            const Xmm d = e.ctx.write_vec(in.rd);
            if (n.is_mem) e.op(s.pick(fops::mul), d, b.reg, n);
            else e.op(s.pick(fops::mul), d, n.reg, b.reg);
            return;
        }
        const Opnd acc = e.src(in.rd);
        const Opnd n = e.src(in.rn);
        const Xmm d = e.ctx.write_vec(in.rd);
        const FmaKind kind = in.k == K::fmla ? FmaKind::fmadd : FmaKind::fnmadd;
        if (!acc.is_mem && d == acc.reg) {
            e.fma(kind, FmaForm::f231, dbl, false, d, b.reg, n);
        } else if (acc.is_mem) {
            e.un(op::movups, d, acc);
            e.fma(kind, FmaForm::f231, dbl, false, d, b.reg, n);
        } else if (!n.is_mem && d == n.reg) {
            e.fma(kind, FmaForm::f213, dbl, false, d, b.reg, acc);
        } else {
            e.mov(d, acc.reg);
            e.fma(kind, FmaForm::f231, dbl, false, d, b.reg, n);
        }
    };
    emit_one(in0);
    emit_one(in1);
    return true;
}

bool emit_fused(FpEmit& e, const Inst& in) {
    const Shape s = in.sh;
    if (s.es == 16) return emit_fused_h(e, in);
    if (fp_tiny_before_on() && emit_arith_tiny_before(e, in)) return true;
    const bool dbl = s.dbl();
    const unsigned w = s.width();
    // The product n * m is commutative: one factor in a register, the other
    // a register or memory operand. Guest index of each (-1: element temp).
    struct Factors { Xmm f1; int f1g; Opnd f2; int f2g; };
    // A factor in memory that is also the destination is loaded now: after dest() its guest register reads
    // as written (fmsub s0, s0, s0, s29 with s0 in memory).
    const auto factors = [&](Opnd n, int ng, Opnd m, int mg, unsigned rd) {
        if (n.is_mem && !m.is_mem) { std::swap(n, m); std::swap(ng, mg); }
        if (n.is_mem) n = Opnd::of(e.vec(static_cast<unsigned>(ng)));   // both in memory (ng is a guest)
        if (m.is_mem && mg == static_cast<int>(rd)) m = Opnd::of(e.vec(rd));
        return Factors{n.reg, ng, m, mg};
    };
    if (in.k == K::fmla || in.k == K::fmls) {
        const Opnd acc = e.src(in.rd);
        const Opnd n = e.src(in.rn);
        const Opnd m = in.index >= 0 ? element_op(e, s, in.rm, in.index) : e.src(in.rm);
        const int mg = in.index < 0 || (s.scalar && in.index == 0) ? in.rm : -1;
        const auto [f1, f1g, f2, f2g] = factors(n, in.rn, m, mg, in.rd);
        // FMLS: acc + (-n) * m, a single rounding of acc - n*m.
        const FmaKind kind = in.k == K::fmla ? FmaKind::fmadd : FmaKind::fnmadd;
        const Xmm d = e.dest(in.rd);
        // 64-bit FMLA with every source zero above bit 64: +0 + 0 * 0 = +0.
        bool zero = w == 128 || (w == 64 && in.k == K::fmla && in.index < 0 && e.zero_above(in.rd, 64) &&
                                 e.zero_above(in.rn, 64) && e.zero_above(in.rm, 64));
        if (!acc.is_mem && d == acc.reg) {
            e.fma(kind, FmaForm::f231, dbl, s.scalar, d, f1, f2);
            zero = zero || (s.scalar && e.zero_above(in.rd, w));
        } else if (acc.is_mem) {   // d = acc (scalar loads zero the upper lanes), then accumulate
            e.un(s.scalar ? (dbl ? op::movsd : op::movss) : op::movups, d, acc);
            e.fma(kind, FmaForm::f231, dbl, s.scalar, d, f1, f2);
            zero = zero || s.scalar;
        } else if (d == f1) {   // d aliases a factor (aliasing contexts)
            // f2 in memory is either a guest register or a derived broadcast slot (f2g < 0).
            const Xmm f2r = f2.is_mem ? (f2g >= 0 ? e.vec(static_cast<unsigned>(f2g)) : load_opnd(e, f2, s)) : f2.reg;
            e.fma(kind, FmaForm::f213, dbl, s.scalar, d, f2r, acc);
            zero = zero || (s.scalar && f1g >= 0 && e.zero_above(static_cast<unsigned>(f1g), w));
        } else if (!f2.is_mem && d == f2.reg) {
            e.fma(kind, FmaForm::f213, dbl, s.scalar, d, f1, acc);
        } else {
            e.mov(d, acc.reg);
            e.fma(kind, FmaForm::f231, dbl, s.scalar, d, f1, f2);
            zero = zero || (s.scalar && e.zero_above(in.rd, w));
        }
        if (!zero) e.zero_upper(d, w);
        return true;
    }
    // FMADD a+n*m, FMSUB a-n*m, FNMADD -a-n*m, FNMSUB -a+n*m.
    FmaKind kind;
    switch (in.k) {
    case K::fmadd: kind = FmaKind::fmadd; break;
    case K::fmsub: kind = FmaKind::fnmadd; break;
    case K::fnmadd: kind = FmaKind::fnmsub; break;
    default: kind = FmaKind::fmsub; break;
    }
    const Opnd a = e.src(in.ra), n = e.src(in.rn), m = e.src(in.rm);
    const auto [f1, f1g, f2, f2gi] = factors(n, in.rn, m, in.rm, in.rd);
    const auto f2g = static_cast<unsigned>(f2gi);
    const Xmm d = e.dest(in.rd);
    const SseOp load = dbl ? op::movsd : op::movss;   // scalar loads zero the upper lanes
    if (!a.is_mem && d == a.reg) {
        e.fma(kind, FmaForm::f231, dbl, true, d, f1, f2);
        if (!e.zero_above(in.ra, w)) e.zero_upper(d, w);
    } else if (d == f1) {   // d = f1 * f2 + a
        e.fma(kind, FmaForm::f213, dbl, true, d, f2.is_mem ? e.vec(f2g) : f2.reg, a);
        if (!e.zero_above(static_cast<unsigned>(f1g), w)) e.zero_upper(d, w);
    } else if (!f2.is_mem && d == f2.reg) {
        e.fma(kind, FmaForm::f213, dbl, true, d, f1, a);
        if (!e.zero_above(f2g, w)) e.zero_upper(d, w);
    } else if (a.is_mem) {
        e.un(load, d, a);
        e.fma(kind, FmaForm::f231, dbl, true, d, f1, f2);
    } else if (e.zero_above(in.ra, w)) {
        e.mov(d, a.reg);
        e.fma(kind, FmaForm::f231, dbl, true, d, f1, f2);
    } else if (f2.is_mem) {   // d = f2 (loaded) * f1 + a
        e.un(load, d, f2);
        e.fma(kind, FmaForm::f213, dbl, true, d, f1, a);
    } else {
        if (dbl) e.movq(d, a.reg);
        else e.ins0(d, a.reg);
        e.fma(kind, FmaForm::f231, dbl, true, d, f1, f2);
    }
    return true;
}

// ---- unary ------------------------------------------------------------------------------------

bool emit_unary_h(FpEmit& e, const Inst& in);

bool emit_unary(FpEmit& e, const Inst& in) {
    const Shape s = in.sh;
    const unsigned w = s.width();
    switch (in.k) {
    case K::fmov: {   // scalar only
        const Opnd n = e.src(in.rn);
        if (s.es != 16 && n.is_mem) {   // movss/movsd loads zero the upper lanes
            const Xmm d = e.dest(in.rd);
            e.un(s.es == 64 ? op::movsd : op::movss, d, n);
            return true;
        }
        const Xmm nr = e.vec(in.rn);
        if (e.zero_above(in.rn, w)) {
            const Xmm d = e.dest(in.rd);
            e.mov(d, nr);
            return true;
        }
        e.commit(in.rd, nr, w);
        return true;
    }
    case K::fabs: case K::fneg: {
        const bool neg = in.k == K::fneg;
        if (e.avx()) {
            // One AND/XOR with a pooled mask. Scalar masks have zero upper
            // lanes: FABS zeroes the result's upper lanes, FNEG keeps n's.
            const std::uint64_t bits = neg ? FpEmit::sign_bits(s.es) : FpEmit::abs_bits(s.es);
            const Mem mask = s.scalar ? e.kscalar(s.es, bits) : e.ksplat(s.es, bits);
            const SseOp o = neg ? op::xorps : op::andps;
            const Opnd n = e.src(in.rn);
            const Xmm d = e.dest(in.rd);
            bool zero = w == 128 || (s.scalar && !neg);
            if (n.is_mem) {
                if (s.scalar && s.es != 16) {   // scalar load: zero upper lanes
                    e.un(s.es == 64 ? op::movsd : op::movss, d, n);
                    e.op(o, d, d, Opnd::at(mask));
                    zero = true;
                } else {
                    e.un(op::movups, d, Opnd::at(mask));
                    e.op(o, d, d, n);
                }
            } else {
                e.op(o, d, n.reg, Opnd::at(mask));
                if (s.scalar && neg) zero = e.zero_above(in.rn, w);
            }
            if (!zero) e.zero_upper(d, w);
            return true;
        }
        const Xmm n = e.vec(in.rn);
        const Xmm m = e.tmp();
        if (!neg) {
            if (s.scalar) {
                e.scalar_const(s.es, m, abs_bits(s.es));   // upper lanes zero
                final_op(e, in.rd, 128, op::andps, m, n, true);
                return true;
            }
            if (s.es == 16) { e.ones(m); e.shift(0x71, 2, m, m, 1); }   // psrlw 1
            else e.abs_mask(s.es, m);
            final_op(e, in.rd, w, op::andps, m, n, true);
            return true;
        }
        if (s.es == 16) { e.ones(m); e.shift(0x71, 6, m, m, 15); }   // psllw 15
        else e.sign_mask(s.es, m);
        if (s.scalar) {
            const Xmm t = e.tmp();
            e.op(op::xorps, t, m, n);
            e.commit(in.rd, t, w);
            return true;
        }
        final_op(e, in.rd, w, op::xorps, m, n, true);
        return true;
    }
    default:
        break;
    }
    if (s.es == 16) return emit_unary_h(e, in);
    switch (in.k) {
    case K::fsqrt: case K::frint: {
        const bool sqrt = in.k == K::fsqrt;
        if (!sqrt && in.rnd == Rnd::a) {
            const Xmm n = e.vec(in.rn);
            const Xmm T[4] = {e.tmp(), e.tmp(), e.tmp(), e.tmp()};
            const Xmm out = e.dest(in.rd);
            emit_round_away(e, s, out, n, false, T);
            e.zero_upper(out, w);
            return true;
        }
        // When every lane of Vn holds the exact same value (e.g. from an earlier
        // DUP.4S/.2D even with intervening instructions), scalar vsqrtss/vsqrtsd
        // + broadcast is bit-exact, FPSR-exact, and 2x faster on E-cores.
        if (sqrt && !s.scalar && s.q && e.feat.avx2 && ((e.ctx.vec_broadcast(s.es) >> in.rn) & 1u) != 0) {
            const Opnd n = e.src(in.rn);
            const Xmm d = e.dest(in.rd);
            if (s.es == 32) {
                if (n.is_mem) {
                    e.un(op::movss, d, n);
                    e.as.vex_rr(vex_of(op::sqrtss), false, x64::id(d), x64::id(d), x64::id(d));
                } else {
                    e.as.vex_rr(vex_of(op::sqrtss), false, x64::id(d), x64::id(n.reg), x64::id(n.reg));
                }
                e.as.vex_rr(vop::vpbroadcastd, false, x64::id(d), 0, x64::id(d));
            } else {
                if (n.is_mem) {
                    e.un(op::movsd, d, n);
                    e.as.vex_rr(vex_of(op::sqrtsd), false, x64::id(d), x64::id(d), x64::id(d));
                } else {
                    e.as.vex_rr(vex_of(op::sqrtsd), false, x64::id(d), x64::id(n.reg), x64::id(n.reg));
                }
                e.as.vex_rr(vex_of(op::movddup), false, x64::id(d), 0, x64::id(d));
            }
            e.ctx.note_vec_broadcast(in.rd, s.es);
            return true;
        }
        const SseOp o = s.pick(sqrt ? fops::sqrt : fops::round);
        const int imm = sqrt ? -1 : frint_imm(in.rnd);
        if (s.scalar && e.avx()) {
            // d[0] = OP(n[0]); the upper lanes come from the first source:
            // n itself when it has zeros there, else a zeroed temp.
            const Opnd n = e.src(in.rn);
            Xmm upper;
            if (!n.is_mem && e.zero_above(in.rn, w)) {
                upper = n.reg;
            } else {
                upper = e.tmp();
                e.zero(upper);
            }
            const Xmm d = e.dest(in.rd);
            if (imm >= 0) e.sc_u_i(o, d, upper, n, static_cast<std::uint8_t>(imm));
            else e.sc_u(o, d, upper, n);
            return true;
        }
        if (s.scalar) {
            const Xmm n = e.vec(in.rn);
            const Xmm d = e.dest(in.rd);
            if (imm >= 0) e.sc_i(o, d, n, static_cast<std::uint8_t>(imm));
            else e.sc(o, d, n);
            e.zero_upper(d, w);
            return true;
        }
        const Opnd n = e.src(in.rn);
        const Xmm d = e.dest(in.rd);
        if (imm >= 0) e.un_i(o, d, n, static_cast<std::uint8_t>(imm));
        else e.un(o, d, n);
        e.zero_upper(d, w);
        return true;
    }
    default:
        return false;
    }
}

// ---- FRINT32/FRINT64 (FEAT_FRINTTS) ------------------------------------------------------------

// r = round(x): the FPCR mode (X) or toward zero (Z), precision exceptions
// enabled (FPRoundIntN raises IXC for an inexact in-range result in both
// forms); unused lanes are zero. 32-bit range: cvtt(r) is INT_MIN with IE
// (IOC) exactly for NaN, infinite and out-of-range lanes, converting back
// gives -2^31 there and r elsewhere (exact), and OR-ing r's sign restores -0.
// 64-bit range: compares select -2^63, and cvtt of the all-ones (NaN) mask
// raises IE for those lanes. FZ: DAZ flushes the input.
bool emit_frint_n(FpEmit& e, const Inst& in) {
    const Shape s = in.sh;
    const bool dbl = s.dbl();
    const unsigned es = s.es;
    const std::uint8_t imm = in.rnd == Rnd::x ? 0x04 : 0x03;
    const std::uint64_t sign = FpEmit::sign_bits(es);
    const Xmm r = e.tmp(), t = e.tmp(), u = e.tmp();
    if (s.scalar) {
        if (e.avx()) {   // upper lanes from n when it has zeros there, else a zeroed temp
            const Opnd n = e.src(in.rn);
            Xmm upper = u;
            if (!n.is_mem && e.zero_above(in.rn, s.width())) upper = n.reg;
            else e.zero(u);
            e.sc_u_i(s.pick(fops::round), r, upper, n, imm);
        } else {
            const Xmm x = e.vec(in.rn);
            if (dbl) e.movq(r, x);
            else e.ins0(r, x);
            e.sc_i(s.pick(fops::round), r, r, imm);
        }
    } else if (!s.q) {
        e.movq(r, e.vec(in.rn));   // clean upper lanes: no spurious IOC/IXC
        e.round(s, r, r, imm);
    } else {
        e.un_i(s.pick(fops::round), r, e.src(in.rn), imm);
    }
    if (in.ibits == 32) {
        if (dbl) {
            e.un(op::cvttpd2dq, t, r);
            e.un(op::cvtdq2pd, t, t);
        } else {
            e.un(op::cvttps2dq, t, r);
            e.un(op::cvtdq2ps, t, t);
        }
        if (e.avx()) {
            e.op(s.pick(fops::and_), u, r, Opnd::at(e.ksplat(es, sign)));
        } else {
            e.sign_mask(es, u);
            e.op(s.pick(fops::and_), u, u, r);
        }
        const Xmm d = e.dest(in.rd);
        e.op(s.pick(fops::or_), d, t, u);
        return true;
    }
    const std::uint64_t limit = dbl ? 0x43e0000000000000ULL : 0x5f000000ULL;   // 2^63
    // Packed compares also for scalars: r's unused lanes are zero (in range).
    const SseOp cmp = dbl ? op::cmppd : op::cmpps;
    if (e.avx()) {
        e.op_i(cmp, t, r, Opnd::at(e.ksplat(es, limit)), pred_nlt);          // >= 2^63 or NaN
        e.op_i(cmp, u, r, Opnd::at(e.ksplat(es, limit | sign)), pred_lt);    // < -2^63
    } else {
        e.splat(es, t, limit);
        e.op_i(cmp, t, t, r, pred_le);                                       // 2^63 <= r
        e.splat(es, u, limit | sign);
        e.op_i(cmp, u, u, r, pred_nle);                                      // !(-2^63 <= r): < -2^63 or NaN
    }
    e.op(s.pick(fops::or_), t, t, u);
    e.un(dbl ? op::cvttpd2dq : op::cvttps2dq, u, t);   // IE (IOC) iff a lane is out of range
    e.splat(es, u, limit | sign);
    const Xmm scratch = e.avx() ? u : e.tmp();
    const Xmm d = e.dest(in.rd);
    e.blend(dbl, d, r, u, t, scratch);
    return true;
}

// ---- FMLAL/FMLSL (FEAT_FHM) ---------------------------------------------------------------------

// Vd.S[i] = Vd.S[i] + (-)Vn.H[part + i] * Vm.H[part + i or index] with one
// rounding (FPMulAddH): the F16C widening is exact and a product of two
// halves is exact in single precision, so one fp32 FMA gives the A64 result.
// FPCR.FZ applies to the fp32 addend and result (DAZ/FTZ); half inputs are
// not flushed (FPCR.FZ16 is not modelled; vcvtph2ps ignores DAZ).
bool emit_fhm(FpEmit& e, const Inst& in) {
    const bool q = in.sh.q;
    const unsigned part = in.upper ? (q ? 4u : 2u) : 0u;
    const Opnd acc = e.src(in.rd);
    const Xmm A = e.tmp(), B = e.tmp();
    // t = the halves of Vg from element `first` (4 for Q, else 2) as singles,
    // other lanes zero.
    const auto widen = [&](Xmm t, unsigned g, unsigned first) {
        const Opnd x = e.src(g);
        if (x.is_mem) {
            Mem m = x.mem;
            m.disp += static_cast<std::int32_t>(2 * first);
            if (q) {
                e.cvtph2ps(t, Opnd::at(m));
            } else {
                e.un(op::movss, t, Opnd::at(m));
                e.cvtph2ps(t, t);
            }
            return;
        }
        if (q) {
            if (first == 0) {
                e.cvtph2ps(t, x.reg);
            } else {
                e.un_i(op::pshufd, t, x.reg, 0xee);
                e.cvtph2ps(t, t);
            }
        } else {
            e.op_i(op::insertps, t, x.reg, x.reg, static_cast<std::uint8_t>(((first / 2) << 6) | 0x0e));
            e.cvtph2ps(t, t);
        }
    };
    widen(A, in.rn, part);
    if (in.index < 0) {
        widen(B, in.rm, part);
    } else {
        const auto index = static_cast<unsigned>(in.index);
        const Opnd m = e.src(in.rm);
        if (m.is_mem && e.feat.avx2) {
            Mem at = m.mem;
            at.disp += static_cast<std::int32_t>(2 * index);
            e.as.vex_rm(VexOp{1, 2, 0x79, false}, false, x64::id(B), 0, at);   // vpbroadcastw
            e.cvtph2ps(B, B);
        } else {
            const Xmm x = e.vec(in.rm);
            if (index >= 4) {
                e.un_i(op::pshufd, B, x, 0xee);
                e.cvtph2ps(B, B);
            } else {
                e.cvtph2ps(B, x);
            }
            e.as.vex_rr(vop::vpermilps, false, x64::id(B), 0, x64::id(B));
            e.as.db(static_cast<std::uint8_t>((index & 3) * 0x55));
        }
        if (!q) e.movq(B, B);   // lanes 2-3: 0 * 0, no spurious exceptions
    }
    const Xmm d = e.dest(in.rd);
    if (acc.is_mem) e.un(q ? op::movups : op::movq_xx, d, acc);
    else if (q) e.mov(d, acc.reg);
    else e.movq(d, acc.reg);   // upper lanes: 0 + 0 * 0 (no spurious exceptions)
    e.fma(in.k == K::fmlsl ? FmaKind::fnmadd : FmaKind::fmadd, FmaForm::f231, false, false, d, A, Opnd::of(B));
    if (!q && in.k == K::fmlsl) e.movq(d, d);   // +0 - 0 * 0 is -0 when rounding toward -inf
    return true;
}

// ---- compares and selects ---------------------------------------------------------------------

namespace {

// Converts the low half-precision lane of x to single precision (exact).
Xmm h_to_s(FpEmit& e, Xmm x) {
    const Xmm t = e.tmp();
    e.cvtph2ps(t, x);
    return t;
}

// ARM-layout NZCV of an x64 (u)comis result: GT 0010, LT 1000, EQ 0110, UN 0011.
void nzcv_from_ucomi(FpEmit& e, Gp v, Gp c) {
    auto& a = e.as;
    a.mov_imm(v, 0x20000000);
    a.mov_imm(c, 0x80000000);
    a.cmov(Cond::b, 32, v, c);
    a.mov_imm(c, 0x60000000);
    a.cmov(Cond::e, 32, v, c);
    a.mov_imm(c, 0x30000000);
    a.cmov(Cond::p, 32, v, c);
}

// Evaluates ARM condition `cond` (0..13) on a 4-bit NZCV immediate (N=8,Z=4,C=2,V=1).
bool a64_cond_holds_imm(unsigned cond, unsigned nzcv4) {
    const bool n = (nzcv4 & 8u) != 0, z = (nzcv4 & 4u) != 0, c = (nzcv4 & 2u) != 0, v = (nzcv4 & 1u) != 0;
    bool holds = true;
    switch (cond >> 1) {
    case 0: holds = z; break;
    case 1: holds = c; break;
    case 2: holds = n; break;
    case 3: holds = v; break;
    case 4: holds = c && !z; break;
    case 5: holds = n == v; break;
    case 6: holds = !z && (n == v); break;
    default: holds = true; break;
    }
    return (cond & 1u) ? !holds : holds;
}

// Sets byte `dst` to 1 iff ARM condition `cond` (0..13) holds on x64 (u)comis(Rn, Rm)
// EFLAGS (GT: Z0P0C0, LT: Z0P0C1, EQ: Z1P0C0, UN: Z1P1C1), else 0.
void ucomi_cond_byte(FpEmit& e, unsigned cond, Gp dst, Gp tmp) {
    auto& a = e.as;
    switch (cond) {
    case 0:   // EQ: e && np
        a.setcc(Cond::e, dst);
        a.setcc(Cond::np, tmp);
        a.alu(x64::Alu::and_, 8, dst, tmp);
        return;
    case 1:   // NE: ne || p
        a.setcc(Cond::ne, dst);
        a.setcc(Cond::p, tmp);
        a.alu(x64::Alu::or_, 8, dst, tmp);
        return;
    case 2: case 5:   // CS/PL: ae || p
        a.setcc(Cond::ae, dst);
        a.setcc(Cond::p, tmp);
        a.alu(x64::Alu::or_, 8, dst, tmp);
        return;
    case 3: case 4:   // CC/MI: b && np
        a.setcc(Cond::b, dst);
        a.setcc(Cond::np, tmp);
        a.alu(x64::Alu::and_, 8, dst, tmp);
        return;
    case 6: a.setcc(Cond::p, dst); return;    // VS
    case 7: a.setcc(Cond::np, dst); return;   // VC
    case 8:   // HI: a || p
        a.setcc(Cond::a, dst);
        a.setcc(Cond::p, tmp);
        a.alu(x64::Alu::or_, 8, dst, tmp);
        return;
    case 9:   // LS: be && np
        a.setcc(Cond::be, dst);
        a.setcc(Cond::np, tmp);
        a.alu(x64::Alu::and_, 8, dst, tmp);
        return;
    case 10: a.setcc(Cond::ae, dst); return;   // GE
    case 11: a.setcc(Cond::b, dst); return;    // LT
    case 12: a.setcc(Cond::a, dst); return;    // GT
    default: a.setcc(Cond::be, dst); return;   // LE
    }
}

} // namespace

bool emit_select(FpEmit& e, const Inst& in) {
    const Shape s = in.sh;
    const bool dbl = s.dbl();
    auto& a = e.as;
    switch (in.k) {
    case K::fcmp: {
        // The context may take the raw (u)comis EFLAGS as NZCV (fcmp_swap /
        // set_fcmp_flags); `swap` compares (m, n) instead of (n, m).
        const bool swap = e.ctx.fcmp_swap();
        Opnd n = e.src(in.rn);
        Opnd m;
        if (in.zero) {
            const Xmm z = e.tmp();
            e.zero(z);
            m = Opnd::of(z);
        } else {
            m = e.src(in.rm);
        }
        if (s.es == 16) {
            n = Opnd::of(h_to_s(e, e.vec(in.rn)));
            if (!in.zero) m = Opnd::of(h_to_s(e, e.vec(in.rm)));
        }
        // The first comis operand must be a register; the second may be memory.
        Opnd first = swap ? m : n, second = swap ? n : m;
        if (first.is_mem) first = Opnd::of(e.vec(swap ? in.rm : in.rn));
        const Gp x = e.gtmp(), y = e.gtmp(), c = e.gtmp();
        e.ucomi(dbl, first.reg, second, in.signal);
        if (e.ctx.set_fcmp_flags(swap)) return true;
        // Integers whose x64 CMP produces the A64 NZCV in the "sub" producer
        // convention: GT (1,0) EQ (0,0) LT (0,1) UN (0x80000000,1). Built
        // without touching the comis EFLAGS until the final CMP.
        a.mov_imm(x, 0);
        a.mov_imm(y, 0);
        a.setcc(swap ? Cond::b : Cond::a, x);
        a.setcc(swap ? Cond::a : Cond::b, y);
        a.mov_imm(c, 0x80000000);
        a.cmov(Cond::p, 32, x, c);
        a.cmp(32, x, y);
        e.ctx.set_flags(FlagsProducer::sub);
        return true;
    }
    case K::fccmp: {
        Xmm n = e.vec(in.rn);
        Opnd m = s.es == 16 ? Opnd::of(e.vec(in.rm)) : e.src(in.rm);
        if (s.es == 16) {
            n = h_to_s(e, n);
            m = Opnd::of(h_to_s(e, m.reg));
        }
        const Gp v = e.gtmp(), c = e.gtmp();
        if (in.cond >= 14) {
            e.ucomi(dbl, n, m, in.signal);
            if (e.ctx.set_fcmp_flags(false)) return true;
            nzcv_from_ucomi(e, v, c);
            e.ctx.set_nzcv(v);
            return true;
        }
        // Evaluate incoming condition directly from live host EFLAGS before
        // ucomi clobbers them: avoids 7-insn NZCV packing + STL stall, and
        // skips ucomi when in.cond is false (exact A64 FCCMPE NaN semantics).
        if (const int c2 = e.ctx.fccmp_consumer_cond(); c2 >= 0) {
            const bool k = a64_cond_holds_imm(static_cast<unsigned>(c2), in.nzcv);
            const Cond cc = e.ctx.condition(in.cond);
            const auto use_imm = a.new_label(), done = a.new_label();
            a.jcc_short(x64::invert(cc), use_imm);
            e.ucomi(dbl, n, m, in.signal);
            ucomi_cond_byte(e, static_cast<unsigned>(c2), v, c);
            a.jmp_short(done);
            a.bind(use_imm);
            a.mov_imm(v, k ? 1 : 0);
            a.bind(done);
            a.test(8, v, v);
            e.ctx.set_fccmp_bool();
            return true;
        }
        const Cond cc = e.ctx.condition(in.cond);
        const auto use_imm = a.new_label(), done = a.new_label();
        a.jcc_short(x64::invert(cc), use_imm);
        e.ucomi(dbl, n, m, in.signal);
        nzcv_from_ucomi(e, v, c);
        a.jmp_short(done);
        a.bind(use_imm);
        a.mov_imm(v, static_cast<std::uint64_t>(in.nzcv) << 28);
        a.bind(done);
        e.ctx.set_nzcv(v);
        return true;
    }
    case K::fcsel: {
        // On AVX for S/D with at least one source already in an XMM register,
        // select in the vector domain with vblendvps/vblendvpd so FP values
        // avoid 2-3 XMM<->GPR domain crossings on the critical path.
        // Because vmovd/vmovq zero bits [127:w] of the mask, vblendv takes its
        // upper lanes from its first operand (a); placing a zero-upper operand
        // in `a` makes A64 upper-lane zeroing free.
        const Opnd n = e.src(in.rn), m = e.src(in.rm);
        const unsigned bits = dbl ? 64 : 32;
        if (e.avx() && s.es >= 32 && (!n.is_mem || !m.is_mem)) {
            const unsigned w = s.width();
            if (in.cond >= 14) {
                const Xmm d = e.dest(in.rd);
                if (n.is_mem) e.un(dbl ? op::movsd : op::movss, d, n);
                else if (e.zero_above(in.rn, w)) e.mov(d, n.reg);
                else if (dbl) e.movq(d, n.reg);
                else e.ins0(d, n.reg);
                return true;
            }
            const bool n_zero = n.is_mem || e.zero_above(in.rn, w);
            const bool m_zero = m.is_mem || e.zero_above(in.rm, w);
            const bool swap = (!m_zero && n_zero) || (m_zero == n_zero && m.is_mem && !n.is_mem);
            const Opnd& a_op = swap ? n : m;
            const Opnd& b_op = swap ? m : n;
            const bool need_zero = !(swap ? n_zero : m_zero);
            const Cond cc = swap ? x64::invert(e.ctx.condition(in.cond)) : e.ctx.condition(in.cond);
            const Gp g = e.gtmp();
            a.mov_imm(g, 0);
            a.cmov(cc, bits, g, e.kconst(~0ULL, ~0ULL));
            const Xmm mask = e.tmp();
            if (dbl) e.movq(mask, g);
            else e.movd(mask, g);
            const Xmm ar = load_opnd(e, a_op, s);
            const Xmm d = e.dest(in.rd);
            const VexOp blend = dbl ? vop::vblendvpd : vop::vblendvps;
            if (b_op.is_mem) a.vex_is4(blend, d, ar, b_op.mem, mask);
            else a.vex_is4(blend, d, ar, b_op.reg, mask);
            if (need_zero) e.zero_upper(d, w);
            return true;
        }
        // SSE, FP16, or both sources in memory: select in GPRs.
        const Gp gn = e.gtmp(), gm = e.gtmp();
        if (!n.is_mem) { if (dbl) e.movq(gn, n.reg); else e.movd(gn, n.reg); }
        if (m.is_mem) a.mov(bits, gm, m.mem);
        else if (dbl) e.movq(gm, m.reg);
        else e.movd(gm, m.reg);
        if (in.cond < 14) {
            const Cond cc = e.ctx.condition(in.cond);
            if (n.is_mem) a.cmov(cc, bits, gm, n.mem);
            else a.cmov(cc, bits, gm, gn);
        } else if (n.is_mem) {
            a.mov(bits, gm, n.mem);
        } else {
            a.mov(bits, gm, gn);
        }
        if (s.es == 16) a.movzx(32, gm, gm, 16);
        const Xmm d = e.dest(in.rd);
        if (dbl) e.movq(d, gm);
        else e.movd(d, gm);
        return true;
    }
    default:
        return false;
    }
}

// ---- moves --------------------------------------------------------------------------------------

bool emit_move(FpEmit& e, const Inst& in) {
    const Shape s = in.sh;
    auto& a = e.as;
    switch (in.k) {
    case K::fmov_imm: {
        if (e.avx() && fp_on(fp_old_imm)) {   // one load from the pool: {imm, 0}, upper lanes zero
            const Xmm d = e.dest(in.rd);
            e.un(op::movups, d, Opnd::at(e.kscalar(s.es == 64 ? 64 : 32, in.imm)));
            return true;
        }
        const Gp g = e.gtmp();
        a.mov_imm(g, in.imm);
        const Xmm d = e.dest(in.rd);
        if (s.es == 64) e.movq(d, g);
        else e.movd(d, g);
        return true;
    }
    case K::fmov_vimm: {
        std::uint64_t v = in.imm;
        if (s.es == 16) v = (v & 0xffff) * 0x0001000100010001ULL;
        else if (s.es == 32) v = (v & 0xffffffff) * 0x0000000100000001ULL;
        if (!s.q && e.avx() && fp_on(fp_old_imm)) {
            const Xmm d = e.dest(in.rd);
            e.un(op::movups, d, Opnd::at(e.kconst(v, 0)));
            return true;
        }
        if (!s.q) {
            const Gp g = e.gtmp();
            a.mov_imm(g, v);
            const Xmm d = e.dest(in.rd);
            e.movq(d, g);
            return true;
        }
        const Xmm d = e.dest(in.rd);
        if (s.es == 32) e.splat(32, d, v & 0xffffffff);
        else e.splat(64, d, v);
        return true;
    }
    case K::fmov_to_gpr: {
        const Opnd n = e.src(in.rn);
        const Gp g = e.gdest(in.rd);
        if (n.is_mem) {
            if (s.es == 16) a.movzx(32, g, n.mem, 16);
            else a.mov(s.es == 64 ? 64 : 32, g, n.mem);
        } else if (s.es == 64) {
            e.movq(g, n.reg);
        } else if (s.es == 32) {
            e.movd(g, n.reg);
        } else {
            a.pextr(16, g, n.reg, 0);
        }
        return true;
    }
    case K::fmov_from_gpr: {
        if (s.es != 16) {   // a general register in memory: one zero-extending vector load
            const auto g = e.ctx.gsrc(in.rn);
            if (g.is_mem) {
                const Xmm d = e.dest(in.rd);
                if (e.avx()) a.vex_rm(VexOp{1, 1, 0x6e, s.es == 64}, false, x64::id(d), 0, g.mem);
                else a.sse_raw(SseOp{0x66, 1, 0x6e}, s.es == 64, x64::id(d), g.mem);
                return true;
            }
        }
        const Gp r = e.gpr(in.rn);
        if (s.es == 16) {
            const Gp t = e.gtmp();
            a.movzx(32, t, r, 16);
            const Xmm d = e.dest(in.rd);
            e.movd(d, t);
            return true;
        }
        const Xmm d = e.dest(in.rd);
        if (s.es == 64) e.movq(d, r);
        else e.movd(d, r);
        return true;
    }
    case K::fmov_to_gpr_hi: {
        const Opnd n = e.src(in.rn);
        const Gp g = e.gdest(in.rd);
        if (n.is_mem) {
            Mem hi = n.mem;
            hi.disp += 8;
            a.mov(64, g, hi);
        } else {
            a.pextr(64, g, n.reg, 1);
        }
        return true;
    }
    case K::fmov_from_gpr_hi: {
        const Xmm acc = e.vec(in.rd);
        const Gp r = e.gpr(in.rn);
        const Xmm d = e.dest(in.rd);
        if (e.avx()) {
            a.vex_rr(VexOp{1, 3, 0x22, true}, false, x64::id(d), x64::id(acc), x64::id(r));
            a.db(1);
        } else {
            e.mov(d, acc);
            a.pinsr(64, d, r, 1);
        }
        return true;
    }
    default:
        return false;
    }
}

// ---- system registers ---------------------------------------------------------------------------

bool emit_sysreg(FpEmit& e, const Inst& in) {
    auto& a = e.as;
    const Mem fpcr = ctx_mem(kFpcrDisp), fpsr = ctx_mem(kFpsrDisp);
    switch (in.k) {
    case K::mrs_fpcr: {
        const Gp g = e.gdest(in.rd);
        a.mov(32, g, fpcr);
        return true;
    }
    case K::msr_fpcr: {
        const Gp v = e.gpr(in.rd);
        const Gp m = e.gtmp(), t = e.gtmp(), u = e.gtmp();
        e.ctx.clobber_host_flags();
        // Current MXCSR status bits (FPCR's field is the scratch slot).
        a.stmxcsr(fpcr);
        a.mov(32, m, fpcr);
        a.and_(32, m, 0x3f);
        a.or_(32, m, 0x1f80);
        // RMode (bits 23:22) -> RC (bits 14:13) with the two bits swapped.
        a.mov(32, t, v);
        a.shr(32, t, 22);
        a.and_(32, t, 3);
        a.mov(32, u, t);
        a.shr(32, u, 1);
        a.xor_(32, u, t);
        a.and_(32, u, 1);            // u = b0 ^ b1
        a.lea(32, u, x64::ptr(u, u, 2));   // u * 3
        a.xor_(32, t, u);            // swapped
        a.shl(32, t, 13);
        a.or_(32, m, t);
        // FZ (bit 24) -> FTZ | DAZ
        a.mov(32, t, v);
        a.shr(32, t, 24);
        a.and_(32, t, 1);
        a.imul(32, t, t, 0x8040);
        a.or_(32, m, t);
        a.mov(32, fpcr, m);
        a.ldmxcsr(fpcr);
        a.mov(32, fpcr, v);
        return true;
    }
    case K::mrs_fpsr: {
        const Gp c = e.gtmp(), m = e.gtmp(), t = e.gtmp();
        e.ctx.clobber_host_flags();
        a.mov(32, c, fpcr);
        a.stmxcsr(fpcr);
        a.mov(32, m, fpcr);
        a.mov(32, fpcr, c);
        // IE -> IOC, ZE/OE/UE/PE (bits 2..5) -> DZC/OFC/UFC/IXC (bits 1..4)
        a.mov(32, t, m);
        a.shr(32, t, 1);
        a.and_(32, t, 0x1e);
        a.and_(32, m, 1);
        a.or_(32, m, t);
        a.alu(x64::Alu::or_, 32, m, fpsr);
        const Gp g = e.gdest(in.rd);
        a.mov(32, g, m);
        return true;
    }
    case K::msr_fpsr: {
        const Gp v = e.gpr(in.rd);
        const Gp c = e.gtmp(), m = e.gtmp(), t = e.gtmp();
        e.ctx.clobber_host_flags();
        a.mov(32, fpsr, v);
        // status = IOC | (DZC/OFC/UFC/IXC << 1)
        a.mov(32, t, v);
        a.and_(32, t, 0x1e);
        a.shl(32, t, 1);
        a.mov(32, m, v);
        a.and_(32, m, 1);
        a.or_(32, t, m);
        a.mov(32, c, fpcr);
        a.stmxcsr(fpcr);
        a.mov(32, m, fpcr);
        a.and_(32, m, ~0x3f);
        a.or_(32, m, t);
        a.mov(32, fpcr, m);
        a.ldmxcsr(fpcr);
        a.mov(32, fpcr, c);
        return true;
    }
    default:
        return false;
    }
}

} // namespace transmute::fx::fpx
