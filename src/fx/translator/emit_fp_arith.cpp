// fp family: arithmetic, min/max, compares, pairwise, reductions, fused
// multiply-add, unary operations, selects and moves (owner: fp).
#include "fx/translator/emit_fp_internal.hpp"

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
    // An element other than a scalar's lane 0 comes straight from a current
    // context slot (one load: movss/movsd/vbroadcastss/movddup) even when a
    // clean copy is in a register (one shuffle saved; EmitContext::vec_slot).
    Mem slot;
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
// translation time (EmitContext::vec_fp_const) that is finite and normal in
// every element, so never NaN, zero or denormal, and the other operand x can
// hold no signalling NaN (EmitContext::vec_fp_quiet: an FP arithmetic result
// or a constant): one x64 min/max, bit-exact for every x with FPCR.DN = 0.
// x64 returns its second operand on NaN or equality, so FMAXNM/FMINNM put the
// constant second (a quiet NaN x yields c, as A64 does) and FMAX/FMIN put x
// second (a quiet NaN x is returned unchanged: A64 returns it quieted, which
// is the same bits). A tie x == c has no +-0 case (c is non-zero) and returns
// equal bits; a denormal x under FZ is flushed by both lowerings.
// TRANSMUTE_FX_FP=noconstminmax.
bool emit_minmax_const(FpEmit& e, const Inst& in, Shape s, unsigned w) {
    const unsigned es = s.es;
    const auto normal = [es](std::uint64_t v) {
        const unsigned exp = es == 32 ? static_cast<unsigned>((v >> 23) & 0xffu) : static_cast<unsigned>((v >> 52) & 0x7ffu);
        return exp != 0 && exp != (es == 32 ? 0xffu : 0x7ffu);
    };
    const unsigned span = s.scalar ? es : w;
    const auto usable = [&](unsigned c_reg, unsigned x_reg) {
        std::uint64_t c = 0;
        return e.ctx.vec_fp_const(c_reg, es, span, c) && normal(c) && e.ctx.vec_fp_quiet(x_reg, es);
    };
    unsigned cr = in.rm, xr = in.rn;
    if (!usable(in.rm, in.rn)) {
        if (!usable(in.rn, in.rm)) return false;
        cr = in.rn;
        xr = in.rm;
    }
    const bool nm = in.k == K::fmaxnm || in.k == K::fminnm;
    const bool is_max = in.k == K::fmax || in.k == K::fmaxnm;
    const Xmm x = e.vec(xr), cv = e.vec(cr);
    const unsigned first = nm ? xr : cr;   // a scalar result takes its upper lanes from here
    const bool zero = s.scalar ? e.zero_above(first, w) : w == 128;
    const Xmm d = e.dest(in.rd);
    e.op(s.pick(is_max ? fops::max : fops::min), d, nm ? x : cv, nm ? cv : x);
    if (!zero) e.zero_upper(d, w);
    return true;
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
struct MinMaxGuard {
    Gp reg;
    bool jrcxz;
};

// The NaN-lane test's GPR: RCX (JRCXZ leaves the host flags alone), else any
// temp after a flag save (clobber_host_flags). Before the first label.
MinMaxGuard minmax_guard(FpEmit& e) {
    const Gp rcx = e.ctx.temp_gpr_prefer(Gp::rcx);
    if (rcx != Gp::none) return {rcx, true};
    e.ctx.clobber_host_flags();
    return {e.gtmp(), false};
}

// T = op(a, b) (base kind), bit-exact. T, t0, t1, t2 are distinct and differ
// from a and b; a and b are unchanged. Only vector ops and the guard's
// MOVD/MOVMSKPS between the label and its target (no host flags written).
// `width`: the bits that matter (scalar: the element; vector: 64 or 128).
void minmax_exact_core(FpEmit& e, K kind, Shape s, unsigned width, Xmm T, Xmm a, Xmm b, Xmm t0, Xmm t1, Xmm t2,
                       const MinMaxGuard& guard) {
    const bool nm = kind == K::fmaxnm || kind == K::fminnm;
    const bool is_max = kind == K::fmax || kind == K::fmaxnm;
    const bool dbl = s.dbl();
    const SseOp mm = s.pick(is_max ? fops::max : fops::min), cmp = s.pick(fops::cmp);
    const SseOp and_ = s.pick(fops::and_), or_ = s.pick(fops::or_), andn = s.pick(fops::andn);
    const auto numeric = [&](Xmm out, Xmm scratch) {
        e.op(mm, out, a, b);
        e.op(mm, scratch, b, a);
        e.op(is_max ? and_ : or_, out, out, scratch);
    };
    numeric(T, t1);
    e.op_i(cmp, t0, a, b, pred_unord);
    if (s.scalar) {
        if (dbl) e.movq(guard.reg, t0);
        else e.movd(guard.reg, t0);
    } else if (width == 64) {
        e.movq(guard.reg, t0);
    } else {
        e.as.movmskps(guard.reg, t0);
    }
    const auto done = e.as.new_label();
    if (guard.jrcxz) {
        e.as.jrcxz_short(done);
    } else {
        e.as.test(64, guard.reg, guard.reg);   // MOVQ: lane 1 is in bits 63:32
        e.as.jcc_short(x64::Cond::e, done);
    }
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
    // A64 flushes it, which max(x, x) does too (a NaN comes back unchanged).
    if (nm) e.op(s.pick(fops::max), t1, t1, t1);
    numeric(T, t2);
    e.blend(dbl, T, T, t1, t0, t0);
    e.as.bind(done);
}

void emit_minmax_exact(FpEmit& e, const Inst& in, Shape s, unsigned w) {
    const Xmm a = e.vec(in.rn), b = e.vec(in.rm);
    const bool zero = s.scalar ? e.zero_above(in.rn, w) && e.zero_above(in.rm, w) : w == 128;
    // Every register before the first label: the guard GPR, the temps, Vd.
    const auto guard = minmax_guard(e);
    const Xmm t0 = e.tmp(), t1 = e.tmp(), t2 = e.tmp();
    const Xmm d = e.dest(in.rd);
    const bool alias = d == a || d == b;   // a and b must survive until the fixup
    const Xmm T = alias ? e.tmp() : d;
    minmax_exact_core(e, in.k, s, w, T, a, b, t0, t1, t2, guard);
    if (alias) e.mov(d, T);
    if (!zero) e.zero_upper(d, w);
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
                const auto guard = minmax_guard(e);
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
            const auto guard = minmax_guard(e);
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
            e.allow_vec_temps(5);
            const K kind = base_minmax(in.k);
            const Xmm n = e.vec(in.rn);
            const auto guard = minmax_guard(e);
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

bool emit_fused(FpEmit& e, const Inst& in) {
    const Shape s = in.sh;
    if (s.es == 16) return emit_fused_h(e, in);
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
            // f2 in memory is a guest register (element temps are registers).
            e.fma(kind, FmaForm::f213, dbl, s.scalar, d, f2.is_mem ? e.vec(static_cast<unsigned>(f2g)) : f2.reg, acc);
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
        e.ctx.clobber_host_flags();
        e.ucomi(dbl, n, m, in.signal);
        nzcv_from_ucomi(e, v, c);
        if (in.cond < 14) {
            a.mov_imm(c, static_cast<std::uint64_t>(in.nzcv) << 28);
            const Cond cc = e.ctx.condition(in.cond);
            a.cmov(x64::invert(cc), 32, v, c);
        }
        e.ctx.set_nzcv(v);
        return true;
    }
    case K::fcsel: {
        // In-memory sources are read straight into (or cmov'ed from) GPRs.
        const Opnd n = e.src(in.rn), m = e.src(in.rm);
        const Gp gn = e.gtmp(), gm = e.gtmp();
        const unsigned bits = dbl ? 64 : 32;
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
