// Integer SIMD shifts: by immediate (vector and scalar, including narrowing,
// widening, rounding, saturating and insert forms) and by register
// (owner: neon).
#include "fx/translator/emit_neon_internal.hpp"

#include <bit>

namespace transmute::fx::neon {

namespace {

constexpr std::uint64_t elem_mask(unsigned size) { return size == 3 ? ~0ULL : (1ULL << (8u << size)) - 1; }
constexpr std::uint64_t smax(unsigned size) { return (1ULL << ((8u << size) - 1)) - 1; }

// Rounding right shift by n (1..esize): floor((x + 2^(n-1)) / 2^n).
void rshr(V& v, unsigned size, bool is_signed, Xmm d, Xmm x, unsigned n) {
    const unsigned esize = 8u << size;
    if (!is_signed) {
        if (size < 2) {   // pavg(x >> (n-1), 0)
            const Xmm t = v.temp();
            const Xmm z = v.temp();
            v.srl(size, t, x, n - 1);
            v.zero(z);
            v.op(size == 0 ? op::pavgb : op::pavgw, d, t, z, true);
            return;
        }
        const Xmm t = v.temp();
        const Xmm h = v.temp();
        v.srl(size, t, x, n - 1);
        v.srl(size, h, t, 1);
        v.op(kSub[size], d, t, h);
        return;
    }
    if (size == 0) {
        if (n == esize) { v.zero(d); return; }
        // SRSHR(x) = URSHR(x ^ 0x80) - (0x80 >> n)
        const Xmm t = v.temp();
        const Xmm z = v.temp();
        v.op(op::pxor, t, x, splat8(0x80));
        v.srl(0, t, t, n - 1);
        v.zero(z);
        v.op(op::pavgb, t, t, z, true);
        v.op(op::psubb, d, t, splat8(static_cast<std::uint8_t>(0x80u >> n)));
        return;
    }
    if (size == 3) {   // (x >>a n) + ((x >> (n-1)) & 1)
        const Xmm t = v.temp();
        const Xmm b = v.temp();
        v.sra(3, t, x, n);
        v.srl(3, b, x, n - 1);
        v.op(op::pand, b, b, splat64(1));
        v.op(op::paddq, d, t, b, true);
        return;
    }
    const Xmm t = v.temp();   // t - (t >>a 1), t = x >>a (n-1)
    const Xmm h = v.temp();
    v.sra(size, t, x, n - 1);
    v.sra(size, h, t, 1);
    v.op(kSub[size], d, t, h);
}

// Rounding right shift for the saturating narrowing forms (source size).
void rshr_exact(V& v, unsigned size, bool is_signed, Xmm d, Xmm x, unsigned n) {
    // (x >> n) + ((x >> (n-1)) & 1): exact, no overflow of the element.
    const Xmm t = v.temp();
    const Xmm b = v.temp();
    if (is_signed) v.sra(size, t, x, n); else v.srl(size, t, x, n);
    v.srl(size, b, x, n - 1);
    v.op(op::pand, b, b, splat(size, 1));
    v.op(kAdd[size], d, t, b, true);
}

// Saturating left shift by immediate n (0..esize-1).
enum class SatShl { signed_, unsigned_, signed_to_unsigned };

void sat_shl(V& v, unsigned size, SatShl kind, Xmm d, Xmm x, unsigned n) {
    const Xmm r = v.temp();
    const Xmm back = v.temp();
    v.shl(size, r, x, n);
    if (kind == SatShl::signed_) v.sra(size, back, r, n);
    else v.srl(size, back, r, n);
    v.op(kCmpEq[size], back, back, x, true);   // 1s where no overflow
    if (kind == SatShl::signed_) {
        const Xmm s = v.temp();
        v.sign_mask(size, s, x);
        v.op(op::pxor, s, s, splat(size, smax(size)));
        v.blendv(d, s, r, back);
        return;
    }
    // r | ~ok
    v.op(op::pandn, back, back, v.k(splat8(0xff)));
    if (kind == SatShl::unsigned_) {
        v.por(d, r, back);
        return;
    }
    v.por(r, r, back);
    const Xmm s = v.temp();
    v.sign_mask(size, s, x);
    v.pandn(d, s, r);
}

// Core of the immediate shift group shared by vector and scalar forms.
// Returns false for encodings that are not handled. `size` is the element
// size from immh. d receives a full 128-bit result (caller trims).
struct ShiftImm {
    unsigned opc;
    bool u;
    unsigned size;
    unsigned immhb;
};

bool shift_imm_valid(const ShiftImm& s, bool scalar, bool q) {
    switch (s.opc) {
    case 0x00: case 0x02: case 0x04: case 0x06: case 0x0a:
        if (scalar) return s.size == 3;
        return s.size != 3 || q;
    case 0x08:
        if (!s.u) return false;
        if (scalar) return s.size == 3;
        return s.size != 3 || q;
    case 0x0c:
        if (!s.u) return false;
        return scalar || s.size != 3 || q;
    case 0x0e:
        return scalar || s.size != 3 || q;
    case 0x10: case 0x11:
        if (scalar && !s.u) return false;   // scalar SHRN/RSHRN do not exist
        return s.size != 3;
    case 0x12: case 0x13:
        return s.size != 3;
    case 0x14:
        return !scalar && s.size != 3;
    default:
        return false;
    }
}

bool shift_imm_accumulates(unsigned opc) {
    return opc == 0x02 || opc == 0x06 || opc == 0x08 || (opc == 0x0a);
}

// opc 0x0a: SHL (u = 0) does not accumulate; SLI (u = 1) does.
bool reads_destination(const ShiftImm& s) {
    if (s.opc == 0x0a) return s.u;
    return shift_imm_accumulates(s.opc);
}

void shift_imm_core(V& v, const ShiftImm& s, Xmm d, Xmm x, Xmm dold) {
    const unsigned esize = 8u << s.size;
    const unsigned rshift = 2 * esize - s.immhb;   // right shifts: 1..esize
    const unsigned lshift = s.immhb - esize;       // left shifts: 0..esize-1
    switch (s.opc) {
    case 0x00:
        if (s.u) v.srl(s.size, d, x, rshift); else v.sra(s.size, d, x, rshift);
        return;
    case 0x02: {
        const Xmm t = v.temp();
        if (s.u) v.srl(s.size, t, x, rshift); else v.sra(s.size, t, x, rshift);
        v.op(kAdd[s.size], d, dold, t, true);
        return;
    }
    case 0x04: rshr(v, s.size, !s.u, d, x, rshift); return;
    case 0x06: {
        const Xmm t = v.temp();
        rshr(v, s.size, !s.u, t, x, rshift);
        v.op(kAdd[s.size], d, dold, t, true);
        return;
    }
    case 0x08: {   // SRI
        if (rshift >= esize) { v.mov(d, dold); return; }
        const Xmm t = v.temp();
        v.srl(s.size, t, x, rshift);
        const std::uint64_t keep = ~(elem_mask(s.size) >> rshift) & elem_mask(s.size);
        const Xmm k = v.temp();
        v.op(op::pand, k, dold, splat(s.size, keep));
        v.por(d, k, t);
        return;
    }
    case 0x0a:
        if (!s.u) {
            v.shl(s.size, d, x, lshift);
        } else {   // SLI
            const Xmm t = v.temp();
            const Xmm k = v.temp();
            v.shl(s.size, t, x, lshift);
            const std::uint64_t keep = lshift == 0 ? 0 : (elem_mask(s.size) >> (esize - lshift));
            v.op(op::pand, k, dold, splat(s.size, keep));
            v.por(d, k, t);
        }
        return;
    case 0x0c: sat_shl(v, s.size, SatShl::signed_to_unsigned, d, x, lshift); return;
    case 0x0e: sat_shl(v, s.size, s.u ? SatShl::unsigned_ : SatShl::signed_, d, x, lshift); return;
    default: break;
    }
}

// Narrowing shifts (opc 0x10-0x13). `size` is the destination size; the
// source has 2x elements. Result in the low 64 bits, upper zero.
void shift_narrow(V& v, unsigned opc, bool u, unsigned size, unsigned immhb, Xmm d, Xmm x) {
    const unsigned esize = 8u << size;
    const unsigned n = 2 * esize - immhb;   // 1..esize
    const unsigned wide = size + 1;
    const bool round = (opc & 1) != 0;
    const Xmm t = v.temp();
    if (opc == 0x10 || opc == 0x11) {
        if (!u) {   // SHRN / RSHRN: truncating
            if (round) {
                v.op(kAdd[wide], t, x, splat(wide, 1ULL << (n - 1)));
                v.srl(wide, t, t, n);
            } else {
                v.srl(wide, t, x, n);
            }
            narrow(v, size, Narrow::truncate, d, t);
            return;
        }
        // SQSHRUN / SQRSHRUN
        if (round) rshr_exact(v, wide, true, t, x, n);
        else v.sra(wide, t, x, n);
        narrow(v, size, Narrow::sat_signed_to_unsigned, d, t);
        return;
    }
    // SQSHRN/UQSHRN (0x12), SQRSHRN/UQRSHRN (0x13)
    if (round) rshr_exact(v, wide, !u, t, x, n);
    else if (u) v.srl(wide, t, x, n);
    else v.sra(wide, t, x, n);
    narrow(v, size, u ? Narrow::sat_unsigned : Narrow::sat_signed, d, t);
}

} // namespace

unsigned emit_shift_imm(EmitContext& ctx, std::uint32_t w) {
    const bool q = bit(w, 30), u = bit(w, 29);
    const unsigned immh = bits(w, 22, 19), immb = bits(w, 18, 16), opc = bits(w, 15, 11);
    const unsigned rn = bits(w, 9, 5), rd = bits(w, 4, 0);
    if (immh == 0) return 0;
    const unsigned size = static_cast<unsigned>(std::bit_width(immh)) - 1;
    const ShiftImm s{opc, u, size, (immh << 3) | immb};
    if (!shift_imm_valid(s, false, q)) return 0;

    V v(ctx);
    if (opc >= 0x10 && opc <= 0x13) {
        const Xmm x = ctx.read_vec(rn);
        const Xmm dold = q ? ctx.read_vec(rd) : Xmm{};
        const Xmm d = ctx.write_vec(rd);
        if (q) {
            const Xmm t = v.temp();
            shift_narrow(v, opc, u, size, s.immhb, t, x);
            v.op(op::punpcklqdq, d, dold, t);
        } else {
            shift_narrow(v, opc, u, size, s.immhb, d, x);
        }
        return 1;
    }
    if (opc == 0x14) {   // SSHLL/USHLL (2)
        const unsigned esize = 8u << size;
        const unsigned lshift = s.immhb - esize;
        const Xmm x = ctx.read_vec(rn);
        const Xmm d = ctx.write_vec(rd);
        if (lshift == 0) {
            widen(v, size, !u, q, d, x);
        } else {
            const Xmm t = v.temp();
            widen(v, size, !u, q, t, x);
            v.shl(size + 1, d, t, lshift);
        }
        return 1;
    }
    const Xmm x = ctx.read_vec(rn);
    const Xmm dold = reads_destination(s) ? ctx.read_vec(rd) : Xmm{};
    // Every shift (and accumulate/insert) here maps zero lanes to zero.
    const bool upper_zero = !q && zero_above_64(ctx, rn) && (!reads_destination(s) || zero_above_64(ctx, rd));
    const Xmm d = ctx.write_vec(rd);
    shift_imm_core(v, s, d, x, dold);
    if (!upper_zero) v.finish(d, q);
    return 1;
}

unsigned emit_scalar_shift_imm(EmitContext& ctx, std::uint32_t w) {
    const bool u = bit(w, 29);
    const unsigned immh = bits(w, 22, 19), immb = bits(w, 18, 16), opc = bits(w, 15, 11);
    const unsigned rn = bits(w, 9, 5), rd = bits(w, 4, 0);
    if (immh == 0) return 0;
    const unsigned size = static_cast<unsigned>(std::bit_width(immh)) - 1;
    const ShiftImm s{opc, u, size, (immh << 3) | immb};
    if (!shift_imm_valid(s, true, true)) return 0;

    V v(ctx);
    const Xmm x = ctx.read_vec(rn);
    if (opc >= 0x10 && opc <= 0x13) {
        const Xmm d = ctx.write_vec(rd);
        const Xmm t = v.temp();
        shift_narrow(v, opc, u, size, s.immhb, t, x);
        v.keep_element0(size, d, t);
        return 1;
    }
    const Xmm dold = reads_destination(s) ? ctx.read_vec(rd) : Xmm{};
    const Xmm d = ctx.write_vec(rd);
    const Xmm t = v.temp();
    shift_imm_core(v, s, t, x, dold);
    v.keep_element0(size, d, t);
    return 1;
}

// ============================================================================
// Shifts by register
// ============================================================================

namespace {

enum class Dir { left, right_u, right_s };

// d = x shifted by the per-lane count c (0..esize inclusive; esize yields 0,
// or the sign fill for right_s). d must differ from c; t1/t2 are scratch.
void var_shift(V& v, unsigned size, Dir dir, Xmm d, Xmm x, Xmm c, Xmm t1, Xmm t2) {
    if (v.avx2 && size >= 2) {
        switch (dir) {
        case Dir::left: v.vex3(size == 2 ? vop::vpsllvd : vop::vpsllvq, d, x, c); return;
        case Dir::right_u: v.vex3(size == 2 ? vop::vpsrlvd : vop::vpsrlvq, d, x, c); return;
        case Dir::right_s:
            if (size == 2) {
                v.vex3(vop::vpsravd, d, x, c);
            } else {   // ((x ^ s) >>u c) ^ s
                v.sign_mask(3, t1, x);
                v.pxor(t2, x, t1);
                v.vex3(vop::vpsrlvq, t2, t2, c);
                v.pxor(d, t2, t1);
            }
            return;
        }
    }
    // Binary decomposition of the count: conditionally shift by 1, 2, 4, ...
    const unsigned esize = 8u << size;
    v.mov(d, x);
    for (unsigned amount = 1; amount <= esize; amount <<= 1) {
        v.op(op::pand, t2, c, splat(size, amount));
        v.op(kCmpEq[size], t2, t2, splat(size, amount));
        switch (dir) {
        case Dir::left: v.shl(size, t1, d, amount); break;
        case Dir::right_u: v.srl(size, t1, d, amount); break;
        case Dir::right_s: v.sra(size, t1, d, amount); break;
        }
        v.blendv(d, d, t1, t2);
    }
}

// Unsigned per-lane minimum with a small constant; counts are < 2^32 so the
// D form can use the dword minimum.
void min_count(V& v, unsigned size, Xmm d, Xmm x, unsigned limit) {
    if (size == 3) v.op(op::pminud, d, x, splat64(limit));
    else v.op(kMinU[size], d, x, splat(size, limit));
}

} // namespace

void emit_register_shift(EmitContext& ctx, unsigned size, unsigned lanes, RegShift kind,
                         unsigned rd, unsigned rn, unsigned rm, bool scalar) {
    V v(ctx);
    const Xmm x = ctx.read_vec(rn);
    const Xmm m = ctx.read_vec(rm);
    const Xmm d = ctx.write_vec(rd);
    const unsigned esize = 8u << size;
    const bool q = scalar || lanes * esize == 128;
    if (v.avx2 && size == 2 && !kind.saturating && !kind.rounding) {
        // USHL/SSHL on 32-bit lanes: variable shifts saturate their counts, so
        // USHL = (x << s) | (x >> -s); SSHL selects by the sign of s.
        const Xmm s = v.temp(), l = v.temp(), r = v.temp();
        v.pslld(s, m, 24);
        v.psrad(s, s, 24);
        if (neon_on(neon_old_shl)) {
            v.op(op::psignd, r, s, splat32(0xffffffffu));   // -s (psign by -1)
        } else {
            v.zero(r);
            v.op(op::psubd, r, r, s);
        }
        v.vex3(vop::vpsllvd, l, x, s);
        v.vex3(kind.is_unsigned ? vop::vpsrlvd : vop::vpsravd, r, x, r);
        const Xmm out = scalar ? v.temp() : d;
        if (kind.is_unsigned) v.por(out, l, r);
        else v.blendv_lane(2, out, l, r, s);
        if (scalar) v.keep_element0(2, d, out);
        else v.finish(d, q);
        return;
    }
    if (v.avx2 && size == 3 && !kind.saturating && !kind.rounding && neon_on(neon_old_shl)) {
        // USHL/SSHL on 64-bit lanes. s = the signed count bytes (pshufb
        // gathers bytes 0 and 8, pmovsxbq widens them). vpsllvq/vpsrlvq give
        // 0 for counts >= 64, negative counts included (they read as huge):
        // USHL = (x << s) | (x >>u -s); SSHL takes ((x ^ sx) >>u -s) ^ sx
        // (sx = sign fill of x; a count >= 64 leaves sx) where s < 0.
        static const Const128 kCountBytes =
            bytes_const({0, 8, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80});
        const Xmm s = v.temp(), z = v.temp(), r = v.temp(), l = v.temp();
        v.pshufb(s, m, kCountBytes);
        v.un(op::pmovsxbq, s, s);
        v.zero(z);
        v.op(op::psubq, r, z, s);
        const Xmm out = scalar ? z : d;
        if (kind.is_unsigned) {
            v.vex3(vop::vpsllvq, l, x, s);
            v.vex3(vop::vpsrlvq, r, x, r);
            v.por(out, l, r);
        } else {
            v.op(op::pcmpgtq, z, z, x);           // sx
            v.pxor(l, x, z);
            v.vex3(vop::vpsrlvq, l, l, r);
            v.pxor(r, l, z);                      // right result
            v.vex3(vop::vpsllvq, l, x, s);        // left result
            v.blendv_lane(3, out, l, r, s);
        }
        if (scalar) v.keep_element0(3, d, out);
        return;
    }
    // Fixed temp set: S count, N negative mask, K clamped count, L left
    // result, R right result, A/B shift scratch.
    const Xmm S = v.temp(), N = v.temp(), K = v.temp(), L = v.temp(), R = v.temp();
    const Xmm A = v.temp(), B = v.temp();

    // S = signed low byte of each count lane.
    switch (size) {
    case 0: v.mov(S, m); break;
    case 1: v.psllw(S, m, 8); v.psraw(S, S, 8); break;
    case 2: v.pslld(S, m, 24); v.psrad(S, S, 24); break;
    default: v.psllq(S, m, 56); v.sra(3, S, S, 56); break;
    }
    v.sign_mask(size, N, S);

    // Left: K = min(max(s, 0), esize).
    v.pandn(K, N, S);
    min_count(v, size, K, K, esize);
    var_shift(v, size, Dir::left, L, x, K, A, B);
    if (kind.saturating) {
        var_shift(v, size, kind.is_unsigned ? Dir::right_u : Dir::right_s, R, L, K, A, B);
        v.op(kCmpEq[size], R, R, x, true);   // R = no overflow
        if (kind.is_unsigned) {
            v.op(op::pandn, R, R, v.k(splat8(0xff)));
            v.por(L, L, R);
        } else {
            v.sign_mask(size, A, x);
            v.op(op::pxor, A, A, splat(size, smax(size)));
            v.blendv(L, A, L, R);
        }
    }

    // Right: K = -s (truncating) or -s - 1 = ~s (rounding), clamped.
    if (kind.rounding) {
        v.not_(K, S);
    } else {
        v.zero(K);
        v.op(kSub[size], K, K, S);
    }
    v.pand(K, K, N);
    min_count(v, size, K, K, esize);
    var_shift(v, size, kind.is_unsigned ? Dir::right_u : Dir::right_s, R, x, K, A, B);
    if (kind.rounding) {   // R = t - (t >> 1)
        if (kind.is_unsigned) v.srl(size, A, R, 1); else v.sra(size, A, R, 1);
        v.op(kSub[size], R, R, A);
    }

    if (scalar) {
        v.blendv(A, L, R, N);
        v.keep_element0(size, d, A);
    } else {
        v.blendv(d, L, R, N);
        v.finish(d, q);
    }
}

} // namespace transmute::fx::neon
