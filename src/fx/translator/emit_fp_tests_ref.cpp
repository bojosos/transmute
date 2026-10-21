// fp family tests: references for instructions the reference interpreter
// lacks, built from oracle primitives it does implement where possible
// (owner: fp).
#include "fx/translator/emit_fp_tests.hpp"

#include <cstring>

namespace transmute::fx::fptest {

namespace {

std::uint64_t lane(const GuestContext& c, unsigned r, unsigned es, unsigned i) {
    const unsigned per = 64 / es;
    const std::uint64_t half = c.v[r][i / per];
    return es == 64 ? half : (half >> ((i % per) * es)) & ((1ULL << es) - 1);
}

void set_lane(GuestContext& c, unsigned r, unsigned es, unsigned i, std::uint64_t value) {
    const unsigned per = 64 / es;
    auto& half = c.v[r][i / per];
    if (es == 64) { half = value; return; }
    const unsigned shift = (i % per) * es;
    const std::uint64_t mask = ((1ULL << es) - 1) << shift;
    half = (half & ~mask) | ((value << shift) & mask);
}

unsigned ebits(unsigned es) { return es == 64 ? 11 : es == 32 ? 8 : 5; }
bool is_zero(std::uint64_t v, unsigned es, bool fz) {
    const std::uint64_t mag = v & ((1ULL << (es - 1)) - 1);
    if (mag == 0) return true;
    return fz && es != 16 && (mag >> (es - 1 - ebits(es))) == 0;   // flushed denormal
}
bool is_inf(std::uint64_t v, unsigned es) {
    const unsigned f = es - 1 - ebits(es);
    return (v & ((1ULL << (es - 1)) - 1)) == (((1ULL << ebits(es)) - 1) << f);
}
std::uint64_t two(unsigned es, bool negative) {
    const std::uint64_t t = es == 64 ? 0x4000000000000000ULL : es == 32 ? 0x40000000ULL : 0x4000ULL;
    return negative ? t | (1ULL << (es - 1)) : t;
}

bool oracle(GuestContext& c, std::uint32_t word) {
    const auto pc = c.pc;
    const bool ok = testing::oracle_step(c, word);
    c.pc = pc;
    return ok;
}

} // namespace

// SIMD scalar instruction through its 128-bit vector form: lane 0 kept.
bool ref_scalar_via_vector(std::uint32_t word, GuestContext& c, unsigned es) {
    const std::uint32_t vector_word = word & ~0x10000000u;
    GuestContext t = c;
    if (!oracle(t, vector_word)) return false;
    const unsigned rd = word & 31;
    const std::uint64_t r = lane(t, rd, es, 0);
    c.v[rd][0] = r;
    c.v[rd][1] = 0;
    c.fpsr = t.fpsr;
    return true;
}

// Vector FADDP/FMAXP/FMINP/FMAXNMP/FMINNMP: the non-pairwise vector op on
// de-interleaved operands.
bool ref_pairwise_vector(std::uint32_t word, GuestContext& c) {
    const bool q = (word >> 30) & 1;
    const unsigned es = ((word >> 22) & 1) ? 64 : 32;
    const unsigned rd = word & 31, rn = (word >> 5) & 31, rm = (word >> 16) & 31;
    const unsigned n = (q ? 128 : 64) / es;
    GuestContext t = c;
    t.v[0][0] = t.v[0][1] = t.v[1][0] = t.v[1][1] = 0;
    for (unsigned i = 0; i < n; ++i) {
        const unsigned src = i < n / 2 ? rn : rm;
        const unsigned j = (i % (n / 2)) * 2;
        set_lane(t, 0, es, i, lane(c, src, es, j));
        set_lane(t, 1, es, i, lane(c, src, es, j + 1));
    }
    // U=1 pairwise -> U=0 plain op (FADDP -> FADD).
    const std::uint32_t op_word = (word & ~0x2000001fu & ~(31u << 5) & ~(31u << 16)) | 2u | (0u << 5) | (1u << 16);
    if (!oracle(t, op_word)) return false;
    c.v[rd][0] = t.v[2][0];
    c.v[rd][1] = q ? t.v[2][1] : 0;
    c.fpsr = t.fpsr;
    return true;
}

// Scalar FADDP/FMAXP/... Vd = op(Vn[0], Vn[1]) via the scalar FP op.
bool ref_pairwise_scalar(std::uint32_t word, GuestContext& c) {
    const unsigned es = ((word >> 22) & 1) ? 64 : 32;
    const unsigned rd = word & 31, rn = (word >> 5) & 31;
    const unsigned opcode = (word >> 12) & 31;
    const bool a = (word >> 23) & 1;
    unsigned fp_opcode;   // scalar FP 2-source opcode
    if (opcode == 0x0d) fp_opcode = 2;                 // FADD
    else if (opcode == 0x0f) fp_opcode = a ? 5 : 4;    // FMIN / FMAX
    else fp_opcode = a ? 7 : 6;                        // FMINNM / FMAXNM
    GuestContext t = c;
    t.v[0][0] = lane(c, rn, es, 0);
    t.v[0][1] = 0;
    t.v[1][0] = lane(c, rn, es, 1);
    t.v[1][1] = 0;
    if (!oracle(t, enc::fp2(fp_opcode, es == 64 ? 1 : 0, 2, 0, 1))) return false;
    c.v[rd][0] = lane(t, 2, es, 0);
    c.v[rd][1] = 0;
    c.fpsr = t.fpsr;
    return true;
}

// FMULX (vector, scalar, by element): FMUL with 0 * inf -> +-2.0.
bool ref_fmulx(std::uint32_t word, GuestContext& c) {
    const unsigned rd = word & 31, rn = (word >> 5) & 31;
    const bool fz = (c.fpcr >> 24) & 1;
    unsigned es, n;
    std::uint32_t mul;
    int index = -1;
    unsigned rm = (word >> 16) & 31;
    bool scalar;
    if ((word & 0x9f200400u) == 0x0e200400u || (word & 0xdf200400u) == 0x5e200400u) {   // three same
        scalar = (word >> 28) & 1;
        es = ((word >> 22) & 1) ? 64 : 32;
        if (scalar) {
            mul = enc::fp2(0, es == 64 ? 1 : 0, rd, rn, rm);
        } else {
            mul = word | (1u << 29);
        }
    } else {   // by element (U=1 -> U=0)
        scalar = (word >> 28) & 1;
        es = ((word >> 22) & 1) ? 64 : 32;
        const unsigned h = (word >> 11) & 1, l = (word >> 21) & 1;
        index = es == 64 ? static_cast<int>(h) : static_cast<int>((h << 1) | l);
        mul = word & ~(1u << 29);
        if (scalar) mul &= ~0x10000000u, mul |= 0x40000000u;   // via the vector form
    }
    const bool q = scalar || ((word >> 30) & 1);
    n = scalar ? 1 : (q ? 128 : 64) / es;
    GuestContext t = c;
    if (!oracle(t, mul)) return false;
    for (unsigned i = 0; i < n; ++i) {
        const std::uint64_t x = lane(c, rn, es, i);
        const std::uint64_t y = lane(c, rm, es, index >= 0 ? static_cast<unsigned>(index) : i);
        if ((is_zero(x, es, fz) && is_inf(y, es)) || (is_inf(x, es) && is_zero(y, es, fz))) {
            const bool neg = (((x ^ y) >> (es - 1)) & 1) != 0;
            set_lane(t, rd, es, i, two(es, neg));
        }
    }
    c.v[rd][0] = t.v[rd][0];
    c.v[rd][1] = scalar ? 0 : t.v[rd][1];
    if (scalar && es == 32) c.v[rd][0] &= 0xffffffffULL;
    c.fpsr = t.fpsr;
    return true;
}

// FCVTXN/FCVTXN2 (vector) and FCVTXN Sd, Dn: round to odd.
bool ref_fcvtxn(std::uint32_t word, GuestContext& c) {
    const unsigned rd = word & 31, rn = (word >> 5) & 31;
    const bool scalar = (word >> 28) & 1;
    const bool upper = !scalar && ((word >> 30) & 1);
    const bool fz = (c.fpcr >> 24) & 1;
    std::uint32_t out[2] = {};
    const unsigned count = scalar ? 1 : 2;
    for (unsigned i = 0; i < count; ++i) {
        const std::uint64_t d = c.v[rn][i];
        GuestContext t = c;
        t.fpcr = (c.fpcr & ~(3u << 22)) | (3u << 22);   // RZ
        t.v[0][0] = d;
        t.v[0][1] = 0;
        if (!oracle(t, enc::fp1(4, 1, 1, 0))) return false;   // FCVT S1, D0
        auto r = static_cast<std::uint32_t>(t.v[1][0]);
        if (!is_nan(d, 64)) {
            if (!oracle(t, enc::fp1(5, 0, 2, 1))) return false;   // FCVT D2, S1
            const bool flushed = fz && (r & 0x7fffffffu) == 0 && (d & 0x7fffffffffffffffULL) != 0;
            if (t.v[2][0] != d && !flushed) r |= 1;
        }
        out[i] = r;
    }
    if (scalar) {
        c.v[rd][0] = out[0];
        c.v[rd][1] = 0;
    } else {
        const std::uint64_t packed = out[0] | (static_cast<std::uint64_t>(out[1]) << 32);
        if (upper) c.v[rd][1] = packed;
        else { c.v[rd][0] = packed; c.v[rd][1] = 0; }
    }
    return true;
}

// FRECPX (scalar).
bool ref_frecpx(std::uint32_t word, GuestContext& c) {
    const unsigned rd = word & 31, rn = (word >> 5) & 31;
    const unsigned es = ((word >> 22) & 1) ? 64 : 32;
    const unsigned e = ebits(es), f = es - e - 1;
    const std::uint64_t x = lane(c, rn, es, 0);
    std::uint64_t r;
    if (is_nan(x, es)) {
        r = x | (1ULL << (f - 1));
    } else {
        const std::uint64_t sign = x & (1ULL << (es - 1));
        const std::uint64_t exp = (x >> f) & ((1ULL << e) - 1);
        const std::uint64_t emax = (1ULL << e) - 1;
        r = sign | ((exp == 0 ? emax - 1 : (~exp & emax)) << f);
    }
    c.v[rd][0] = r;
    c.v[rd][1] = 0;
    return true;
}

} // namespace transmute::fx::fptest

namespace transmute::fx::fptest {

// Scalar FP two-source (FMAX/FMIN/FMAXNM/FMINNM single precision): the
// reference interpreter does not mask its 64-bit source to 32 bits, which
// breaks the +-0 rule and the upper-bit zeroing. Run it on sources with bits
// 63:32 cleared (they do not affect the architectural result).
bool ref_scalar_minmax(std::uint32_t w, GuestContext& c) {
    const unsigned rd = w & 31, rn = (w >> 5) & 31, rm = (w >> 16) & 31;
    const bool single = ((w >> 22) & 3) == 0;
    GuestContext t = c;
    if (single) {
        t.v[rn][0] &= 0xffffffffULL;
        t.v[rm][0] &= 0xffffffffULL;
    }
    if (!testing::oracle_step(t, w)) return false;
    c.v[rd][0] = single ? (t.v[rd][0] & 0xffffffffULL) : t.v[rd][0];
    c.v[rd][1] = 0;
    c.fpsr = t.fpsr;
    return true;
}

// Reference for words the reference interpreter lacks (independent bit
// pattern decode).
bool ref_missing(std::uint32_t w, GuestContext& c) {
    if ((w & 0xbfa0fc00u) == 0x0e20dc00u || (w & 0xffa0fc00u) == 0x5e20dc00u ||
        (w & 0xbf00f400u) == 0x2f009000u || (w & 0xff00f400u) == 0x7f009000u)
        return ref_fmulx(w, c);
    const std::uint32_t pair = w & 0xbf20fc00u;
    if (pair == 0x2e20c400u || pair == 0x2e20f400u || (w & 0xbfa0fc00u) == 0x2e20d400u)
        return ref_pairwise_vector(w, c);
    if ((w & 0xff3e0c00u) == 0x7e300800u) return ref_pairwise_scalar(w, c);
    if ((w & 0xbffffc00u) == 0x2e616800u || (w & 0xfffffc00u) == 0x7e616800u) return ref_fcvtxn(w, c);
    if ((w & 0xffbffc00u) == 0x5ea1f800u) return ref_frecpx(w, c);
    if ((w & 0xd0000000u) == 0x50000000u) {   // other AdvSIMD scalar
        const unsigned es = ((w >> 22) & 1) ? 64 : 32;
        if ((w & 0xdf800400u) == 0x5f000400u) {   // shift by immediate: size from immh
            const unsigned immh = (w >> 19) & 15;
            return ref_scalar_via_vector(w, c, (immh & 8) ? 64 : 32);
        }
        return ref_scalar_via_vector(w, c, es);
    }
    return false;
}

} // namespace transmute::fx::fptest

#include <cmath>

namespace transmute::fx::fptest {

// FJCVTZS Wd, Dn (A64 FPToFixedJS), missing from the reference interpreter.
bool ref_fjcvtzs(std::uint32_t w, GuestContext& c) {
    const unsigned rd = w & 31, rn = (w >> 5) & 31;
    const std::uint64_t x = c.v[rn][0];
    const bool fz = (c.fpcr >> 24) & 1;
    const unsigned exp = static_cast<unsigned>((x >> 52) & 0x7ff);
    const std::uint64_t frac = x & ((1ULL << 52) - 1);
    const bool sign = (x >> 63) != 0;
    std::uint32_t result = 0;
    bool z = true;
    if (exp == 0x7ff) {
        z = false;
    } else if (exp == 0 && (frac == 0 || fz)) {
        z = !sign && frac == 0;
    } else {
        double v;
        std::memcpy(&v, &x, 8);
        const double t = std::trunc(v);
        if (t != v) z = false;
        if (t < -2147483648.0 || t > 2147483647.0) z = false;
        const double r = std::fmod(t, 4294967296.0);
        result = static_cast<std::uint32_t>(static_cast<std::int64_t>(r));
    }
    if (rd != 31) c.x[rd] = result;
    c.nzcv = z ? 0x40000000u : 0u;
    return true;
}

} // namespace transmute::fx::fptest
