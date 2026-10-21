// fp family: reciprocal / reciprocal-square-root estimates and steps
// (owner: fp).
#include "fx/translator/emit_fp_internal.hpp"

#include <cstring>

namespace transmute::fx::fpx {

namespace {

std::uint64_t fconst(unsigned es, double value) {
    if (es == 64) {
        std::uint64_t b;
        static_assert(sizeof(b) == sizeof(value));
        std::memcpy(&b, &value, 8);
        return b;
    }
    const float f = static_cast<float>(value);
    std::uint32_t b;
    std::memcpy(&b, &f, 4);
    return b;
}

void load_const(FpEmit& e, Shape s, Xmm d, double value) {
    if (s.scalar) e.scalar_const(s.es, d, fconst(s.es, value));
    else e.splat(s.es, d, fconst(s.es, value));
}

// FRECPS: 2 - a*b; FRSQRTS: (3 - a*b) / 2; both fused, inf*0 -> 2.0 / 1.5.
bool emit_step(FpEmit& e, const Inst& in) {
    const Shape s = in.sh;
    const bool dbl = s.dbl();
    const bool rsqrt = in.k == K::frsqrts;
    const Xmm a = e.vec(in.rn), b = e.vec(in.rm);
    const Xmm t = e.tmp(), c = e.tmp(), un = e.tmp(), nn = e.tmp();
    load_const(e, s, t, rsqrt ? 3.0 : 2.0);
    e.fma(FmaKind::fnmadd, FmaForm::f231, dbl, s.scalar, t, a, b);
    if (rsqrt) {
        load_const(e, s, c, 0.5);
        e.op(s.pick(fops::mul), t, t, c);
    }
    e.op_i(s.pick(fops::cmp), un, t, t, pred_unord);
    e.op_i(s.pick(fops::cmp), nn, a, b, pred_unord);
    e.op(s.pick(fops::andn), nn, nn, un);           // NaN from inf * 0
    load_const(e, s, c, rsqrt ? 1.5 : 2.0);
    e.blend(dbl, t, t, c, nn, un);
    e.commit(in.rd, t, s.width());
    return true;
}

// FRECPX (scalar only): sign : NOT(exp) : zeros, exp 0 -> max_exp - 1.
bool emit_frecpx(FpEmit& e, const Inst& in) {
    const unsigned es = in.sh.es;
    const unsigned ebits = es == 64 ? 11 : es == 32 ? 8 : 5;
    const unsigned fbits = es - ebits - 1;
    const std::uint64_t emax = (1ULL << ebits) - 1;
    auto& a = e.as;
    const Xmm n = e.vec(in.rn);
    const Gp g = e.gtmp(), ex = e.gtmp(), r = e.gtmp(), c = e.gtmp(), t = e.gtmp();
    e.ctx.clobber_host_flags();
    if (es == 64) e.movq(g, n);
    else if (es == 32) e.movd(g, n);
    else a.pextr(16, g, n, 0);
    a.mov(64, ex, g);
    a.shr(64, ex, static_cast<std::uint8_t>(fbits));
    a.and_(32, ex, static_cast<std::int32_t>(emax));
    a.mov(32, r, ex);
    a.xor_(32, r, static_cast<std::int32_t>(emax));
    a.mov_imm(c, emax - 1);
    a.test(32, ex, ex);
    a.cmov(Cond::e, 32, r, c);
    a.shl(64, r, static_cast<std::uint8_t>(fbits));
    a.mov(64, c, g);
    a.shr(64, c, static_cast<std::uint8_t>(es - 1));
    a.shl(64, c, static_cast<std::uint8_t>(es - 1));
    a.or_(64, r, c);
    // NaN -> quieted input.
    a.mov(64, c, g);
    a.bts(64, c, static_cast<std::uint8_t>(fbits - 1));
    a.mov(64, ex, g);
    a.shl(64, ex, static_cast<std::uint8_t>(64 - es + 1));
    a.mov_imm(t, emax << (64 - ebits));
    a.cmp(64, ex, t);
    a.cmov(Cond::a, 64, r, c);
    const Xmm d = e.dest(in.rd);
    if (es == 64) e.movq(d, r);
    else e.movd(d, r);
    return true;
}

// ---- FRECPE / FRSQRTE ------------------------------------------------------------
//
// Exact A64 estimates without tables. The 8-bit estimate depends only on the
// top fraction bits; one correctly rounded division (and square root)
// followed by round-to-nearest reproduces the A64 RecipEstimate /
// RecipSqrtEstimate integer algorithms for every input in every rounding
// mode (checked exhaustively by fp_estimate_formulas_match_arm_tables).
// Normal inputs take a straight path; if a valid lane is zero/denormal
// (FRECPE) or not a positive normal (FRSQRTE) the code branches to an inline
// slow path.

struct EstConst {
    unsigned fbits;
    std::uint64_t one, expm;
};

EstConst est_const(unsigned es) {
    if (es == 64) return {52, 0x3ff0000000000000ULL, 0x7ff0000000000000ULL};
    return {23, 0x3f800000ULL, 0x7f800000ULL};
}

SseOp pick_i(unsigned es, SseOp d32, SseOp q64) { return es == 64 ? q64 : d32; }

// out = FRECPE of lanes of x with a non-zero biased exponent (inf -> +-0;
// NaN lanes are garbage). Scratch t1, t2.
void recpe_core(FpEmit& e, unsigned es, Xmm out, Xmm x, Xmm t1, Xmm t2) {
    const bool d = es == 64;
    // m = sign | 1.<top 8 fraction bits> | 2^-9 (the 9-bit midpoint in [1, 2))
    e.splat(es, t1, d ? 0x800ff00000000000ULL : 0x807f8000ULL);
    e.op(op::andps, t1, t1, x);
    e.splat(es, t2, d ? 0x3ff0080000000000ULL : 0x3f804000ULL);
    e.op(op::orps, t1, t1, t2);
    e.splat(es, out, d ? 0x4080000000000000ULL : 0x44000000ULL);   // 512
    e.op(d ? op::divpd : op::divps, out, out, t1);
    e.un_i(d ? op::roundpd : op::roundps, out, out, 0x08);          // +-estimate, 256..511
    e.splat(es, t1, d ? 0x3f60000000000000ULL : 0x3b000000ULL);    // 2^-9
    e.op(d ? op::mulpd : op::mulps, out, out, t1);
    // Divide by 2^(e - bias): exact, denormal results included (FZ flushes them).
    e.splat(es, t1, est_const(es).expm);
    e.op(op::andps, t1, t1, x);
    e.op(d ? op::divpd : op::divps, out, out, t1);
}

// out = FRSQRTE of positive lanes of x with a normal biased exponent.
void rsqrte_core(FpEmit& e, unsigned es, Xmm out, Xmm x, Xmm t1, Xmm t2) {
    const bool d = es == 64;
    const auto c = est_const(es);
    // 1.<top 7 fraction bits> + 2^-8, doubled for even biased exponents (A64
    // scales x by an even power of two into [0.25, 1)): the inverted parity
    // bit, just above the fraction, is added into the exponent.
    e.splat(es, t1, d ? 0x001fe00000000000ULL : 0x00ff0000ULL);
    e.op(op::andps, t1, t1, x);
    e.splat(es, t2, 1ULL << c.fbits);
    e.op(op::xorps, t1, t1, t2);
    e.splat(es, t2, d ? 0x3ff0100000000000ULL : 0x3f808000ULL);
    e.op(pick_i(es, op::paddd, op::paddq), t1, t1, t2);
    e.un(d ? op::sqrtpd : op::sqrtps, t1, t1);
    e.splat(es, out, d ? 0x4080000000000000ULL : 0x44000000ULL);   // 512
    e.op(d ? op::divpd : op::divps, out, out, t1);
    e.un_i(d ? op::roundpd : op::roundps, out, out, 0x08);          // estimate, 256..511
    e.splat(es, t1, d ? 0x000ff00000000000ULL : 0x007f8000ULL);
    e.op(op::andps, out, out, t1);                                   // estimate<7:0> in place
    // exponent field (C - e) / 2, C = 380 (single) / 3068 (double)
    e.shift(d ? 0x73 : 0x72, 2, t1, x, static_cast<std::uint8_t>(c.fbits));
    e.splat(es, t2, d ? 3068 : 380);
    e.op(pick_i(es, op::psubd, op::psubq), t2, t2, t1);
    e.shift(d ? 0x73 : 0x72, 2, t2, t2, 1);
    e.shift(d ? 0x73 : 0x72, 6, t2, t2, static_cast<std::uint8_t>(c.fbits));
    e.op(op::orps, out, out, t2);
}

// m = 2^k (bits one + delta) on zero/denormal lanes of x, 1.0 elsewhere.
void denormal_scale(FpEmit& e, unsigned es, Xmm m, Xmm x, Xmm t, std::uint64_t delta) {
    const auto c = est_const(es);
    e.splat(es, m, c.expm);
    e.op(op::andps, m, m, x);
    e.zero(t);
    e.op(pick_i(es, op::pcmpeqd, op::pcmpeqq), m, m, t);
    e.splat(es, t, delta);
    e.op(op::andps, m, m, t);
    e.splat(es, t, c.one);
    e.op(pick_i(es, op::paddd, op::paddq), m, m, t);
}

// m = lanes of x that are not positive normals (in place on m = x copy).
void not_positive_normal(FpEmit& e, unsigned es, Xmm m, Xmm x, Xmm t) {
    const auto c = est_const(es);
    const bool d = es == 64;
    // (x - min_normal) >= (inf - min_normal), unsigned, via the top bits.
    e.splat(es, t, 1ULL << c.fbits);
    e.op(pick_i(es, op::psubd, op::psubq), m, x, t);
    e.shift(d ? 0x73 : 0x72, 2, m, m, static_cast<std::uint8_t>(c.fbits + 1));
    e.splat(es, t, d ? 0x3fe : 0x7e);
    e.op(pick_i(es, op::pcmpgtd, op::pcmpgtq), m, m, t);
}

// m = lanes of x with a zero biased exponent.
void zero_exponent(FpEmit& e, unsigned es, Xmm m, Xmm x, Xmm t) {
    e.splat(es, t, est_const(es).expm);
    e.op(op::andps, m, x, t);
    e.zero(t);
    e.op(pick_i(es, op::pcmpeqd, op::pcmpeqq), m, m, t);
}

bool emit_recip_estimate(FpEmit& e, const Inst& in) {
    const Shape s = in.sh;
    const unsigned es = s.es;
    const bool d = es == 64;
    const bool rsqrt = in.k == K::frsqrte;
    const auto c = est_const(es);
    auto& a = e.as;
    const Xmm x = e.vec(in.rn);
    const Xmm T0 = e.tmp(), T1 = e.tmp(), T2 = e.tmp(), T3 = e.tmp();
    e.const_gpr();     // before the internal label
    e.dest(in.rd);     // likewise (written by commit after the label)
    const Label done = a.new_label();
    e.ctx.clobber_host_flags();
    const auto core = [&](Xmm out, Xmm in_x, Xmm t1, Xmm t2) {
        if (rsqrt) rsqrte_core(e, es, out, in_x, t1, t2);
        else recpe_core(e, es, out, in_x, t1, t2);
    };
    // ---- common path ----
    core(T0, x, T1, T2);
    if (rsqrt) {
        not_positive_normal(e, es, T1, x, T2);
    } else {
        e.op_i(d ? op::cmppd : op::cmpps, T2, x, x, pred_unord);   // NaN -> NaN
        e.op(op::orps, T0, T0, T2);
        zero_exponent(e, es, T1, x, T2);
    }
    if (s.width() == 128) {
        e.un(op::ptest, T1, T1);
    } else {
        e.ones(T2);
        e.zero_upper(T2, s.width());
        e.un(op::ptest, T1, T2);
    }
    a.jcc(Cond::e, done);
    // ---- slow path ----
    // Denormals are scaled to normals (A64 normalizes them) and the result
    // scaled back; lanes that are still zero/denormal (or, for FRSQRTE, not
    // positive normals) use x64 1/x or 1/sqrt(x), whose special results
    // (+-inf with DZC, overflow per rounding mode, +-0, NaN with IOC) are the
    // A64 ones.
    const std::uint64_t pre = rsqrt ? (static_cast<std::uint64_t>(d ? 54 : 24) << c.fbits) : (2ULL << c.fbits);
    const std::uint64_t post = rsqrt ? (static_cast<std::uint64_t>(d ? 27 : 12) << c.fbits) : (2ULL << c.fbits);
    const SseOp mul = d ? op::mulpd : op::mulps;
    denormal_scale(e, es, T1, x, T2, pre);
    e.op(mul, T3, x, T1);                         // x'
    core(T0, T3, T1, T2);
    if (rsqrt) not_positive_normal(e, es, T1, T3, T2);
    else zero_exponent(e, es, T1, T3, T2);       // T1 = fallback lanes
    if (rsqrt) e.un(d ? op::sqrtpd : op::sqrtps, T3, T3);
    e.splat(es, T2, c.one);
    e.op(d ? op::divpd : op::divps, T2, T2, T3);  // 1/x' or 1/sqrt(x')
    e.blend(d, T0, T0, T2, T1, T3);
    // Scale back (before: overflow of 1/x' then rounds like 1/x would).
    denormal_scale(e, es, T1, x, T2, post);
    e.op(mul, T0, T0, T1);
    if (!rsqrt) {
        e.op_i(d ? op::cmppd : op::cmpps, T1, x, x, pred_unord);
        e.op(op::orps, T0, T0, T1);
    }
    a.bind(done);
    e.commit(in.rd, T0, s.width());
    return true;
}

} // namespace

bool emit_estimate(FpEmit& e, const Inst& in) {
    switch (in.k) {
    case K::frecps: case K::frsqrts:
        // Half-precision steps would need the double-precision fused path;
        // they are left to the interpreter (NOTES_FP.md).
        if (in.sh.es == 16) return false;
        return emit_step(e, in);
    case K::frecpx:
        return emit_frecpx(e, in);
    case K::frecpe: case K::frsqrte:
        if (in.sh.es == 16) return false;   // half-precision estimates: interpreter (NOTES_FP.md)
        return emit_recip_estimate(e, in);
    default:
        return false;
    }
}

} // namespace transmute::fx::fpx
