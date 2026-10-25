// Integer SIMD arithmetic: three same, three different, two-register misc,
// across lanes, by element, dot products and their scalar forms (owner: neon).
#include "fx/translator/emit_neon_internal.hpp"

namespace transmute::fx::neon {

namespace {

// ---- building blocks ---------------------------------------------------------------

const Const128 kEvenOdd8 = bytes_const({0, 2, 4, 6, 8, 10, 12, 14, 1, 3, 5, 7, 9, 11, 13, 15});
const Const128 kEvenOdd16 = bytes_const({0, 1, 4, 5, 8, 9, 12, 13, 2, 3, 6, 7, 10, 11, 14, 15});

constexpr std::uint64_t smax(unsigned size) { return (1ULL << ((8u << size) - 1)) - 1; }
constexpr std::uint64_t sign_bit(unsigned size) { return 1ULL << ((8u << size) - 1); }

void halving_add(V& v, unsigned size, bool u, bool round, Xmm d, Xmm n, Xmm m) {
    if (round) {
        if (size < 2) {
            const SseOp avg = size == 0 ? op::pavgb : op::pavgw;
            if (u) { v.op(avg, d, n, m, true); return; }
            const Const128 bias = splat(size, sign_bit(size));
            const Xmm t = v.temp();
            v.op(op::pxor, t, m, bias);
            v.op(op::pxor, d, n, bias);
            v.op(avg, d, d, t, true);
            v.op(op::pxor, d, d, bias);
            return;
        }
        const Xmm t = v.temp();   // (n | m) - ((n ^ m) >> 1)
        v.pxor(t, n, m);
        if (u) v.psrld(t, t, 1); else v.psrad(t, t, 1);
        v.por(d, n, m);
        v.op(op::psubd, d, d, t);
        return;
    }
    if (size == 0 && !u) {   // UHADD(n^0x80, m^0x80) ^ 0x80
        const Xmm p = v.temp();
        const Xmm t = v.temp();
        v.pxor(p, n, m);
        v.op(op::pand, p, p, splat8(1));
        v.op(op::pxor, t, m, splat8(0x80));
        v.op(op::pxor, d, n, splat8(0x80));
        v.op(op::pavgb, d, d, t, true);
        v.op(op::psubb, d, d, p);
        v.op(op::pxor, d, d, splat8(0x80));
        return;
    }
    if (u && size < 2) {   // pavg - ((n ^ m) & 1)
        const Xmm p = v.temp();
        v.pxor(p, n, m);
        v.op(op::pand, p, p, splat(size, 1));
        v.op(size == 0 ? op::pavgb : op::pavgw, d, n, m, true);
        v.op(kSub[size], d, d, p);
        return;
    }
    const Xmm t = v.temp();   // (n & m) + ((n ^ m) >> 1)
    v.pxor(t, n, m);
    if (u) v.srl(size, t, t, 1); else v.sra(size, t, t, 1);
    v.pand(d, n, m);
    v.op(kAdd[size], d, d, t, true);
}

void halving_sub(V& v, unsigned size, bool u, Xmm d, Xmm n, Xmm m) {
    if (size < 2) {
        // UHSUB(x, y) = pavg(x, ~y) ^ bias; SHSUB = UHSUB(x ^ bias, y ^ bias).
        const SseOp avg = size == 0 ? op::pavgb : op::pavgw;
        const Const128 bias = splat(size, sign_bit(size));
        const Xmm t = v.temp();
        if (u) {
            v.not_(t, m);
            v.op(avg, d, n, t, true);
        } else {
            v.op(op::pxor, t, m, splat(size, smax(size)));
            v.op(op::pxor, d, n, bias);
            v.op(avg, d, d, t, true);
        }
        v.op(op::pxor, d, d, bias);
        return;
    }
    const Xmm t = v.temp();
    const Xmm s = v.temp();
    if (u) {   // ((n ^ m) >> 1) - (~n & m)
        v.pxor(t, n, m);
        v.psrld(t, t, 1);
        v.pandn(s, n, m);
        v.op(op::psubd, d, t, s);
        return;
    }
    // (n >> 1) - (m >> 1) - (~n & m & 1)
    v.psrad(t, n, 1);
    v.psrad(s, m, 1);
    v.op(op::psubd, t, t, s);
    v.pandn(s, n, m);
    v.op(op::pand, s, s, splat32(1));
    v.op(op::psubd, d, t, s);
}

} // namespace

void sat_add_sub(V& v, unsigned size, bool u, bool sub, Xmm d, Xmm n, Xmm m) {
    if (size < 2) {
        static constexpr SseOp table[2][2][2] = {
            {{op::paddsb, op::paddsw}, {op::paddusb, op::paddusw}},
            {{op::psubsb, op::psubsw}, {op::psubusb, op::psubusw}},
        };
        v.op(table[sub][u][size], d, n, m, !sub);
        return;
    }
    if (u) {
        if (!sub) {
            if (size == 2) {   // n + min(m, ~n)
                const Xmm t = v.temp();
                v.not_(t, n);
                v.op(op::pminud, t, t, m, true);
                v.op(op::paddd, d, n, t, true);
                return;
            }
            const Xmm r = v.temp();
            const Xmm c = v.temp();
            v.op(op::paddq, r, n, m, true);
            v.cmp_gt_u(3, c, n, r);
            v.por(d, r, c);
            return;
        }
        if (size == 2) {   // n - min(n, m)
            const Xmm t = v.temp();
            v.op(op::pminud, t, n, m, true);
            v.op(op::psubd, d, n, t);
            return;
        }
        const Xmm r = v.temp();
        const Xmm b = v.temp();
        v.op(op::psubq, r, n, m);
        v.cmp_gt_u(3, b, m, n);
        v.pandn(d, b, r);
        return;
    }
    const Xmm r = v.temp();
    const Xmm o = v.temp();
    const Xmm s = v.temp();
    if (!sub) {
        v.op(kAdd[size], r, n, m, true);
        v.pxor(o, r, n);
        v.pxor(s, r, m);
    } else {
        v.op(kSub[size], r, n, m);
        v.pxor(o, n, m);
        v.pxor(s, n, r);
    }
    v.pand(o, o, s);
    v.sign_mask(size, s, n);
    v.op(op::pxor, s, s, splat(size, smax(size)));
    if (!v.avx) v.sign_mask(size, o, o);
    v.blendv_lane(size, d, r, s, o);
}

namespace {

void mul(V& v, unsigned size, Xmm d, Xmm n, Xmm m) {
    if (size == 1) { v.op(op::pmullw, d, n, m, true); return; }
    if (size == 2) { v.op(op::pmulld, d, n, m, true); return; }
    // bytes: even lanes from the low byte of a word product, odd lanes from
    // (n >> 8) * (m & 0xff00).
    const Xmm lo = v.temp();
    const Xmm hi = v.temp();
    const Xmm t = v.temp();
    v.op(op::pmullw, lo, n, m, true);
    v.op(op::pand, lo, lo, splat16(0x00ff));
    v.psrlw(hi, n, 8);
    v.op(op::pand, t, m, splat16(0xff00));
    v.op(op::pmullw, hi, hi, t, true);
    v.por(d, lo, hi);
}

// Polynomial (carry-less) 8-bit multiply of every byte.
void pmul8(V& v, Xmm d, Xmm n, Xmm m) {
    const Xmm acc = v.temp();
    const Xmm x = v.temp();
    const Xmm mask = v.temp();
    v.zero(acc);
    v.mov(x, n);
    for (unsigned i = 0; i < 8; ++i) {
        const auto b = static_cast<std::uint8_t>(1u << i);
        v.op(op::pand, mask, m, splat8(b));
        v.op(op::pcmpeqb, mask, mask, splat8(b));
        v.pand(mask, mask, x);
        if (i == 7) v.pxor(d, acc, mask);
        else v.pxor(acc, acc, mask);
        if (i != 7) v.op(op::paddb, x, x, x, true);
    }
}

// 8x8 -> 16 polynomial multiply of the low (high = false) or high bytes.
void pmull8(V& v, bool high, Xmm d, Xmm n, Xmm m) {
    const Xmm x = v.temp();
    const Xmm y = v.temp();
    const Xmm acc = v.temp();
    const Xmm mask = v.temp();
    if (high) {
        v.pshufd(x, n, 0xee);
        v.un(op::pmovzxbw, x, x);
        v.pshufd(y, m, 0xee);
        v.un(op::pmovzxbw, y, y);
    } else {
        v.un(op::pmovzxbw, x, n);
        v.un(op::pmovzxbw, y, m);
    }
    if (neon_on(neon_old_pmull8)) {
        // x * (y & 2^i) is x << i or 0: a single-bit multiplier never carries, so
        // the integer products are the carry-less partial products.
        for (unsigned i = 0; i < 8; ++i) {
            v.op(op::pand, mask, y, splat16(static_cast<std::uint16_t>(1u << i)));
            if (i == 0) {
                v.op(op::pmullw, acc, x, mask, true);
                continue;
            }
            v.op(op::pmullw, mask, x, mask, true);
            if (i == 7) v.pxor(d, acc, mask);
            else v.pxor(acc, acc, mask);
        }
        return;
    }
    v.zero(acc);
    for (unsigned i = 0; i < 8; ++i) {
        const auto b = static_cast<std::uint16_t>(1u << i);
        v.op(op::pand, mask, y, splat16(b));
        v.op(op::pcmpeqw, mask, mask, splat16(b));
        v.pand(mask, mask, x);
        if (i == 7) v.pxor(d, acc, mask);
        else v.pxor(acc, acc, mask);
        if (i != 7) v.op(op::paddw, x, x, x, true);
    }
}

// abs difference |n - m| (fits the unsigned element).
void abd(V& v, unsigned size, bool u, Xmm d, Xmm n, Xmm m) {
    const Xmm t = v.temp();
    if (u) {
        v.op(kMinU[size], t, n, m, true);
        v.op(kMaxU[size], d, n, m, true);
    } else {
        v.op(kMinS[size], t, n, m, true);
        v.op(kMaxS[size], d, n, m, true);
    }
    v.op(kSub[size], d, d, t);
}

// Signed saturating doubling multiply returning the high half (H/S).
void sqdmulh(V& v, unsigned size, bool round, Xmm d, Xmm n, Xmm m) {
    if (size == 1) {
        if (round) {
            const Xmm f = v.temp();
            v.op(op::pmulhrsw, d, n, m, true);
            v.op(op::pcmpeqw, f, d, splat16(0x8000));
            v.pxor(d, d, f);
            return;
        }
        const Xmm hi = v.temp();
        const Xmm lo = v.temp();
        v.op(op::pmulhw, hi, n, m, true);
        v.op(op::pmullw, lo, n, m, true);
        v.psllw(hi, hi, 1);
        v.psrlw(lo, lo, 15);
        v.por(d, hi, lo);
        v.op(op::pcmpeqw, lo, d, splat16(0x8000));
        v.pxor(d, d, lo);
        return;
    }
    const Xmm no = v.temp();
    const Xmm mo = v.temp();
    const Xmm pe = v.temp();
    v.pshufd(no, n, 0xf5);
    v.pshufd(mo, m, 0xf5);
    v.op(op::pmuldq, pe, n, m, true);
    v.op(op::pmuldq, no, no, mo, true);
    v.op(op::paddq, pe, pe, pe, true);
    v.op(op::paddq, no, no, no, true);
    if (round) {
        v.op(op::paddq, pe, pe, splat64(0x80000000ULL));
        v.op(op::paddq, no, no, splat64(0x80000000ULL));
    }
    v.psrlq(pe, pe, 32);
    v.op_imm(op::pblendw, d, pe, no, 0xcc);
    v.op(op::pcmpeqd, mo, d, splat32(0x80000000u));
    v.pxor(d, d, mo);
}

// E/O deinterleave of concat(n, m) (q) or concat(n.lo, m.lo) (results in the
// low 64 bits when !q). e and o must be fresh temps.
void deinterleave(V& v, unsigned size, bool q, Xmm e, Xmm o, Xmm n, Xmm m) {
    if (q) {
        if (size == 3) {
            v.op(op::punpcklqdq, e, n, m);
            v.op(op::punpckhqdq, o, n, m);
            return;
        }
        if (size == 2) {
            v.op_imm(op::shufps, e, n, m, 0x88);
            v.op_imm(op::shufps, o, n, m, 0xdd);
            return;
        }
        const Const128 c = size == 0 ? kEvenOdd8 : kEvenOdd16;
        const Xmm b = v.temp();
        v.pshufb(o, n, c);
        v.pshufb(b, m, c);
        v.op(op::punpcklqdq, e, o, b);
        v.op(op::punpckhqdq, o, o, b);
        return;
    }
    const Xmm t = v.temp();
    v.op(op::punpcklqdq, t, n, m);
    if (size == 2) {
        v.pshufd(e, t, 0x08);
        v.pshufd(o, t, 0x0d);
        return;
    }
    v.pshufb(e, t, size == 0 ? kEvenOdd8 : kEvenOdd16);
    v.pshufd(o, e, 0xee);
}

enum class Pairwise { add, smax, smin, umax, umin };

void pairwise(V& v, unsigned size, bool q, Pairwise kind, Xmm d, Xmm n, Xmm m) {
    if (kind == Pairwise::add && (size == 1 || size == 2)) {
        const SseOp hadd = size == 1 ? op::phaddw : op::phaddd;
        if (q) {
            v.op(hadd, d, n, m);
        } else {
            const Xmm t = v.temp();
            v.op(op::punpcklqdq, t, n, m);
            v.op(hadd, d, t, t);
            v.movq(d, d);
        }
        return;
    }
    const Xmm e = v.temp();
    const Xmm o = v.temp();
    deinterleave(v, size, q, e, o, n, m);
    switch (kind) {
    case Pairwise::add: v.op(kAdd[size], d, e, o, true); break;
    case Pairwise::smax: v.op(kMaxS[size], d, e, o, true); break;
    case Pairwise::smin: v.op(kMinS[size], d, e, o, true); break;
    case Pairwise::umax: v.op(kMaxU[size], d, e, o, true); break;
    case Pairwise::umin: v.op(kMinU[size], d, e, o, true); break;
    }
    v.finish(d, q);
}

} // namespace

// Sign/zero extension of the low (high = false) or high half of x to 2x width.
void widen(V& v, unsigned size, bool is_signed, bool high, Xmm d, Xmm x) {
    const SseOp ext = is_signed ? kSext[size] : kZext[size];
    if (high && !is_signed && neon_on(neon_old_widen_hi)) {
        v.op(kUnpackHi[size], d, x, splat32(0));   // interleaved with zeros: zero-extended
        return;
    }
    if (high) {
        v.pshufd(d, x, 0xee);
        v.un(ext, d, d);
    } else {
        v.un(ext, d, x);
    }
}

namespace {

// Widening multiply of the low/high halves: d = ext(n) * ext(m) in 2x elements.
void mull(V& v, unsigned size, bool is_signed, bool high, Xmm d, Xmm n, Xmm m) {
    switch (size) {
    case 0: {
        const Xmm a = v.temp();
        const Xmm b = v.temp();
        widen(v, 0, is_signed, high, a, n);
        widen(v, 0, is_signed, high, b, m);
        v.op(op::pmullw, d, a, b, true);
        return;
    }
    case 1: {
        const Xmm lo = v.temp();
        const Xmm hi = v.temp();
        v.op(op::pmullw, lo, n, m, true);
        v.op(is_signed ? op::pmulhw : op::pmulhuw, hi, n, m, true);
        v.op(high ? op::punpckhwd : op::punpcklwd, d, lo, hi);
        return;
    }
    default: {
        const Xmm a = v.temp();
        const Xmm b = v.temp();
        v.pshufd(a, n, high ? 0xfa : 0x50);
        v.pshufd(b, m, high ? 0xfa : 0x50);
        v.op(is_signed ? op::pmuldq : op::pmuludq, d, a, b, true);
        return;
    }
    }
}

// SMULL by element on 16-bit lanes: m holds the element in every word. The
// low/high half of n zero-extended to dwords leaves PMADDWD's odd products 0,
// so each dword is the exact product n[i] * e.
void smull16_splat(V& v, bool high, Xmm d, Xmm n, Xmm m) {
    const Xmm a = v.temp();
    if (high) v.op(op::punpckhwd, a, n, splat32(0));
    else v.un(kZext[1], a, n);
    v.op(op::pmaddwd, d, a, m, true);
}

// Signed saturating doubling widening multiply (H->S, S->D). m_splat: m
// holds one element in every lane (by element), so it needs no unpack.
void sqdmull(V& v, unsigned size, bool high, Xmm d, Xmm n, Xmm m, bool m_splat = false) {
    const Xmm a = v.temp();
    const Xmm b = v.temp();
    if (size == 1) {
        const SseOp unpack = high ? op::punpckhwd : op::punpcklwd;
        v.op(unpack, a, n, n);
        if (!m_splat) v.op(unpack, b, m, m);
        v.op(op::pmaddwd, d, a, m_splat ? m : b, true);
        v.op(op::pcmpeqd, a, d, splat32(0x80000000u));
        v.pxor(d, d, a);
        return;
    }
    v.pshufd(a, n, high ? 0xfa : 0x50);
    v.pshufd(b, m, high ? 0xfa : 0x50);
    v.op(op::pmuldq, d, a, b, true);
    v.op(op::paddq, d, d, d, true);
    v.op(op::pcmpeqq, a, d, splat64(0x8000000000000000ULL));
    v.pxor(d, d, a);
}

const Const128 kTruncate[3] = {
    bytes_const({0, 2, 4, 6, 8, 10, 12, 14, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80}),
    bytes_const({0, 1, 4, 5, 8, 9, 12, 13, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80}),
    bytes_const({0, 1, 2, 3, 8, 9, 10, 11, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80}),
};

} // namespace

namespace {

void clamp_s64(V& v, Xmm d, Xmm x, std::int64_t lo, std::int64_t hi) {
    // d = min(max(x, lo), hi) on signed 64-bit lanes.
    const Xmm c = v.temp();
    const Xmm m = v.temp();
    v.load_const(c, splat64(static_cast<std::uint64_t>(hi)));
    v.op(op::pcmpgtq, m, x, c);
    v.blendv(d, x, c, m);
    v.load_const(c, splat64(static_cast<std::uint64_t>(lo)));
    v.op(op::pcmpgtq, m, c, d);
    v.blendv(d, d, c, m);
}

} // namespace

void narrow(V& v, unsigned size, Narrow kind, Xmm d, Xmm x) {
    switch (kind) {
    case Narrow::truncate:
        v.pshufb(d, x, kTruncate[size]);
        return;
    case Narrow::sat_signed:
        if (size < 2) {
            v.op(size == 0 ? op::packsswb : op::packssdw, d, x, x, true);
            v.movq(d, d);
        } else {
            const Xmm t = v.temp();
            clamp_s64(v, t, x, INT32_MIN, INT32_MAX);
            v.pshufb(d, t, kTruncate[2]);
        }
        return;
    case Narrow::sat_signed_to_unsigned:
        if (size < 2) {
            v.op(size == 0 ? op::packuswb : op::packusdw, d, x, x, true);
            v.movq(d, d);
        } else {
            const Xmm t = v.temp();
            clamp_s64(v, t, x, 0, 0xffffffffLL);
            v.pshufb(d, t, kTruncate[2]);
        }
        return;
    case Narrow::sat_unsigned:
        if (size < 2) {
            const Xmm t = v.temp();
            if (size == 0) v.op(op::pminuw, t, x, splat16(0x00ff));
            else v.op(op::pminud, t, x, splat32(0x0000ffffu));
            v.pshufb(d, t, kTruncate[size]);
        } else {
            const Xmm t = v.temp();
            const Xmm z = v.temp();
            v.psrlq(t, x, 32);
            v.zero(z);
            v.op(op::pcmpeqq, t, t, z, true);       // 1s where the value fits
            v.op(op::pandn, t, t, v.k(splat8(0xff)));
            v.por(t, t, x);
            v.pshufb(d, t, kTruncate[2]);
        }
        return;
    }
}

namespace {

// d = {dold.lo, r.lo} for the "2" (upper half) narrowing forms.
void insert_upper(V& v, Xmm d, Xmm dold, Xmm r) { v.op(op::punpcklqdq, d, dold, r); }

} // namespace

Src dup_source(V& v, unsigned size, const Src& x) {
    if (!x.is_mem || (v.avx && (size >= 2 || v.avx2))) return x;
    return Src{in_reg(v.ctx, x), {}, false, x.guest};
}

void dup_element(V& v, unsigned size, Xmm d, const Src& x, unsigned index) {
    if (!x.is_mem) {
        dup_element(v, size, d, x.reg, index);
        return;
    }
    if (!v.avx || (size < 2 && !v.avx2)) throw std::logic_error("neon: dup_element source not prepared");
    static constexpr VexOp ops[4] = {vop::vpbroadcastb, vop::vpbroadcastw, vop::vbroadcastss,
                                     VexOp{3, 1, 0x12, false}};   // vmovddup
    v.vex2(ops[size], d, x.element(size, index));
}

// Broadcasts element `index` of size `size` from x into every lane of d.
void dup_element(V& v, unsigned size, Xmm d, Xmm x, unsigned index) {
    if (index == 0 && v.avx2) {
        static constexpr VexOp ops[4] = {vop::vpbroadcastb, vop::vpbroadcastw, vop::vpbroadcastd,
                                         vop::vpbroadcastq};
        v.vex2(ops[size], d, x);
        return;
    }
    switch (size) {
    case 0: v.pshufb(d, x, splat8(static_cast<std::uint8_t>(index))); break;
    case 1: {
        const auto lo = static_cast<std::uint8_t>(index * 2);
        v.pshufb(d, x, splat16(static_cast<std::uint16_t>(lo | ((lo + 1) << 8))));
        break;
    }
    case 2: v.pshufd(d, x, static_cast<std::uint8_t>(index * 0x55)); break;
    default: v.pshufd(d, x, index ? 0xee : 0x44); break;
    }
}

namespace {

// Count leading zero bits per byte: d = clz8(x) (x and d may alias).
void clz8(V& v, Xmm d, Xmm x) {
    // LUT_hi[h] = clz4(h) (8 for h == 0), LUT_lo[l] = 4 + clz4(l) (8 for 0).
    static const Const128 lut_hi = bytes_const({8, 3, 2, 2, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0});
    static const Const128 lut_lo = bytes_const({8, 7, 6, 6, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4});
    const Xmm lo = v.temp();
    const Xmm hi = v.temp();
    const Xmm t = v.temp();
    v.op(op::pand, lo, x, splat8(0x0f));
    v.psrlw(hi, x, 4);
    v.op(op::pand, hi, hi, splat8(0x0f));
    v.load_const(t, lut_hi);
    v.pshufb(hi, t, hi);
    v.load_const(t, lut_lo);
    v.pshufb(lo, t, lo);
    v.op(op::pminub, d, hi, lo, true);
}

void clz(V& v, unsigned size, Xmm d, Xmm x) {
    if (size == 0) { clz8(v, d, x); return; }
    const Xmm c = v.temp();
    const Xmm hi = v.temp();
    const Xmm lo = v.temp();
    clz8(v, c, x);
    // H: r = hi + (hi == 8 ? lo : 0)
    v.psrlw(hi, c, 8);
    v.op(op::pand, lo, c, splat16(0x00ff));
    v.op(op::pcmpeqw, c, hi, splat16(8));
    v.pand(lo, lo, c);
    if (size == 1) { v.op(op::paddw, d, hi, lo, true); return; }
    v.op(op::paddw, c, hi, lo, true);
    // S: r = hi16 + (hi16 == 16 ? lo16 : 0)
    v.psrld(hi, c, 16);
    v.op(op::pand, lo, c, splat32(0xffff));
    v.op(op::pcmpeqd, c, hi, splat32(16));
    v.pand(lo, lo, c);
    v.op(op::paddd, d, hi, lo, true);
}

void cnt8(V& v, Xmm d, Xmm x) {
    static const Const128 lut = bytes_const({0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4});
    const Xmm lo = v.temp();
    const Xmm hi = v.temp();
    const Xmm t = v.temp();
    v.op(op::pand, lo, x, splat8(0x0f));
    v.psrlw(hi, x, 4);
    v.op(op::pand, hi, hi, splat8(0x0f));
    v.load_const(t, lut);
    v.pshufb(lo, t, lo);
    v.pshufb(hi, t, hi);
    v.op(op::paddb, d, lo, hi, true);
}

void rbit8(V& v, Xmm d, Xmm x) {
    // rev(lo) << 4 | rev(hi)
    static const Const128 lut_lo = bytes_const({0x00, 0x80, 0x40, 0xc0, 0x20, 0xa0, 0x60, 0xe0,
                                                0x10, 0x90, 0x50, 0xd0, 0x30, 0xb0, 0x70, 0xf0});
    static const Const128 lut_hi = bytes_const({0x0, 0x8, 0x4, 0xc, 0x2, 0xa, 0x6, 0xe,
                                                0x1, 0x9, 0x5, 0xd, 0x3, 0xb, 0x7, 0xf});
    const Xmm lo = v.temp();
    const Xmm hi = v.temp();
    const Xmm t = v.temp();
    v.op(op::pand, lo, x, splat8(0x0f));
    v.psrlw(hi, x, 4);
    v.op(op::pand, hi, hi, splat8(0x0f));
    v.load_const(t, lut_lo);
    v.pshufb(lo, t, lo);
    v.load_const(t, lut_hi);
    v.pshufb(hi, t, hi);
    v.por(d, lo, hi);
}

// Saturating absolute value / negation.
void sqabs(V& v, unsigned size, Xmm d, Xmm x) {
    if (size < 3) {
        v.un(kAbs[size], d, x);
        v.op(kMinU[size], d, d, splat(size, smax(size)));
        return;
    }
    const Xmm s = v.temp();
    const Xmm t = v.temp();
    v.sign_mask(3, s, x);
    v.pxor(t, x, s);
    v.op(op::psubq, t, t, s);          // |x| (MIN stays MIN)
    v.psrlq(s, t, 63);                 // 1 only for MIN
    v.op(op::psubq, d, t, s);
}

void sqneg(V& v, unsigned size, Xmm d, Xmm x) {
    const Xmm z = v.temp();
    v.zero(z);
    if (size < 2) {
        v.op(size == 0 ? op::psubsb : op::psubsw, d, z, x);
        return;
    }
    const Xmm f = v.temp();
    v.op(kCmpEq[size], f, x, splat(size, sign_bit(size)));
    v.op(kSub[size], d, z, x);
    v.pxor(d, d, f);
}

void abs_(V& v, unsigned size, Xmm d, Xmm x) {
    if (size < 3) { v.un(kAbs[size], d, x); return; }
    const Xmm s = v.temp();
    const Xmm t = v.temp();
    v.sign_mask(3, s, x);
    v.pxor(t, x, s);
    v.op(op::psubq, d, t, s);
}

void neg(V& v, unsigned size, Xmm d, Xmm x) {
    const Xmm z = v.temp();
    v.zero(z);
    v.op(kSub[size], d, z, x);
}

// Compare against zero: kind 0 GT, 1 GE, 2 EQ, 3 LE, 4 LT.
void cmp_zero(V& v, unsigned size, unsigned kind, Xmm d, Xmm x) {
    if (kind == 4 || kind == 1) {   // LT / GE
        if (size == 0) {
            const Xmm z = v.temp();
            v.zero(z);
            v.op(op::pcmpgtb, d, z, x);
        } else {
            v.sign_mask(size, d, x);
        }
        if (kind == 1) v.not_(d, d);
        return;
    }
    const Xmm z = v.temp();
    v.zero(z);
    if (kind == 2) {
        v.op(kCmpEq[size], d, x, z, true);
        return;
    }
    v.op(kCmpGt[size], d, x, z);
    if (kind == 3) v.not_(d, d);
}

// Unsigned saturating add of an unsigned value (any size). d = sat(x + y).
void uqadd(V& v, unsigned size, Xmm d, Xmm x, Xmm y) { sat_add_sub(v, size, true, false, d, x, y); }

void suqadd(V& v, unsigned size, Xmm d, Xmm acc, Xmm x) {
    // sat_s(acc + x_u) = uqadd(acc ^ bias, x) ^ bias
    const Const128 bias = splat(size, sign_bit(size));
    const Xmm t = v.temp();
    v.op(op::pxor, t, acc, bias);
    uqadd(v, size, t, t, x);
    v.op(op::pxor, d, t, bias);
}

void usqadd(V& v, unsigned size, Xmm d, Xmm acc, Xmm x) {
    // sat_u(acc + x_s) = uqsub(uqadd(acc, max(x, 0)), magnitude(min(x, 0)))
    const Xmm s = v.temp();
    const Xmm pos = v.temp();
    const Xmm neg = v.temp();
    v.sign_mask(size, s, x);
    v.pandn(pos, s, x);
    v.zero(neg);
    v.op(kSub[size], neg, neg, x);
    v.pand(neg, neg, s);
    if (size < 2) {
        v.op(size == 0 ? op::paddusb : op::paddusw, pos, acc, pos, true);
        v.op(size == 0 ? op::psubusb : op::psubusw, d, pos, neg);
        return;
    }
    if (size == 2) {
        v.not_(s, acc);                          // uqadd: acc + min(pos, ~acc)
        v.op(op::pminud, s, s, pos, true);
        v.op(op::paddd, pos, acc, s, true);
        v.op(op::pminud, s, pos, neg, true);     // uqsub: t - min(t, neg)
        v.op(op::psubd, d, pos, s);
        return;
    }
    v.op(op::paddq, s, acc, pos, true);          // s = acc + pos
    v.cmp_gt_u(3, pos, acc, s);                  // carry
    v.por(s, s, pos);
    v.cmp_gt_u(3, pos, neg, s);                  // borrow
    v.op(op::psubq, s, s, neg);
    v.pandn(d, pos, s);
}

// Pairwise add long: d = sum of adjacent pairs widened (size = source size).
void addlp(V& v, unsigned size, bool u, Xmm d, Xmm x) {
    switch (size) {
    case 0:
        if (u) {
            v.op(op::pmaddubsw, d, x, splat8(1));
        } else {
            const Xmm one = v.temp();
            v.load_const(one, splat8(1));
            v.op(op::pmaddubsw, d, one, x);
        }
        return;
    case 1:
        if (!u) {
            v.op(op::pmaddwd, d, x, splat16(1));
        } else {
            const Xmm t = v.temp();
            v.psrld(t, x, 16);
            v.op(op::pand, d, x, splat32(0xffff));
            v.op(op::paddd, d, d, t, true);
        }
        return;
    default:
        if (!u) {
            const Xmm e = v.temp();
            const Xmm o = v.temp();
            v.pshufd(e, x, 0x08);
            v.un(op::pmovsxdq, e, e);
            v.pshufd(o, x, 0x0d);
            v.un(op::pmovsxdq, o, o);
            v.op(op::paddq, d, e, o, true);
        } else {
            const Xmm t = v.temp();
            v.psrlq(t, x, 32);
            v.op(op::pand, d, x, splat64(0xffffffffULL));
            v.op(op::paddq, d, d, t, true);
        }
        return;
    }
}

// Horizontal sum of the 32-bit lanes of x into lane 0 (other lanes garbage).
void hsum32(V& v, Xmm t, Xmm x) {
    const Xmm u = v.temp();
    v.pshufd(u, x, 0xee);
    v.op(op::paddd, t, x, u, true);
    v.pshufd(u, t, 0x55);
    v.op(op::paddd, t, t, u, true);
}

// Dot product of byte quadruples into 32-bit lanes: d = acc + dot(n, m).
// Dot product of byte quadruples into 32-bit lanes: d = acc + dot(n, m), with
// each operand signed or unsigned (SDOT, UDOT, USDOT, SUDOT).
void dot(V& v, bool n_unsigned, bool m_unsigned, bool q, Xmm d, Xmm acc, Xmm n, Xmm m) {
    const SseOp ext_n = n_unsigned ? op::pmovzxbw : op::pmovsxbw;
    const SseOp ext_m = m_unsigned ? op::pmovzxbw : op::pmovsxbw;
    const Xmm a = v.temp();
    const Xmm b = v.temp();
    const Xmm p = v.temp();
    v.un(ext_n, a, n);
    v.un(ext_m, b, m);
    v.op(op::pmaddwd, p, a, b, true);
    if (q) {
        v.pshufd(a, n, 0xee);
        v.un(ext_n, a, a);
        v.pshufd(b, m, 0xee);
        v.un(ext_m, b, b);
        v.op(op::pmaddwd, a, a, b, true);
        v.op(op::phaddd, p, p, a);
    } else {
        v.op(op::phaddd, p, p, p);
    }
    v.op(op::paddd, d, acc, p, true);
    v.finish(d, q);
}

// SQRDMLAH / SQRDMLSH (H, S): d = sat(acc + ((+-2*n*m + 2^(e-1)) >> e)),
// computed as acc + ((+-n*m + 2^(e-2)) >> (e-1)) in 2x-wide lanes.
void sqrdmlah(V& v, unsigned size, bool subtract, Xmm d, Xmm acc, Xmm n, Xmm m) {
    if (size == 1) {
        const Xmm lo = v.temp(), hi = v.temp(), p0 = v.temp(), p1 = v.temp();
        v.op(op::pmullw, lo, n, m, true);
        v.op(op::pmulhw, hi, n, m, true);
        v.op(op::punpcklwd, p0, lo, hi);
        v.op(op::punpckhwd, p1, lo, hi);
        if (subtract) {
            v.zero(lo);
            v.op(op::psubd, p0, lo, p0);
            v.op(op::psubd, p1, lo, p1);
        }
        v.op(op::paddd, p0, p0, splat32(0x4000));
        v.op(op::paddd, p1, p1, splat32(0x4000));
        v.psrad(p0, p0, 15);
        v.psrad(p1, p1, 15);
        v.un(op::pmovsxwd, lo, acc);
        v.pshufd(hi, acc, 0xee);
        v.un(op::pmovsxwd, hi, hi);
        v.op(op::paddd, p0, p0, lo, true);
        v.op(op::paddd, p1, p1, hi, true);
        v.op(op::packssdw, d, p0, p1);
        return;
    }
    const Xmm pe = v.temp(), po = v.temp(), t = v.temp(), u = v.temp();
    v.pshufd(t, n, 0xf5);
    v.pshufd(u, m, 0xf5);
    v.op(op::pmuldq, po, t, u, true);   // n1*m1, n3*m3
    v.op(op::pmuldq, pe, n, m, true);   // n0*m0, n2*m2
    if (subtract) {
        v.zero(t);
        v.op(op::psubq, pe, t, pe);
        v.op(op::psubq, po, t, po);
    }
    v.op(op::paddq, pe, pe, splat64(1ULL << 30));
    v.op(op::paddq, po, po, splat64(1ULL << 30));
    v.sra(3, pe, pe, 31);
    v.sra(3, po, po, 31);
    v.pshufd(t, acc, 0x08);
    v.un(op::pmovsxdq, t, t);
    v.pshufd(u, acc, 0x0d);
    v.un(op::pmovsxdq, u, u);
    v.op(op::paddq, pe, pe, t, true);
    v.op(op::paddq, po, po, u, true);
    clamp_s64(v, pe, pe, INT32_MIN, INT32_MAX);
    clamp_s64(v, po, po, INT32_MIN, INT32_MAX);
    v.pshufd(pe, pe, 0x08);
    v.pshufd(po, po, 0x08);
    v.op(op::punpckldq, d, pe, po);
}

// SMMLA / UMMLA / USMMLA: d.4S = acc + (2x8 n) * (2x8 m)^T.
void mmla(V& v, bool n_unsigned, bool m_unsigned, Xmm d, Xmm acc, Xmm n, Xmm m) {
    const SseOp en = n_unsigned ? op::pmovzxbw : op::pmovsxbw;
    const SseOp em = m_unsigned ? op::pmovzxbw : op::pmovsxbw;
    const Xmm n0 = v.temp(), n1 = v.temp(), m0 = v.temp(), m1 = v.temp(), p = v.temp(), q = v.temp();
    v.un(en, n0, n);
    v.pshufd(n1, n, 0xee);
    v.un(en, n1, n1);
    v.un(em, m0, m);
    v.pshufd(m1, m, 0xee);
    v.un(em, m1, m1);
    v.op(op::pmaddwd, p, n0, m0, true);
    v.op(op::pmaddwd, q, n0, m1, true);
    v.op(op::phaddd, p, p, q);
    v.op(op::pmaddwd, q, n1, m0, true);
    v.op(op::pmaddwd, n0, n1, m1, true);
    v.op(op::phaddd, q, q, n0);
    v.op(op::phaddd, p, p, q);
    v.op(op::paddd, d, acc, p, true);
}

// URECPE (rsqrt = false) / URSQRTE (rsqrt = true) on 32-bit lanes, exact
// integer estimates (binary search; no FP instructions, so MXCSR flags are
// untouched).
void unsigned_estimate(V& v, bool rsqrt, Xmm d, Xmm x) {
    const Xmm a = v.temp(), b = v.temp(), c = v.temp(), t = v.temp();
    v.psrld(a, x, 23);                          // 9-bit input
    if (!rsqrt) {
        v.op(op::paddd, a, a, a, true);
        v.op(op::por, a, a, splat32(1));         // a' = 2a + 1
    } else {
        // a < 256: 2a + 1; else (a & ~1) * 2 + 2
        v.op(op::pcmpgtd, t, a, splat32(255));   // a >= 256
        v.op(op::pandn, c, t, a);                // a < 256 ? a : 0
        v.op(op::paddd, c, c, c, true);
        v.op(op::por, c, c, splat32(1));
        v.op(op::pand, b, a, splat32(0x1fe));
        v.op(op::paddd, b, b, b, true);
        v.op(op::paddd, b, b, splat32(2));
        v.pand(b, b, t);
        v.pandn(t, t, c);
        v.por(a, b, t);
    }
    v.zero(b);
    for (int k = 9; k >= 0; --k) {
        v.op(op::por, c, b, splat32(1u << k));
        if (!rsqrt) {
            v.op(op::pmulld, t, c, a, true);
            v.op(op::pcmpgtd, t, t, splat32(1u << 19));          // c * a' > 2^19: too big
        } else {
            v.op(op::pmulld, t, c, c, true);
            v.op(op::pmulld, t, t, a, true);
            v.op(op::pcmpgtd, t, t, splat32((1u << 28) - 1));    // a' * c^2 >= 2^28
        }
        v.blendv(b, c, b, t);
    }
    if (rsqrt) v.op(op::pmaxsd, b, b, splat32(512));   // the search starts at 512
    v.op(op::paddd, b, b, splat32(1));
    v.psrld(b, b, 1);
    v.pslld(b, b, 23);
    if (!rsqrt) {
        v.psrad(t, x, 31);                       // estimate only where bit 31 is set
    } else {
        v.psrld(t, x, 30);
        const Xmm z = c;
        v.zero(z);
        v.op(op::pcmpeqd, t, t, z, true);
        v.not_(t, t);                            // estimate where bits 31:30 != 0
    }
    v.op(op::pandn, t, t, v.k(splat8(0xff)));
    v.por(d, b, t);
}

} // namespace

// ============================================================================
// Three same (vector)
// ============================================================================

unsigned emit_three_same(EmitContext& ctx, std::uint32_t w) {
    const bool q = bit(w, 30), u = bit(w, 29);
    const unsigned size = bits(w, 23, 22), rm = bits(w, 20, 16), opc = bits(w, 15, 11);
    const unsigned rn = bits(w, 9, 5), rd = bits(w, 4, 0);
    if (opc >= 0x18) return 0;   // floating point
    const bool d64_bad = size == 3 && !q;
    switch (opc) {
    case 0x00: case 0x02: case 0x04: case 0x0c: case 0x0d: case 0x0e: case 0x0f:
    case 0x12: case 0x14: case 0x15:
        if (size == 3) return 0;
        break;
    case 0x01: case 0x05: case 0x06: case 0x07: case 0x08: case 0x09: case 0x0a: case 0x0b:
    case 0x10: case 0x11:
        if (d64_bad) return 0;
        break;
    case 0x03: break;
    case 0x13:
        if (u ? size != 0 : size == 3) return 0;
        break;
    case 0x16:
        if (size != 1 && size != 2) return 0;
        break;
    case 0x17:
        if (u || d64_bad) return 0;
        break;
    default: return 0;
    }

    if (opc >= 0x08 && opc <= 0x0b) {
        emit_register_shift(ctx, size, q ? 16u >> size : 8u >> size,
                            RegShift{u, (opc & 2) != 0, (opc & 1) != 0}, rd, rn, rm, false);
        return 1;
    }

    V v(ctx);
    // Single-instruction operations take their second source straight from
    // memory when the register is not in a host register.
    {
        SseOp o = kNone;
        bool comm = true, swap = false;
        switch (opc) {
        case 0x01: if (size < 2) o = u ? (size ? op::paddusw : op::paddusb) : (size ? op::paddsw : op::paddsb); break;
        case 0x05: if (size < 2) { o = u ? (size ? op::psubusw : op::psubusb) : (size ? op::psubsw : op::psubsb); comm = false; } break;
        case 0x03:
            if (!u && size == 0) o = op::pand;
            else if (!u && size == 1) { o = op::pandn; comm = false; swap = true; }   // BIC: ~m & n
            else if (!u && size == 2 && rn != rm) o = op::por;
            else if (u && size == 0) o = op::pxor;
            break;
        case 0x06: if (!u) { o = kCmpGt[size]; comm = false; } break;
        case 0x0c: if (size < 3) o = u ? kMaxU[size] : kMaxS[size]; break;
        case 0x0d: if (size < 3) o = u ? kMinU[size] : kMinS[size]; break;
        case 0x10: o = u ? kSub[size] : kAdd[size]; comm = !u; break;
        case 0x11: if (u) o = kCmpEq[size]; break;
        case 0x13: if (!u && (size == 1 || size == 2)) o = size == 1 ? op::pmullw : op::pmulld; break;
        default: break;
        }
        if (o.opcode != 0) {
            const Src n = source(ctx, rn);
            const Src m = rm == rn ? n : source(ctx, rm);
            // Every op here except CMEQ maps zero lanes to zero: with both
            // sources zero above bit 64 a 64-bit result needs no zeroing.
            const bool upper_zero = !q && opc != 0x11 && zero_above_64(ctx, rn) && zero_above_64(ctx, rm);
            const auto ops = swap ? bin_operands(ctx, m, n, comm) : bin_operands(ctx, n, m, comm);
            const Xmm d = ctx.write_vec(rd);
            v.op(o, d, ops.first, ops.second, comm);
            if (!upper_zero) v.finish(d, q);
            return 1;
        }
        if (opc == 0x03 && u && size != 0) {
            // BSL: d = m ^ ((m ^ n) & d); BIT: d = d ^ ((d ^ n) & m);
            // BIF: d = d ^ ((d ^ n) & ~m). Operands used once may stay in memory.
            const Src ns = source(ctx, rn);
            const Src ms = rm == rn ? ns : source(ctx, rm);
            const Src ds = rd == rn ? ns : rd == rm ? ms : source(ctx, rd);
            const bool upper_zero = !q && zero_above_64(ctx, rn) && zero_above_64(ctx, rm) && zero_above_64(ctx, rd);
            const Xmm t = v.temp();
            Xmm d;
            if (size == 1) {
                const Xmm mr = in_reg(ctx, ms);
                d = ctx.write_vec(rd);
                v.op(op::pxor, t, mr, ns, true);
                v.op(op::pand, t, t, ds, true);
                v.op(op::pxor, d, t, mr, true);
            } else {
                const Xmm dr = in_reg(ctx, ds);
                const Xmm mr = size == 3 ? in_reg(ctx, ms) : Xmm{};
                d = ctx.write_vec(rd);
                v.op(op::pxor, t, dr, ns, true);
                if (size == 2) v.op(op::pand, t, t, ms, true);
                else v.op(op::pandn, t, mr, t);
                v.op(op::pxor, d, dr, t, true);
            }
            if (!upper_zero) v.finish(d, q);
            return 1;
        }
        if (opc == 0x03 && !u && size == 2) {   // MOV (ORR Vd, Vn, Vn): keeps Vn's known-zero upper bits
            const Src n = source(ctx, rn);
            const bool z32 = ((ctx.vec_zero_above(32) >> rn) & 1u) != 0;
            const bool z64 = ((ctx.vec_zero_above(64) >> rn) & 1u) != 0;
            const Xmm d = ctx.write_vec(rd);
            if (n.is_mem) v.un(q ? op::movdqu : op::movq_load, d, n.mem);
            else if (q) v.mov(d, n.reg);
            else v.movq(d, n.reg);
            if (z32) ctx.note_vec_zero_above(rd, 32);
            else if (z64) ctx.note_vec_zero_above(rd, 64);
            return 1;
        }
    }
    const bool accumulates = opc == 0x0f || opc == 0x12 || (opc == 0x03 && u && size != 0);
    const Xmm n = ctx.read_vec(rn);
    const Xmm m = rm == rn ? n : ctx.read_vec(rm);
    const Xmm dold = accumulates ? (rd == rn ? n : rd == rm ? m : ctx.read_vec(rd)) : Xmm{};
    const Xmm d = ctx.write_vec(rd);

    switch (opc) {
    case 0x00: halving_add(v, size, u, false, d, n, m); break;
    case 0x01: sat_add_sub(v, size, u, false, d, n, m); break;
    case 0x02: halving_add(v, size, u, true, d, n, m); break;
    case 0x03: {
        if (!u) {
            switch (size) {
            case 0: v.pand(d, n, m); break;
            case 1: v.pandn(d, m, n); break;
            case 2: if (n == m) v.mov(d, n); else v.por(d, n, m); break;
            default: {
                const Xmm t = v.temp();
                v.not_(t, m);
                v.por(d, n, t);
                break;
            }
            }
        } else {
            const Xmm t = v.temp();
            switch (size) {
            case 0: v.pxor(d, n, m); break;
            case 1:   // BSL: d = m ^ ((m ^ n) & d)
                v.pxor(t, m, n);
                v.pand(t, t, dold);
                v.pxor(d, t, m);
                break;
            case 2:   // BIT: d = d ^ ((d ^ n) & m)
                v.pxor(t, dold, n);
                v.pand(t, t, m);
                v.pxor(d, dold, t);
                break;
            default:  // BIF: d = d ^ ((d ^ n) & ~m)
                v.pxor(t, dold, n);
                v.pandn(t, m, t);
                v.pxor(d, dold, t);
                break;
            }
        }
        break;
    }
    case 0x04: halving_sub(v, size, u, d, n, m); break;
    case 0x05: sat_add_sub(v, size, u, true, d, n, m); break;
    case 0x06:
        if (u) v.cmp_gt_u(size, d, n, m);
        else v.cmp_gt_s(size, d, n, m);
        break;
    case 0x07:
        if (u) v.cmp_ge_u(size, d, n, m);
        else v.cmp_ge_s(size, d, n, m);
        break;
    case 0x0c:
        if (u) v.max_u(size, d, n, m); else v.max_s(size, d, n, m);
        break;
    case 0x0d:
        if (u) v.min_u(size, d, n, m); else v.min_s(size, d, n, m);
        break;
    case 0x0e: abd(v, size, u, d, n, m); break;
    case 0x0f: {
        const Xmm t = v.temp();
        abd(v, size, u, t, n, m);
        v.op(kAdd[size], d, dold, t, true);
        break;
    }
    case 0x10:
        if (u) v.op(kSub[size], d, n, m);
        else v.op(kAdd[size], d, n, m, true);
        break;
    case 0x11:
        if (u) {
            v.op(kCmpEq[size], d, n, m, true);
        } else if (size < 3) {   // CMTST: min(n & m, 1) == 1
            const Xmm t = v.temp();
            v.pand(t, n, m);
            v.op(kMinU[size], t, t, splat(size, 1));
            v.op(kCmpEq[size], d, t, splat(size, 1));
        } else {
            const Xmm t = v.temp();
            const Xmm z = v.temp();
            v.pand(t, n, m);
            v.zero(z);
            v.op(op::pcmpeqq, t, t, z, true);
            v.not_(d, t);
        }
        break;
    case 0x12: {
        const Xmm t = v.temp();
        mul(v, size, t, n, m);
        if (u) v.op(kSub[size], d, dold, t);
        else v.op(kAdd[size], d, dold, t, true);
        break;
    }
    case 0x13:
        if (u) pmul8(v, d, n, m);
        else mul(v, size, d, n, m);
        break;
    case 0x14: pairwise(v, size, q, u ? Pairwise::umax : Pairwise::smax, d, n, m); return 1;
    case 0x15: pairwise(v, size, q, u ? Pairwise::umin : Pairwise::smin, d, n, m); return 1;
    case 0x16: sqdmulh(v, size, u, d, n, m); break;
    case 0x17: pairwise(v, size, q, Pairwise::add, d, n, m); return 1;
    default: break;
    }
    v.finish(d, q);
    return 1;
}

// ============================================================================
// Three different (vector)
// ============================================================================

unsigned emit_three_different(EmitContext& ctx, std::uint32_t w) {
    const bool high = bit(w, 30), u = bit(w, 29);
    const unsigned size = bits(w, 23, 22), rm = bits(w, 20, 16), opc = bits(w, 15, 12);
    const unsigned rn = bits(w, 9, 5), rd = bits(w, 4, 0);
    if (opc == 0xf) return 0;
    if (opc == 0xe) {   // PMULL
        if (u) return 0;
        if (size == 3) {
            if (!features().pclmul) return 0;
        } else if (size != 0) {
            return 0;
        }
    } else {
        if (size == 3) return 0;
        if ((opc == 0x9 || opc == 0xb || opc == 0xd) && (u || size == 0)) return 0;
    }

    V v(ctx);
    const bool accumulates = opc == 0x5 || opc == 0x8 || opc == 0x9 || opc == 0xa || opc == 0xb;
    const bool narrows = opc == 0x4 || opc == 0x6;
    const Xmm n = ctx.read_vec(rn);
    const Xmm m = ctx.read_vec(rm);
    const Xmm dold = (accumulates || (narrows && high)) ? ctx.read_vec(rd) : Xmm{};
    const Xmm d = ctx.write_vec(rd);
    const unsigned wide = size + 1;

    switch (opc) {
    case 0x0: case 0x2: {   // SADDL/UADDL, SSUBL/USUBL
        const Xmm a = v.temp();
        const Xmm b = v.temp();
        widen(v, size, !u, high, a, n);
        widen(v, size, !u, high, b, m);
        if (opc == 0x0) v.op(kAdd[wide], d, a, b, true);
        else v.op(kSub[wide], d, a, b);
        break;
    }
    case 0x1: case 0x3: {   // SADDW/UADDW, SSUBW/USUBW
        const Xmm b = v.temp();
        widen(v, size, !u, high, b, m);
        if (opc == 0x1) v.op(kAdd[wide], d, n, b, true);
        else v.op(kSub[wide], d, n, b);
        break;
    }
    case 0x4: case 0x6: {   // (R)ADDHN, (R)SUBHN
        const Xmm t = v.temp();
        if (opc == 0x4) v.op(kAdd[wide], t, n, m, true);
        else v.op(kSub[wide], t, n, m);
        const unsigned half = 8u << size;
        if (u) v.op(kAdd[wide], t, t, splat(wide, 1ULL << (half - 1)));
        static const Const128 high_halves[3] = {
            bytes_const({1, 3, 5, 7, 9, 11, 13, 15, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80}),
            bytes_const({2, 3, 6, 7, 10, 11, 14, 15, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80}),
            bytes_const({4, 5, 6, 7, 12, 13, 14, 15, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80}),
        };
        if (high) {
            v.pshufb(t, t, high_halves[size]);
            insert_upper(v, d, dold, t);
        } else {
            v.pshufb(d, t, high_halves[size]);
        }
        return 1;
    }
    case 0x5: case 0x7: {   // SABAL/UABAL, SABDL/UABDL
        const Xmm t = v.temp();
        abd(v, size, u, t, n, m);
        if (opc == 0x7) {
            widen(v, size, false, high, d, t);
        } else {
            widen(v, size, false, high, t, t);
            v.op(kAdd[wide], d, dold, t, true);
        }
        break;
    }
    case 0x8: case 0xa: case 0xc: {   // SMLAL/UMLAL, SMLSL/UMLSL, SMULL/UMULL
        if (opc == 0xc) {
            mull(v, size, !u, high, d, n, m);
        } else {
            const Xmm t = v.temp();
            mull(v, size, !u, high, t, n, m);
            if (opc == 0x8) v.op(kAdd[wide], d, dold, t, true);
            else v.op(kSub[wide], d, dold, t);
        }
        break;
    }
    case 0x9: case 0xb: case 0xd: {   // SQDMLAL, SQDMLSL, SQDMULL
        if (opc == 0xd) {
            sqdmull(v, size, high, d, n, m);
        } else {
            const Xmm t = v.temp();
            sqdmull(v, size, high, t, n, m);
            sat_add_sub(v, wide, false, opc == 0xb, d, dold, t);
        }
        break;
    }
    case 0xe:
        if (size == 3) v.op_imm(op::pclmulqdq, d, n, m, high ? 0x11 : 0x00);
        else pmull8(v, high, d, n, m);
        break;
    default: break;
    }
    return 1;
}

// ============================================================================
// Two-register misc (vector)
// ============================================================================

namespace {

const Const128 kRev64[3][2] = {
    {bytes_const({7, 6, 5, 4, 3, 2, 1, 0, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80}),
     bytes_const({7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8})},
    {bytes_const({6, 7, 4, 5, 2, 3, 0, 1, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80}),
     bytes_const({6, 7, 4, 5, 2, 3, 0, 1, 14, 15, 12, 13, 10, 11, 8, 9})},
    {bytes_const({4, 5, 6, 7, 0, 1, 2, 3, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80}),
     bytes_const({4, 5, 6, 7, 0, 1, 2, 3, 12, 13, 14, 15, 8, 9, 10, 11})},
};
const Const128 kRev32[2][2] = {
    {bytes_const({3, 2, 1, 0, 7, 6, 5, 4, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80}),
     bytes_const({3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12})},
    {bytes_const({2, 3, 0, 1, 6, 7, 4, 5, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80}),
     bytes_const({2, 3, 0, 1, 6, 7, 4, 5, 10, 11, 8, 9, 14, 15, 12, 13})},
};
const Const128 kRev16[2] = {
    bytes_const({1, 0, 3, 2, 5, 4, 7, 6, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80}),
    bytes_const({1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14}),
};

} // namespace

unsigned emit_two_reg_misc(EmitContext& ctx, std::uint32_t w) {
    const bool q = bit(w, 30), u = bit(w, 29);
    const unsigned size = bits(w, 23, 22), opc = bits(w, 16, 12);
    const unsigned rn = bits(w, 9, 5), rd = bits(w, 4, 0);
    const bool d64_bad = size == 3 && !q;
    // Validate.
    switch (opc) {
    case 0x00:
        if (u ? size > 1 : size > 2) return 0;
        break;
    case 0x01:
        if (u || size != 0) return 0;
        break;
    case 0x02: case 0x06: case 0x04:
        if (size == 3) return 0;
        break;
    case 0x03: case 0x07: case 0x08: case 0x09: case 0x0b:
        if (d64_bad) return 0;
        break;
    case 0x0a:
        if (u || d64_bad) return 0;
        break;
    case 0x05:
        if (u ? size > 1 : size != 0) return 0;
        break;
    case 0x12: case 0x14:
        if (size == 3) return 0;
        break;
    case 0x13:
        if (!u || size == 3) return 0;
        break;
    case 0x1c:   // URECPE / URSQRTE (sz = 0); other sizes are FP
        if (size != 2) return 0;
        break;
    default: return 0;
    }

    V v(ctx);
    const bool accumulates = opc == 0x03 || opc == 0x06;
    const bool narrows = opc == 0x12 || opc == 0x14;
    const Xmm n = ctx.read_vec(rn);
    const Xmm dold = (accumulates || (narrows && q)) ? ctx.read_vec(rd) : Xmm{};
    const Xmm d = ctx.write_vec(rd);

    switch (opc) {
    case 0x00:
        if (u) v.pshufb(d, n, kRev32[size][q]);
        else v.pshufb(d, n, kRev64[size][q]);
        return 1;
    case 0x01:
        v.pshufb(d, n, kRev16[q]);
        return 1;
    case 0x02: addlp(v, size, u, d, n); break;
    case 0x06: {
        const Xmm t = v.temp();
        addlp(v, size, u, t, n);
        v.op(kAdd[size + 1], d, dold, t, true);
        break;
    }
    case 0x03:
        if (u) usqadd(v, size, d, dold, n);
        else suqadd(v, size, d, dold, n);
        break;
    case 0x04:
        if (u) {
            clz(v, size, d, n);
        } else {   // CLS = CLZ(x ^ sign(x)) - 1
            const Xmm t = v.temp();
            if (size == 0) {
                const Xmm z = v.temp();
                v.zero(z);
                v.op(op::pcmpgtb, t, z, n);
            } else {
                v.sign_mask(size, t, n);
            }
            v.pxor(t, t, n);
            clz(v, size, t, t);
            v.op(kSub[size], d, t, splat(size, 1));
        }
        break;
    case 0x05:
        if (!u) cnt8(v, d, n);
        else if (size == 0) {
            if (q) v.not_(d, n);
            else { v.op(op::pandn, d, n, v.k(Const128{~0ULL, 0})); return 1; }
        } else {
            rbit8(v, d, n);
        }
        break;
    case 0x07:
        if (u) sqneg(v, size, d, n); else sqabs(v, size, d, n);
        break;
    case 0x08: cmp_zero(v, size, u ? 1 : 0, d, n); break;
    case 0x09: cmp_zero(v, size, u ? 3 : 2, d, n); break;
    case 0x0a: cmp_zero(v, size, 4, d, n); break;
    case 0x0b:
        if (u) neg(v, size, d, n); else abs_(v, size, d, n);
        break;
    case 0x12: case 0x14: {
        const Narrow kind = opc == 0x12 ? (u ? Narrow::sat_signed_to_unsigned : Narrow::truncate)
                                        : (u ? Narrow::sat_unsigned : Narrow::sat_signed);
        if (q) {
            const Xmm t = v.temp();
            narrow(v, size, kind, t, n);
            insert_upper(v, d, dold, t);
        } else {
            narrow(v, size, kind, d, n);
        }
        return 1;
    }
    case 0x13: {   // SHLL/SHLL2
        const Xmm z = v.temp();
        v.zero(z);
        v.op(q ? kUnpackHi[size] : kUnpackLo[size], d, z, n);
        return 1;
    }
    case 0x1c: unsigned_estimate(v, u, d, n); break;
    default: break;
    }
    v.finish(d, q);
    return 1;
}

// ============================================================================
// Across lanes
// ============================================================================

unsigned emit_across_lanes(EmitContext& ctx, std::uint32_t w) {
    const bool q = bit(w, 30), u = bit(w, 29);
    const unsigned size = bits(w, 23, 22), opc = bits(w, 16, 12);
    const unsigned rn = bits(w, 9, 5), rd = bits(w, 4, 0);
    if (size == 3 || (size == 2 && !q)) return 0;
    if (opc != 0x03 && opc != 0x0a && opc != 0x1a && !(opc == 0x1b && !u)) return 0;

    V v(ctx);
    const Xmm src = ctx.read_vec(rn);
    const Xmm d = ctx.write_vec(rd);
    const Xmm n = v.temp();

    switch (opc) {
    case 0x1b:   // ADDV
        if (size == 0) {
            const Xmm z = v.temp();
            v.zero(z);
            v.op(op::psadbw, n, src, z, true);
            if (q) {
                v.pshufd(z, n, 0xee);
                v.op(op::paddd, n, n, z, true);
            }
            v.op(op::pand, d, n, Const128{0xff, 0});
        } else if (size == 1) {
            v.op(op::pmaddwd, n, src, q ? splat16(1) : Const128{rep16(1), 0});
            hsum32(v, n, n);
            v.op(op::pand, d, n, Const128{0xffff, 0});
        } else {
            hsum32(v, n, src);
            v.keep_element0(2, d, n);
        }
        return 1;
    case 0x03:   // SADDLV / UADDLV
        if (size == 0) {
            const Xmm z = v.temp();
            v.zero(z);
            if (u) {
                v.op(op::psadbw, n, src, z, true);
            } else {
                v.op(op::pxor, n, src, splat8(0x80));
                v.op(op::psadbw, n, n, z, true);
            }
            if (q) {
                v.pshufd(z, n, 0xee);
                v.op(op::paddq, n, n, z, true);
            }
            if (u) {
                v.movq(d, n);
            } else {
                v.movq(n, n);
                v.op(op::psubw, d, n, Const128{q ? 2048u : 1024u, 0});
            }
        } else if (size == 1) {
            const Xmm x = q ? src : v.temp();
            if (!q) v.movq(x, src);
            if (u) {
                const Xmm t = v.temp();
                v.psrld(t, x, 16);
                v.op(op::pand, n, x, splat32(0xffff));
                v.op(op::paddd, n, n, t, true);
            } else {
                v.op(op::pmaddwd, n, x, splat16(1));
            }
            hsum32(v, n, n);
            v.keep_element0(2, d, n);
        } else {
            const Xmm t = v.temp();
            const SseOp ext = u ? op::pmovzxdq : op::pmovsxdq;
            v.un(ext, n, src);
            v.pshufd(t, src, 0xee);
            v.un(ext, t, t);
            v.op(op::paddq, n, n, t, true);
            v.pshufd(t, n, 0xee);
            v.op(op::paddq, n, n, t, true);
            v.movq(d, n);
        }
        return 1;
    default: break;
    }

    // SMAXV/UMAXV (0x0a) and SMINV/UMINV (0x1a)
    const bool is_max = opc == 0x0a;
    if (size == 2) {
        const Xmm t = v.temp();
        v.pshufd(t, src, 0xee);
        if (is_max) { if (u) v.max_u(2, n, src, t); else v.max_s(2, n, src, t); }
        else { if (u) v.min_u(2, n, src, t); else v.min_s(2, n, src, t); }
        v.pshufd(t, n, 0x55);
        if (is_max) { if (u) v.max_u(2, n, n, t); else v.max_s(2, n, n, t); }
        else { if (u) v.min_u(2, n, n, t); else v.min_s(2, n, n, t); }
        v.keep_element0(2, d, n);
        return 1;
    }
    // Map to an unsigned minimum: key = x ^ flip, result = min(key) ^ flip.
    // UMIN: 0; UMAX: ~0; SMIN: sign bit; SMAX: ~sign bit.
    const std::uint64_t mask = size == 0 ? 0xff : 0xffff;
    const std::uint64_t sbit = size == 0 ? 0x80 : 0x8000;
    const std::uint64_t flip = u ? (is_max ? mask : 0) : (is_max ? mask ^ sbit : sbit);
    if (!q) v.pshufd(n, src, 0x44);
    const Xmm x = q ? src : n;
    if (size == 1) {
        if (flip != 0) {
            v.op(op::pxor, n, x, splat16(static_cast<std::uint16_t>(flip)));
            v.un(op::phminposuw, n, n);
        } else {
            v.un(op::phminposuw, n, x);
        }
    } else {
        const Xmm t = v.temp();
        if (flip != 0) {
            v.op(op::pxor, n, x, splat8(static_cast<std::uint8_t>(flip)));
            v.psrlw(t, n, 8);
            v.op(op::pminub, n, n, t, true);
        } else {
            v.psrlw(t, x, 8);
            v.op(op::pminub, n, x, t, true);
        }
        // High bytes are min(hi, 0) = 0, so every word holds a pair minimum.
        v.un(op::phminposuw, n, n);
    }
    // n = {min, index, 0...}: keep the low element and undo the flip.
    if (flip == mask) {
        v.op(op::pandn, d, n, v.k(Const128{mask, 0}));
    } else if (flip == 0) {
        v.op(op::pand, d, n, Const128{mask, 0});
    } else {
        v.op(op::pand, n, n, Const128{mask, 0});
        v.op(op::pxor, d, n, Const128{flip, 0});
    }
    return 1;
}

// ============================================================================
// Vector x indexed element
// ============================================================================

unsigned emit_indexed(EmitContext& ctx, std::uint32_t w) {
    const bool q = bit(w, 30), u = bit(w, 29);
    const unsigned size = bits(w, 23, 22), opc = bits(w, 15, 12);
    const unsigned rn = bits(w, 9, 5), rd = bits(w, 4, 0);
    const unsigned l = bits(w, 21, 21), mbit = bits(w, 20, 20), h = bits(w, 11, 11);
    // Integer encodings only.
    bool ok = false;
    switch (opc) {
    case 0x0: case 0x4: ok = u; break;                     // MLA, MLS
    case 0x8: ok = !u; break;                              // MUL
    case 0x2: case 0x6: case 0xa: ok = true; break;        // [SU]MLAL, [SU]MLSL, [SU]MULL
    case 0x3: case 0x7: case 0xb: case 0xc: ok = !u; break;   // SQDMLAL/SQDMLSL/SQDMULL/SQDMULH
    case 0xd: ok = true; break;                            // SQRDMULH / SQRDMLAH
    case 0xf: ok = u || size == 0 || size == 2; break;   // SQRDMLSH (U=1), SUDOT/USDOT (U=0)
    case 0xe: ok = size == 2; break;                       // SDOT/UDOT
    default: break;
    }
    if (!ok) return 0;
    if (!(opc == 0xf && !u) && size != 1 && size != 2) return 0;   // SUDOT/USDOT use size 00/10
    const bool rdm = u && (opc == 0xd || opc == 0xf);              // SQRDMLAH / SQRDMLSH
    const bool dot_form = opc == 0xe || (opc == 0xf && !u);
    const unsigned index = size == 1 ? (h << 2) | (l << 1) | mbit : (h << 1) | l;
    const unsigned rm = size == 1 ? bits(w, 19, 16) : bits(w, 20, 16);

    V v(ctx);
    const bool accumulates = opc == 0x0 || opc == 0x4 || opc == 0x2 || opc == 0x6 ||
                             opc == 0x3 || opc == 0x7 || dot_form || rdm;
    const Xmm n = ctx.read_vec(rn);
    const unsigned esize = dot_form ? 2 : size;
    const Src mv = rm == rn ? Src{n, {}, false, rm} : dup_source(v, esize, element_source(ctx, rm, esize));
    const Xmm dold = accumulates ? (rd == rn ? n : ctx.read_vec(rd)) : Xmm{};
    const Xmm d = ctx.write_vec(rd);
    const Xmm m = v.temp();
    if (dot_form) {   // dot: broadcast the 32-bit group
        dup_element(v, 2, m, mv, index);
        if (opc == 0xf) dot(v, size == 2, size != 2, q, d, dold, n, m);   // USDOT / SUDOT
        else dot(v, u, u, q, d, dold, n, m);
        return 1;
    }
    dup_element(v, size, m, mv, index);
    const unsigned wide = size + 1;
    if (rdm) {
        sqrdmlah(v, size, opc == 0xf, d, dold, n, m);
        v.finish(d, q);
        return 1;
    }

    switch (opc) {
    case 0x8: mul(v, size, d, n, m); break;
    case 0x0: case 0x4: {
        const Xmm t = v.temp();
        mul(v, size, t, n, m);
        if (opc == 0x0) v.op(kAdd[size], d, dold, t, true);
        else v.op(kSub[size], d, dold, t);
        break;
    }
    case 0xa: case 0x2: case 0x6: {
        const bool madd = !u && size == 1 && neon_on(neon_old_mull_elem);
        const Xmm t = opc == 0xa ? d : v.temp();
        if (madd) smull16_splat(v, q, t, n, m);
        else mull(v, size, !u, q, t, n, m);
        if (opc == 0x2) v.op(kAdd[wide], d, dold, t, true);
        else if (opc == 0x6) v.op(kSub[wide], d, dold, t);
        return 1;
    }
    case 0xb: sqdmull(v, size, q, d, n, m, neon_on(neon_old_mull_elem)); return 1;
    case 0x3: case 0x7: {
        const Xmm t = v.temp();
        sqdmull(v, size, q, t, n, m, neon_on(neon_old_mull_elem));
        sat_add_sub(v, wide, false, opc == 0x7, d, dold, t);
        return 1;
    }
    case 0xc: case 0xd: sqdmulh(v, size, opc == 0xd, d, n, m); break;
    default: break;
    }
    v.finish(d, q);
    return 1;
}

// ============================================================================
// Dot product (three-register extension group)
// ============================================================================

unsigned emit_dot_product(EmitContext& ctx, std::uint32_t w) {
    const bool q = bit(w, 30), u = bit(w, 29);
    const unsigned size = bits(w, 23, 22), opc = bits(w, 15, 11) & 0xf;
    enum class Form { dot, usdot, rdm_add, rdm_sub, mmla } form;
    if (opc == 0x2 && size == 2) form = Form::dot;                          // SDOT/UDOT
    else if (opc == 0x3 && !u && size == 2) form = Form::usdot;             // USDOT
    else if ((opc == 0x0 || opc == 0x1) && u && (size == 1 || size == 2))   // SQRDMLAH/SQRDMLSH
        form = opc == 0x0 ? Form::rdm_add : Form::rdm_sub;
    else if (((opc == 0x4) || (opc == 0x5 && !u)) && size == 2 && q) form = Form::mmla;   // [SU]MMLA, USMMLA
    else return 0;
    const unsigned rm = bits(w, 20, 16), rn = bits(w, 9, 5), rd = bits(w, 4, 0);
    V v(ctx);
    const Xmm n = ctx.read_vec(rn);
    const Xmm m = ctx.read_vec(rm);
    const Xmm dold = ctx.read_vec(rd);
    const Xmm d = ctx.write_vec(rd);
    switch (form) {
    case Form::dot: dot(v, u, u, q, d, dold, n, m); break;
    case Form::usdot: dot(v, true, false, q, d, dold, n, m); break;
    case Form::rdm_add: case Form::rdm_sub:
        sqrdmlah(v, size, form == Form::rdm_sub, d, dold, n, m);
        v.finish(d, q);
        break;
    case Form::mmla:
        if (opc == 0x5) mmla(v, true, false, d, dold, n, m);
        else mmla(v, u, u, d, dold, n, m);
        break;
    }
    return 1;
}

// SQRDMLAH / SQRDMLSH (scalar, three same extra).
unsigned emit_scalar_three_same_extra(EmitContext& ctx, std::uint32_t w) {
    const bool u = bit(w, 29);
    const unsigned size = bits(w, 23, 22), opc = bits(w, 15, 11) & 0xf;
    if (!u || (opc != 0x0 && opc != 0x1) || (size != 1 && size != 2)) return 0;
    const unsigned rm = bits(w, 20, 16), rn = bits(w, 9, 5), rd = bits(w, 4, 0);
    V v(ctx);
    const Xmm n = ctx.read_vec(rn);
    const Xmm m = ctx.read_vec(rm);
    const Xmm dold = ctx.read_vec(rd);
    const Xmm d = ctx.write_vec(rd);
    const Xmm t = v.temp();
    sqrdmlah(v, size, opc == 0x1, t, dold, n, m);
    v.keep_element0(size, d, t);
    return 1;
}

// ============================================================================
// Scalar forms
// ============================================================================

unsigned emit_scalar_three_same(EmitContext& ctx, std::uint32_t w) {
    const bool u = bit(w, 29);
    const unsigned size = bits(w, 23, 22), rm = bits(w, 20, 16), opc = bits(w, 15, 11);
    const unsigned rn = bits(w, 9, 5), rd = bits(w, 4, 0);
    switch (opc) {
    case 0x01: case 0x05: case 0x09: case 0x0b: break;            // any size
    case 0x06: case 0x07: case 0x08: case 0x0a: case 0x10: case 0x11:
        if (size != 3) return 0;
        break;
    case 0x16:
        if (size != 1 && size != 2) return 0;
        break;
    default: return 0;
    }
    if (opc >= 0x08 && opc <= 0x0b) {
        emit_register_shift(ctx, size, 1, RegShift{u, (opc & 2) != 0, (opc & 1) != 0}, rd, rn, rm, true);
        return 1;
    }
    V v(ctx);
    const Xmm n = ctx.read_vec(rn);
    const Xmm m = ctx.read_vec(rm);
    const Xmm d = ctx.write_vec(rd);
    const Xmm t = v.temp();
    switch (opc) {
    case 0x01: sat_add_sub(v, size, u, false, t, n, m); break;
    case 0x05: sat_add_sub(v, size, u, true, t, n, m); break;
    case 0x06: if (u) v.cmp_gt_u(3, t, n, m); else v.cmp_gt_s(3, t, n, m); break;
    case 0x07: if (u) v.cmp_ge_u(3, t, n, m); else v.cmp_ge_s(3, t, n, m); break;
    case 0x10: if (u) v.op(op::psubq, t, n, m); else v.op(op::paddq, t, n, m, true); break;
    case 0x11:
        if (u) {
            v.op(op::pcmpeqq, t, n, m, true);
        } else {
            const Xmm z = v.temp();
            v.pand(t, n, m);
            v.zero(z);
            v.op(op::pcmpeqq, t, t, z, true);
            v.not_(t, t);
        }
        break;
    case 0x16: sqdmulh(v, size, u, t, n, m); break;
    default: break;
    }
    v.keep_element0(size, d, t);
    return 1;
}

unsigned emit_scalar_three_different(EmitContext& ctx, std::uint32_t w) {
    const bool u = bit(w, 29);
    const unsigned size = bits(w, 23, 22), rm = bits(w, 20, 16), opc = bits(w, 15, 12);
    const unsigned rn = bits(w, 9, 5), rd = bits(w, 4, 0);
    if (u || (size != 1 && size != 2)) return 0;
    if (opc != 0x9 && opc != 0xb && opc != 0xd) return 0;
    V v(ctx);
    const Xmm n = ctx.read_vec(rn);
    const Xmm m = ctx.read_vec(rm);
    const Xmm dold = opc != 0xd ? ctx.read_vec(rd) : Xmm{};
    const Xmm d = ctx.write_vec(rd);
    const Xmm t = v.temp();
    sqdmull(v, size, false, t, n, m);
    if (opc != 0xd) sat_add_sub(v, size + 1, false, opc == 0xb, t, dold, t);
    v.keep_element0(size + 1, d, t);
    return 1;
}

unsigned emit_scalar_two_reg_misc(EmitContext& ctx, std::uint32_t w) {
    const bool u = bit(w, 29);
    const unsigned size = bits(w, 23, 22), opc = bits(w, 16, 12);
    const unsigned rn = bits(w, 9, 5), rd = bits(w, 4, 0);
    switch (opc) {
    case 0x03: case 0x07: break;
    case 0x08: case 0x09: case 0x0b:
        if (size != 3) return 0;
        break;
    case 0x0a:
        if (u || size != 3) return 0;
        break;
    case 0x12:
        if (!u || size == 3) return 0;
        break;
    case 0x14:
        if (size == 3) return 0;
        break;
    default: return 0;
    }
    V v(ctx);
    const Xmm n = ctx.read_vec(rn);
    const Xmm dold = opc == 0x03 ? ctx.read_vec(rd) : Xmm{};
    const Xmm d = ctx.write_vec(rd);
    const Xmm t = v.temp();
    switch (opc) {
    case 0x03:
        if (u) usqadd(v, size, t, dold, n); else suqadd(v, size, t, dold, n);
        break;
    case 0x07: if (u) sqneg(v, size, t, n); else sqabs(v, size, t, n); break;
    case 0x08: cmp_zero(v, 3, u ? 1 : 0, t, n); break;
    case 0x09: cmp_zero(v, 3, u ? 3 : 2, t, n); break;
    case 0x0a: cmp_zero(v, 3, 4, t, n); break;
    case 0x0b: if (u) neg(v, 3, t, n); else abs_(v, 3, t, n); break;
    case 0x12: case 0x14: {
        const Narrow kind = opc == 0x12 ? Narrow::sat_signed_to_unsigned
                                        : (u ? Narrow::sat_unsigned : Narrow::sat_signed);
        narrow(v, size, kind, t, n);
        break;
    }
    default: break;
    }
    v.keep_element0(size, d, t);
    return 1;
}

unsigned emit_scalar_pairwise(EmitContext& ctx, std::uint32_t w) {
    const bool u = bit(w, 29);
    const unsigned size = bits(w, 23, 22), opc = bits(w, 16, 12);
    if (u || opc != 0x1b || size != 3) return 0;   // ADDP Dd, Vn.2D
    const unsigned rn = bits(w, 9, 5), rd = bits(w, 4, 0);
    V v(ctx);
    const Xmm n = ctx.read_vec(rn);
    const Xmm d = ctx.write_vec(rd);
    const Xmm t = v.temp();
    v.pshufd(t, n, 0xee);
    v.op(op::paddq, t, t, n, true);
    v.movq(d, t);
    return 1;
}

unsigned emit_scalar_indexed(EmitContext& ctx, std::uint32_t w) {
    const bool u = bit(w, 29);
    const unsigned size = bits(w, 23, 22), opc = bits(w, 15, 12);
    const unsigned rn = bits(w, 9, 5), rd = bits(w, 4, 0);
    const unsigned l = bits(w, 21, 21), mbit = bits(w, 20, 20), h = bits(w, 11, 11);
    if (size != 1 && size != 2) return 0;
    const bool rdm = u && (opc == 0xd || opc == 0xf);
    if (u && !rdm) return 0;
    if (!rdm && opc != 0x3 && opc != 0x7 && opc != 0xb && opc != 0xc && opc != 0xd) return 0;
    const unsigned index = size == 1 ? (h << 2) | (l << 1) | mbit : (h << 1) | l;
    const unsigned rm = size == 1 ? bits(w, 19, 16) : bits(w, 20, 16);
    V v(ctx);
    const bool accumulates = opc == 0x3 || opc == 0x7 || rdm;
    const Xmm n = ctx.read_vec(rn);
    const Xmm mv = ctx.read_vec(rm);
    const Xmm dold = accumulates ? ctx.read_vec(rd) : Xmm{};
    const Xmm d = ctx.write_vec(rd);
    const Xmm m = v.temp();
    const Xmm t = v.temp();
    dup_element(v, size, m, mv, index);
    if (rdm) {
        sqrdmlah(v, size, opc == 0xf, t, dold, n, m);
        v.keep_element0(size, d, t);
        return 1;
    }
    if (opc == 0xc || opc == 0xd) {
        sqdmulh(v, size, opc == 0xd, t, n, m);
        v.keep_element0(size, d, t);
        return 1;
    }
    sqdmull(v, size, false, t, n, m);
    if (accumulates) sat_add_sub(v, size + 1, false, opc == 0x7, t, dold, t);
    v.keep_element0(size + 1, d, t);
    return 1;
}

} // namespace transmute::fx::neon
