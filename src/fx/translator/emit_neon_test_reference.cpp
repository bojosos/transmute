// Hand-written C++ references for instructions the reference interpreter
// (src/a64.cpp) does not implement (owner: neon). Used by check() when the
// oracle declines a word that the NEON family emits.
//
// Covered: SQDMULL/SQDMLAL/SQDMLSL (vector, by element, scalar, scalar by
// element), scalar SSHL/USHL/SRSHL/URSHL, scalar SUB, SUQADD/USQADD (vector and
// scalar), scalar SQNEG/SQABS, PMUL, PMULL (8-bit), and DUP (general) with
// don't-care imm5 bits.
#include "fx/translator/emit_neon_test_util.hpp"

#include <algorithm>
#include <cstring>

namespace transmute::fx::neon_test {

namespace {

constexpr unsigned field(std::uint32_t w, unsigned hi, unsigned lo) {
    return (w >> lo) & ((1u << (hi - lo + 1)) - 1u);
}

std::uint64_t lane(const GuestContext& c, unsigned reg, unsigned size, unsigned index) {
    std::uint64_t v = 0;
    std::memcpy(&v, reinterpret_cast<const std::uint8_t*>(c.v[reg]) + (std::size_t{index} << size), std::size_t{1} << size);
    return v;
}

void set_lane(std::uint64_t (&out)[2], unsigned size, unsigned index, std::uint64_t value) {
    std::memcpy(reinterpret_cast<std::uint8_t*>(out) + (std::size_t{index} << size), &value, std::size_t{1} << size);
}

std::int64_t sext(std::uint64_t v, unsigned bits) {
    if (bits >= 64) return static_cast<std::int64_t>(v);
    const unsigned s = 64 - bits;
    return static_cast<std::int64_t>(v << s) >> s;
}

std::int64_t sat_signed(std::int64_t v, unsigned bits) {
    if (bits >= 64) return v;
    const std::int64_t hi = (1LL << (bits - 1)) - 1, lo = -(1LL << (bits - 1));
    return std::clamp(v, lo, hi);
}

// Signed saturating 64-bit add/sub.
std::int64_t sat_add64(std::int64_t a, std::int64_t b) {
    const auto r = static_cast<std::int64_t>(static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b));
    if (((a ^ r) & (b ^ r)) < 0) return a < 0 ? INT64_MIN : INT64_MAX;
    return r;
}
std::int64_t sat_sub64(std::int64_t a, std::int64_t b) {
    const auto r = static_cast<std::int64_t>(static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b));
    if (((a ^ b) & (a ^ r)) < 0) return a < 0 ? INT64_MIN : INT64_MAX;
    return r;
}

// 2*a*b saturated to 2*esize bits (esize = 16 or 32).
std::int64_t sqdmull_element(std::int64_t a, std::int64_t b, unsigned esize) {
    const std::int64_t product = a * b;
    if (esize == 32 && product == (1LL << 62)) return INT64_MAX;
    return sat_signed(product * 2, 2 * esize);
}

std::int64_t sat_accumulate(std::int64_t acc, std::int64_t value, bool subtract, unsigned bits) {
    if (bits == 64) return subtract ? sat_sub64(acc, value) : sat_add64(acc, value);
    return sat_signed(subtract ? acc - value : acc + value, bits);
}

// opc: 0x9 SQDMLAL, 0xb SQDMLSL, 0xd SQDMULL. `b_index` < 0: element-wise.
void sqdml(GuestContext& c, unsigned opc, unsigned size, bool upper, bool scalar, unsigned rd, unsigned rn,
           unsigned rm, int b_index) {
    const unsigned esize = 8u << size, wide = size + 1;
    const unsigned lanes = scalar ? 1 : 64 / esize;
    std::uint64_t out[2] = {0, 0};
    for (unsigned i = 0; i < lanes; ++i) {
        const unsigned src = i + (upper ? lanes : 0);
        const std::int64_t a = sext(lane(c, rn, size, src), esize);
        const std::int64_t b = sext(lane(c, rm, size, b_index < 0 ? src : static_cast<unsigned>(b_index)), esize);
        std::int64_t r = sqdmull_element(a, b, esize);
        if (opc != 0xd) {
            const std::int64_t acc = sext(lane(c, rd, wide, i), 2 * esize);
            r = sat_accumulate(acc, r, opc == 0xb, 2 * esize);
        }
        set_lane(out, wide, i, static_cast<std::uint64_t>(r));
    }
    c.v[rd][0] = out[0];
    c.v[rd][1] = out[1];
}

std::uint64_t shift_reg64(std::uint64_t x, std::int64_t s, bool is_unsigned, bool rounding) {
    if (s >= 0) return s >= 64 ? 0 : x << s;
    const std::int64_t k = -s;
    if (is_unsigned) {
        const std::uint64_t hi = k >= 64 ? 0 : x >> k;
        const std::uint64_t rb = !rounding || k - 1 >= 64 ? 0 : (x >> (k - 1)) & 1;
        return hi + rb;
    }
    const auto sx = static_cast<std::int64_t>(x);
    const std::int64_t hi = sx >> std::min<std::int64_t>(k, 63);
    const std::int64_t rb = !rounding ? 0 : (sx >> std::min<std::int64_t>(k - 1, 63)) & 1;
    return static_cast<std::uint64_t>(hi + rb);
}

std::uint64_t suqadd_element(std::uint64_t acc_raw, std::uint64_t x, unsigned esize) {
    const std::uint64_t mask = esize == 64 ? ~0ULL : (1ULL << esize) - 1;
    if (esize < 64) {
        const std::int64_t r = sext(acc_raw, esize) + static_cast<std::int64_t>(x & mask);
        return static_cast<std::uint64_t>(sat_signed(r, esize)) & mask;
    }
    const auto acc = static_cast<std::int64_t>(acc_raw);
    const std::uint64_t limit = static_cast<std::uint64_t>(INT64_MAX) - static_cast<std::uint64_t>(acc);
    return x > limit ? static_cast<std::uint64_t>(INT64_MAX) : acc_raw + x;
}

std::uint64_t usqadd_element(std::uint64_t acc, std::uint64_t x_raw, unsigned esize) {
    const std::uint64_t mask = esize == 64 ? ~0ULL : (1ULL << esize) - 1;
    const std::int64_t x = sext(x_raw, esize);
    if (esize < 64) {
        const std::int64_t r = static_cast<std::int64_t>(acc & mask) + x;
        return static_cast<std::uint64_t>(std::clamp<std::int64_t>(r, 0, static_cast<std::int64_t>(mask)));
    }
    if (x >= 0) {
        const std::uint64_t r = acc + static_cast<std::uint64_t>(x);
        return r < acc ? ~0ULL : r;
    }
    const std::uint64_t magnitude = 0 - static_cast<std::uint64_t>(x);
    return magnitude > acc ? 0 : acc - magnitude;
}

std::uint64_t pmul8(std::uint64_t a, std::uint64_t b) {
    std::uint64_t r = 0;
    for (unsigned i = 0; i < 8; ++i)
        if (b & (1ULL << i)) r ^= a << i;
    return r;
}

// SQRDMLAH/SQRDMLSH element: SignedSatQ(((acc << e) +- 2ab + 2^(e-1)) >> e).
// (acc << e) is a multiple of 2^e, so the shift distributes exactly.
std::int64_t rdm_element(std::int64_t acc, std::int64_t a, std::int64_t b, bool subtract, unsigned e) {
    const std::int64_t product = subtract ? -(a * b) : a * b;
    const std::int64_t h = (product + (1LL << (e - 2))) >> (e - 1);
    return sat_signed(acc + h, e);
}

// Arm UnsignedRecipEstimate / UnsignedRSqrtEstimate (32-bit).
std::uint32_t urecpe(std::uint32_t x) {
    if (!(x >> 31)) return 0xffffffffu;
    int a = static_cast<int>(x >> 23);
    a = a * 2 + 1;
    const int b = (1 << 19) / a;
    const int r = (b + 1) / 2;
    return static_cast<std::uint32_t>(r) << 23;
}

std::uint32_t ursqrte(std::uint32_t x) {
    if ((x >> 30) == 0) return 0xffffffffu;
    std::int64_t a = x >> 23;
    if (a < 256) a = a * 2 + 1;
    else a = ((a >> 1) << 1) + 1, a *= 2;
    std::int64_t b = 512;
    while (a * (b + 1) * (b + 1) < (1LL << 28)) ++b;
    const std::int64_t r = (b + 1) / 2;
    return static_cast<std::uint32_t>(r) << 23;
}

} // namespace

bool reference_step(GuestContext& c, std::uint32_t w) {
    const unsigned rd = field(w, 4, 0), rn = field(w, 9, 5), rm = field(w, 20, 16);
    const unsigned size = field(w, 23, 22);
    const bool q = field(w, 30, 30) != 0, u = field(w, 29, 29) != 0;

    // Three-register extension: SQRDMLAH/SQRDMLSH, SMMLA/UMMLA/USMMLA.
    if ((w & 0x9f208400u) == 0x0e008400u || (w & 0xdf208400u) == 0x5e008400u) {
        const bool scalar = (w & 0x10000000u) != 0;
        const unsigned opc = field(w, 14, 11);
        if (u && (opc == 0x0 || opc == 0x1) && (size == 1 || size == 2)) {
            const unsigned e = 8u << size, lanes = scalar ? 1 : (q ? 16u : 8u) >> size;
            std::uint64_t out[2] = {0, 0};
            for (unsigned i = 0; i < lanes; ++i) {
                const auto r = rdm_element(sext(lane(c, rd, size, i), e), sext(lane(c, rn, size, i), e),
                                           sext(lane(c, rm, size, i), e), opc == 0x1, e);
                set_lane(out, size, i, static_cast<std::uint64_t>(r));
            }
            c.v[rd][0] = out[0];
            c.v[rd][1] = out[1];
            return true;
        }
        if (!scalar && q && size == 2 && (opc == 0x4 || (opc == 0x5 && !u))) {
            const bool nu = opc == 0x5 || u, mu = opc == 0x4 && u;
            std::uint64_t out[2] = {0, 0};
            for (unsigned i = 0; i < 2; ++i)
                for (unsigned j = 0; j < 2; ++j) {
                    std::int64_t sum = static_cast<std::int64_t>(lane(c, rd, 2, 2 * i + j));
                    for (unsigned k = 0; k < 8; ++k) {
                        const auto a = lane(c, rn, 0, 8 * i + k), b = lane(c, rm, 0, 8 * j + k);
                        sum += (nu ? static_cast<std::int64_t>(a) : sext(a, 8)) *
                               (mu ? static_cast<std::int64_t>(b) : sext(b, 8));
                    }
                    set_lane(out, 2, 2 * i + j, static_cast<std::uint64_t>(sum));
                }
            c.v[rd][0] = out[0];
            c.v[rd][1] = out[1];
            return true;
        }
        return false;
    }
    // Vector three different: SQDMLAL/SQDMLSL/SQDMULL.
    if ((w & 0x9f200c00u) == 0x0e200000u) {
        const unsigned opc = field(w, 15, 12);
        if (!u && (opc == 0x9 || opc == 0xb || opc == 0xd) && (size == 1 || size == 2)) {
            sqdml(c, opc, size, q, false, rd, rn, rm, -1);
            return true;
        }
        if (!u && opc == 0xe && size == 3) {   // PMULL/PMULL2 (64 x 64 -> 128)
            const std::uint64_t a = c.v[rn][q ? 1 : 0], b = c.v[rm][q ? 1 : 0];
            std::uint64_t lo = 0, hi = 0;
            for (unsigned i = 0; i < 64; ++i) {
                if (!((b >> i) & 1)) continue;
                lo ^= a << i;
                if (i) hi ^= a >> (64 - i);
            }
            c.v[rd][0] = lo;
            c.v[rd][1] = hi;
            return true;
        }
        if (!u && opc == 0xe && size == 0) {   // PMULL/PMULL2 (8-bit)
            std::uint64_t out[2] = {0, 0};
            for (unsigned i = 0; i < 8; ++i)
                set_lane(out, 1, i, pmul8(lane(c, rn, 0, i + (q ? 8 : 0)), lane(c, rm, 0, i + (q ? 8 : 0))));
            c.v[rd][0] = out[0];
            c.v[rd][1] = out[1];
            return true;
        }
        return false;
    }
    // Scalar three different.
    if ((w & 0xdf200c00u) == 0x5e200000u) {
        const unsigned opc = field(w, 15, 12);
        if (!u && (opc == 0x9 || opc == 0xb || opc == 0xd) && (size == 1 || size == 2)) {
            sqdml(c, opc, size, false, true, rd, rn, rm, -1);
            return true;
        }
        return false;
    }
    // By element (vector and scalar): SQDMLAL/SQDMLSL/SQDMULL.
    if ((w & 0x9f000400u) == 0x0f000000u || (w & 0xdf000400u) == 0x5f000000u) {
        const bool scalar = (w & 0x10000000u) != 0;
        const unsigned opc = field(w, 15, 12);
        const unsigned h = field(w, 11, 11), l = field(w, 21, 21), mbit = field(w, 20, 20);
        const unsigned index = size == 1 ? (h << 2) | (l << 1) | mbit : (h << 1) | l;
        const unsigned reg_m = size == 1 ? field(w, 19, 16) : rm;
        if (u && (opc == 0xd || opc == 0xf) && (size == 1 || size == 2)) {   // SQRDMLAH/SQRDMLSH
            const unsigned e = 8u << size, lanes = scalar ? 1 : (q ? 16u : 8u) >> size;
            const std::int64_t b = sext(lane(c, reg_m, size, index), e);
            std::uint64_t out[2] = {0, 0};
            for (unsigned i = 0; i < lanes; ++i) {
                const auto r = rdm_element(sext(lane(c, rd, size, i), e), sext(lane(c, rn, size, i), e), b,
                                           opc == 0xf, e);
                set_lane(out, size, i, static_cast<std::uint64_t>(r));
            }
            c.v[rd][0] = out[0];
            c.v[rd][1] = out[1];
            return true;
        }
        if (u || (opc != 0x3 && opc != 0x7 && opc != 0xb) || (size != 1 && size != 2)) return false;
        const unsigned mapped = opc == 0x3 ? 0x9 : opc == 0x7 ? 0xb : 0xd;
        sqdml(c, mapped, size, !scalar && q, scalar, rd, rn, reg_m, static_cast<int>(index));
        return true;
    }
    // Scalar three same: SUB D, SSHL/USHL/SRSHL/URSHL D.
    if ((w & 0xdf200400u) == 0x5e200400u) {
        const unsigned opc = field(w, 15, 11);
        if (size != 3) return false;
        const std::uint64_t x = c.v[rn][0], m = c.v[rm][0];
        if (opc == 0x10 && u) {
            c.v[rd][0] = x - m;
            c.v[rd][1] = 0;
            return true;
        }
        if (opc == 0x08 || opc == 0x0a) {
            const std::int64_t s = static_cast<std::int8_t>(m & 0xff);
            c.v[rd][0] = shift_reg64(x, s, u, opc == 0x0a);
            c.v[rd][1] = 0;
            return true;
        }
        return false;
    }
    // Two-register misc (vector and scalar): SUQADD, USQADD, SQNEG, SQABS.
    if ((w & 0x9f3e0c00u) == 0x0e200800u || (w & 0xdf3e0c00u) == 0x5e200800u) {
        const bool scalar = (w & 0x10000000u) != 0;
        const unsigned opc = field(w, 16, 12);
        const unsigned esize = 8u << size;
        const unsigned lanes = scalar ? 1 : (q ? 16u : 8u) >> size;
        if (!scalar && opc == 0x1c && size == 2) {   // URECPE / URSQRTE
            std::uint64_t out[2] = {0, 0};
            for (unsigned i = 0; i < (q ? 4u : 2u); ++i) {
                const auto x = static_cast<std::uint32_t>(lane(c, rn, 2, i));
                set_lane(out, 2, i, u ? ursqrte(x) : urecpe(x));
            }
            c.v[rd][0] = out[0];
            c.v[rd][1] = out[1];
            return true;
        }
        if (!scalar && opc == 0x05 && u && size == 1) {   // RBIT (vector)
            std::uint64_t out[2] = {0, 0};
            for (unsigned i = 0; i < (q ? 16u : 8u); ++i) {
                const auto b = static_cast<unsigned>(lane(c, rn, 0, i));
                unsigned r = 0;
                for (unsigned bit_ = 0; bit_ < 8; ++bit_)
                    if (b & (1u << bit_)) r |= 0x80u >> bit_;
                set_lane(out, 0, i, r);
            }
            c.v[rd][0] = out[0];
            c.v[rd][1] = out[1];
            return true;
        }
        if (scalar && size == 3 && (opc == 0x08 || opc == 0x09 || opc == 0x0a || opc == 0x0b)) {
            const auto x = static_cast<std::int64_t>(c.v[rn][0]);
            std::uint64_t r = 0;
            switch (opc) {
            case 0x08: r = (u ? x >= 0 : x > 0) ? ~0ULL : 0; break;
            case 0x09: r = (u ? x <= 0 : x == 0) ? ~0ULL : 0; break;
            case 0x0a: r = x < 0 ? ~0ULL : 0; break;
            default:
                r = u ? 0 - static_cast<std::uint64_t>(x)
                      : (x < 0 ? 0 - static_cast<std::uint64_t>(x) : static_cast<std::uint64_t>(x));
                break;
            }
            c.v[rd][0] = r;
            c.v[rd][1] = 0;
            return true;
        }
        if (scalar && (opc == 0x12 || opc == 0x14) && size < 3) {   // SQXTUN, SQXTN, UQXTN
            const unsigned dbits = 8u << size, sbits = 2 * dbits;
            const std::uint64_t raw = lane(c, rn, size + 1, 0);
            std::uint64_t r;
            if (opc == 0x12 || !u) {
                const std::int64_t v = sext(raw, sbits);
                if (opc == 0x12) r = static_cast<std::uint64_t>(std::clamp<std::int64_t>(v, 0, (1LL << dbits) - 1));
                else r = static_cast<std::uint64_t>(sat_signed(v, dbits)) & ((1ULL << dbits) - 1);
            } else {
                r = std::min<std::uint64_t>(raw, (1ULL << dbits) - 1);
            }
            c.v[rd][0] = r;
            c.v[rd][1] = 0;
            return true;
        }
        if (opc != 0x03 && opc != 0x07) return false;
        std::uint64_t out[2] = {0, 0};
        for (unsigned i = 0; i < lanes; ++i) {
            const std::uint64_t x = lane(c, rn, size, i);
            std::uint64_t r;
            if (opc == 0x03) {
                const std::uint64_t acc = lane(c, rd, size, i);
                r = u ? usqadd_element(acc, x, esize) : suqadd_element(acc, x, esize);
            } else {
                const std::int64_t v = sext(x, esize);
                if (esize == 64) {
                    if (u) r = v == INT64_MIN ? INT64_MAX : static_cast<std::uint64_t>(-v);
                    else r = v == INT64_MIN ? INT64_MAX : static_cast<std::uint64_t>(v < 0 ? -v : v);
                } else {
                    r = static_cast<std::uint64_t>(sat_signed(u ? -v : (v < 0 ? -v : v), esize));
                }
            }
            set_lane(out, size, i, r);
        }
        c.v[rd][0] = out[0];
        c.v[rd][1] = out[1];
        return true;
    }
    // Three same: PMUL.
    if ((w & 0x9f200400u) == 0x0e200400u) {
        if (field(w, 15, 11) != 0x13 || !u || size != 0) return false;
        std::uint64_t out[2] = {0, 0};
        for (unsigned i = 0; i < (q ? 16u : 8u); ++i) set_lane(out, 0, i, pmul8(lane(c, rn, 0, i), lane(c, rm, 0, i)));
        c.v[rd][0] = out[0];
        c.v[rd][1] = out[1];
        return true;
    }
    // DUP (general) with don't-care imm5 bits: canonicalize and use the oracle.
    if ((w & 0xbfe0fc00u) == 0x0e000c00u) {
        const unsigned imm5 = field(w, 20, 16);
        if (imm5 == 0) return false;
        const unsigned canonical = imm5 & (0u - imm5);
        if (canonical == imm5) return false;
        GuestContext copy = c;
        if (!testing::oracle_step(copy, (w & ~0x001f0000u) | (canonical << 16))) return false;
        copy.pc = c.pc;
        c = copy;
        return true;
    }
    return false;
}

} // namespace transmute::fx::neon_test
