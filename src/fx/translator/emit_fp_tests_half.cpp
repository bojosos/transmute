// fp family tests: half precision (FEAT_FP16) against a hand-written
// reference (the reference interpreter has no FP16 arithmetic) (owner: fp).
#include "fx/translator/emit_fp_internal.hpp"
#include "fx/translator/emit_fp_tests.hpp"

#include <immintrin.h>

#include <cmath>
#include <cstdio>
#include <cstring>

using namespace transmute::fx;
using namespace transmute::fx::fptest;

namespace {

using fpx::Inst;
using fpx::K;
using fpx::Rnd;

double h2d(std::uint16_t h) {
    const int sign = (h >> 15) & 1;
    const int exp = (h >> 10) & 31;
    const int frac = h & 0x3ff;
    double v;
    if (exp == 0) v = std::ldexp(frac, -24);
    else if (exp == 31) v = frac ? std::nan("") : INFINITY;
    else v = std::ldexp(1024 + frac, exp - 25);
    return sign ? -v : v;
}

bool h_nan(std::uint16_t h) { return (h & 0x7c00) == 0x7c00 && (h & 0x3ff) != 0; }
bool h_snan(std::uint16_t h) { return h_nan(h) && !(h & 0x200); }

// Correctly rounded double -> half with A64 rounding mode rm (0 RN, 1 RP, 2 RM, 3 RZ).
std::uint16_t d2h(double v, unsigned rm) {
    if (std::isnan(v)) return 0x7e00;
    const std::uint16_t sign = std::signbit(v) ? 0x8000 : 0;
    const double a = std::fabs(v);
    if (std::isinf(a)) return sign | 0x7c00;
    if (a == 0) return sign;
    int e;
    std::frexp(a, &e);                   // a = f * 2^e, f in [0.5, 1)
    const int qexp = std::max(e - 11, -24);   // quantum 2^qexp (11 significant bits, denormal floor)
    const double x = std::ldexp(a, -qexp);   // exact
    const double fl = std::floor(x), rem = x - fl;
    bool up = false;
    if (rem != 0) {
        switch (rm) {
        case 0: up = rem > 0.5 || (rem == 0.5 && std::fmod(fl, 2.0) != 0); break;
        case 1: up = !sign; break;
        case 2: up = sign != 0; break;
        default: break;
        }
    }
    const double r = std::ldexp(fl + (up ? 1 : 0), qexp);
    if (r >= 65536.0) {
        const bool inf = rm == 0 || (rm == 1 && !sign) || (rm == 2 && sign);
        return sign | (inf ? 0x7c00 : 0x7bff);
    }
    if (r < std::ldexp(1.0, -14)) return sign | static_cast<std::uint16_t>(std::ldexp(r, 24));
    int re;
    const double rf = std::frexp(r, &re);   // r = rf * 2^re
    const unsigned hexp = static_cast<unsigned>(re - 1 + 15);
    const unsigned hfrac = static_cast<unsigned>(std::ldexp(rf * 2 - 1, 10));
    return sign | static_cast<std::uint16_t>(hexp << 10 | hfrac);
}

// Runs f under MXCSR = round mode rm (no FTZ/DAZ: halves widened to double are normal).
template <typename F>
double in_mode(unsigned rm, F f) {
    static constexpr unsigned rc[4] = {0, 2, 1, 3};
    const unsigned saved = _mm_getcsr();
    _mm_setcsr(0x1f80u | (rc[rm] << 13));
    const double r = f();
    _mm_setcsr(saved);
    return r;
}
double dop(char op, double a, double b, unsigned rm) {
    return in_mode(rm, [&] {
        const __m128d x = _mm_set_sd(a), y = _mm_set_sd(b);
        __m128d r;
        switch (op) {
        case '+': r = _mm_add_sd(x, y); break;
        case '-': r = _mm_sub_sd(x, y); break;
        case '*': r = _mm_mul_sd(x, y); break;
        case '/': r = _mm_div_sd(x, y); break;
        default: r = _mm_sqrt_sd(x, x); break;
        }
        return _mm_cvtsd_f64(r);
    });
}
double dfma(double a, double b, double c, unsigned rm) {   // a * b + c, single rounding
    return in_mode(rm, [&] { return _mm_cvtsd_f64(_mm_fmadd_sd(_mm_set_sd(a), _mm_set_sd(b), _mm_set_sd(c))); });
}

std::uint16_t hmax(std::uint16_t a, std::uint16_t b, bool is_max, bool nm) {
    if (h_nan(a) || h_nan(b)) {
        if (nm && !h_snan(a) && !h_snan(b)) {
            if (h_nan(a) && !h_nan(b)) return b;
            if (h_nan(b) && !h_nan(a)) return a;
        }
        return 0x7e00;
    }
    const double x = h2d(a), y = h2d(b);
    if (x == 0 && y == 0) return is_max ? (a & b) : (a | b);
    if (x == y) return a;
    return (is_max ? x > y : x < y) ? a : b;
}

std::uint16_t lane16(const GuestContext& c, unsigned r, unsigned i) {
    return static_cast<std::uint16_t>(c.v[r][i / 4] >> ((i % 4) * 16));
}
void set16(GuestContext& c, unsigned r, unsigned i, std::uint16_t v) {
    auto& h = c.v[r][i / 4];
    const unsigned sh = (i % 4) * 16;
    h = (h & ~(0xffffULL << sh)) | (static_cast<std::uint64_t>(v) << sh);
}

std::int64_t round_int(double x, Rnd rnd, unsigned rm) {
    switch (rnd) {
    case Rnd::n: return static_cast<std::int64_t>(std::nearbyint(x));   // test process runs RN
    case Rnd::a: return static_cast<std::int64_t>(std::round(x));
    case Rnd::p: return static_cast<std::int64_t>(std::ceil(x));
    case Rnd::m: return static_cast<std::int64_t>(std::floor(x));
    case Rnd::z: return static_cast<std::int64_t>(std::trunc(x));
    default: {
        static constexpr Rnd map[4] = {Rnd::n, Rnd::p, Rnd::m, Rnd::z};
        return round_int(x, map[rm], rm);
    }
    }
}

double round_frac(double x, Rnd rnd, unsigned rm) {
    if (std::isnan(x) || std::isinf(x)) return x;
    double r;
    switch (rnd) {
    case Rnd::n: r = std::nearbyint(x); break;
    case Rnd::a: r = std::round(x); break;
    case Rnd::p: r = std::ceil(x); break;
    case Rnd::m: r = std::floor(x); break;
    case Rnd::z: r = std::trunc(x); break;
    default: {
        static constexpr Rnd map[4] = {Rnd::n, Rnd::p, Rnd::m, Rnd::z};
        return round_frac(x, map[rm], rm);
    }
    }
    return std::copysign(r, x);   // keeps -0 for small negatives
}

std::uint64_t sat_int(double x, Rnd rnd, unsigned rm, bool uns, unsigned bits) {
    if (std::isnan(x)) return 0;
    const std::uint64_t mask = bits == 64 ? ~0ULL : ((1ULL << bits) - 1);
    const std::int64_t ilo = uns ? 0 : (bits == 64 ? INT64_MIN : -(1LL << (bits - 1)));
    const std::uint64_t uhi = uns ? mask : (mask >> 1);
    const double r = std::isinf(x) ? x : static_cast<double>(round_int(x, rnd, rm));
    if (r < static_cast<double>(ilo)) return static_cast<std::uint64_t>(ilo) & mask;
    if (r >= std::ldexp(1.0, uns ? bits : bits - 1)) return uhi;
    return static_cast<std::uint64_t>(static_cast<std::int64_t>(r)) & mask;
}

// Reference for half-precision words; false if not modelled.
bool ref_half(std::uint32_t w, GuestContext& c) {
    Inst in;
    if (!fpx::decode(w, in) || (in.sh.es != 16 && in.dst_es != 16)) return false;
    const unsigned rm = (c.fpcr >> 22) & 3;
    const GuestContext src = c;
    const unsigned lanes = in.sh.scalar ? 1 : (in.sh.q ? 8 : 4);
    std::uint16_t out[8] = {};
    const auto n = [&](unsigned i) { return lane16(src, in.rn, i); };
    const auto m = [&](unsigned i) { return lane16(src, in.rm, in.index >= 0 ? static_cast<unsigned>(in.index) : i); };
    const auto arith = [&](std::uint16_t a, std::uint16_t b) -> std::uint16_t {
        const double x = h2d(a), y = h2d(b);
        switch (in.k) {
        case K::fadd: return d2h(dop('+', x, y, rm), rm);
        case K::fsub: return d2h(dop('-', x, y, rm), rm);
        case K::fmul: return d2h(dop('*', x, y, rm), rm);
        case K::fdiv: return d2h(dop('/', x, y, rm), rm);
        case K::fabd: { const std::uint16_t r = d2h(dop('-', x, y, rm), rm); return h_nan(r) ? r : r & 0x7fff; }
        case K::fnmul: { const std::uint16_t r = d2h(dop('*', x, y, rm), rm); return h_nan(r) ? r : r ^ 0x8000; }
        case K::fmax: return hmax(a, b, true, false);
        case K::fmin: return hmax(a, b, false, false);
        case K::fmaxnm: return hmax(a, b, true, true);
        case K::fminnm: return hmax(a, b, false, true);
        case K::fcmeq: return x == y ? 0xffff : 0;
        case K::fcmge: return x >= y ? 0xffff : 0;
        case K::fcmgt: return x > y ? 0xffff : 0;
        case K::facge: return std::fabs(x) >= std::fabs(y) ? 0xffff : 0;
        case K::facgt: return std::fabs(x) > std::fabs(y) ? 0xffff : 0;
        default: return 0;
        }
    };
    switch (in.k) {
    case K::fadd: case K::fsub: case K::fmul: case K::fdiv: case K::fabd: case K::fnmul:
    case K::fmax: case K::fmin: case K::fmaxnm: case K::fminnm:
    case K::fcmeq: case K::fcmge: case K::fcmgt: case K::facge: case K::facgt:
        for (unsigned i = 0; i < lanes; ++i) out[i] = arith(n(i), m(i));
        break;
    case K::fcmeq0: case K::fcmge0: case K::fcmgt0: case K::fcmle0: case K::fcmlt0:
        for (unsigned i = 0; i < lanes; ++i) {
            const double x = h2d(n(i));
            bool r = false;
            switch (in.k) {
            case K::fcmeq0: r = x == 0; break;
            case K::fcmge0: r = x >= 0; break;
            case K::fcmgt0: r = x > 0; break;
            case K::fcmle0: r = x <= 0; break;
            default: r = x < 0; break;
            }
            out[i] = r ? 0xffff : 0;
        }
        break;
    case K::fsqrt:
        for (unsigned i = 0; i < lanes; ++i) out[i] = d2h(dop('s', h2d(n(i)), 0, rm), rm);
        break;
    case K::fabs:
        for (unsigned i = 0; i < lanes; ++i) out[i] = n(i) & 0x7fff;
        break;
    case K::frecpx: {
        const std::uint16_t a = n(0);
        if (h_nan(a)) out[0] = a | 0x200;
        else {
            const unsigned ex = (a >> 10) & 31;
            out[0] = static_cast<std::uint16_t>((a & 0x8000) | ((ex == 0 ? 30u : (~ex & 31u)) << 10));
        }
        break;
    }
    case K::fneg:
        for (unsigned i = 0; i < lanes; ++i) out[i] = n(i) ^ 0x8000;
        break;
    case K::frint:
        for (unsigned i = 0; i < lanes; ++i) {
            const std::uint16_t a = n(i);
            out[i] = h_nan(a) ? 0x7e00 : d2h(round_frac(h2d(a), in.rnd, rm), rm);
        }
        break;
    case K::fmla: case K::fmls:
        for (unsigned i = 0; i < lanes; ++i) {
            const double acc = h2d(lane16(src, in.rd, i)), x = h2d(n(i)), y = h2d(m(i));
            out[i] = d2h(dfma(in.k == K::fmla ? x : -x, y, acc, rm), rm);
        }
        break;
    case K::fmadd: case K::fmsub: case K::fnmadd: case K::fnmsub: {
        const double a = h2d(lane16(src, in.ra, 0)), x = h2d(n(0)), y = h2d(m(0));
        const bool negp = in.k == K::fmsub || in.k == K::fnmadd;
        const bool nega = in.k == K::fnmadd || in.k == K::fnmsub;
        out[0] = d2h(dfma(negp ? -x : x, y, nega ? -a : a, rm), rm);
        break;
    }
    case K::faddp: case K::fmaxp: case K::fminp: case K::fmaxnmp: case K::fminnmp: {
        const auto op = [&](std::uint16_t a, std::uint16_t b) -> std::uint16_t {
            switch (in.k) {
            case K::faddp: return d2h(dop('+', h2d(a), h2d(b), rm), rm);
            case K::fmaxp: return hmax(a, b, true, false);
            case K::fminp: return hmax(a, b, false, false);
            case K::fmaxnmp: return hmax(a, b, true, true);
            default: return hmax(a, b, false, true);
            }
        };
        if (in.sh.scalar) {
            out[0] = op(n(0), n(1));
        } else {
            std::uint16_t cat[16];
            for (unsigned i = 0; i < lanes; ++i) { cat[i] = lane16(src, in.rn, i); cat[lanes + i] = lane16(src, in.rm, i); }
            for (unsigned i = 0; i < lanes; ++i) out[i] = op(cat[2 * i], cat[2 * i + 1]);
        }
        break;
    }
    case K::fmaxv: case K::fminv: case K::fmaxnmv: case K::fminnmv: {
        const bool mx = in.k == K::fmaxv || in.k == K::fmaxnmv, nm = in.k == K::fmaxnmv || in.k == K::fminnmv;
        std::uint16_t v[8];
        for (unsigned i = 0; i < lanes; ++i) v[i] = n(i);
        for (unsigned width = lanes; width > 1; width /= 2)
            for (unsigned i = 0; i < width / 2; ++i) v[i] = hmax(v[2 * i], v[2 * i + 1], mx, nm);
        out[0] = v[0];
        c.v[in.rd][0] = out[0];
        c.v[in.rd][1] = 0;
        return true;
    }
    case K::cvt_f2i: {
        if (in.gpr) {
            const double x = std::ldexp(h2d(n(0)), in.fbits);
            const std::uint64_t r = sat_int(x, in.rnd, rm, in.is_unsigned, in.ibits);
            if (in.rd != 31) c.x[in.rd] = r;
            return true;
        }
        for (unsigned i = 0; i < lanes; ++i)
            out[i] = static_cast<std::uint16_t>(sat_int(std::ldexp(h2d(n(i)), in.fbits), in.rnd, rm, in.is_unsigned, 16));
        break;
    }
    case K::cvt_i2f: {
        if (in.gpr) {
            std::uint64_t x = src.x[in.rn];
            if (in.rn == 31) x = 0;
            double v;
            if (in.ibits == 32) v = in.is_unsigned ? static_cast<double>(static_cast<std::uint32_t>(x))
                                                   : static_cast<double>(static_cast<std::int32_t>(x));
            else if (in.is_unsigned) v = x > (1ULL << 20) ? std::ldexp(1.0, 20) : static_cast<double>(x);
            else {
                const auto sx = static_cast<std::int64_t>(x);
                v = sx > (1LL << 20) ? std::ldexp(1.0, 20) : sx < -(1LL << 20) ? -std::ldexp(1.0, 20) : static_cast<double>(sx);
            }
            out[0] = d2h(std::ldexp(v, -static_cast<int>(in.fbits)), rm);
            break;
        }
        for (unsigned i = 0; i < lanes; ++i) {
            const std::uint16_t a = n(i);
            const double v = in.is_unsigned ? static_cast<double>(a) : static_cast<double>(static_cast<std::int16_t>(a));
            out[i] = d2h(std::ldexp(v, -static_cast<int>(in.fbits)), rm);
        }
        break;
    }
    case K::fcmp: case K::fccmp: {
        std::uint64_t nzcv;
        const double x = h2d(n(0)), y = in.zero ? 0.0 : h2d(lane16(src, in.rm, 0));
        if (std::isnan(x) || std::isnan(y)) nzcv = 0x30000000;
        else if (x == y) nzcv = 0x60000000;
        else if (x < y) nzcv = 0x80000000;
        else nzcv = 0x20000000;
        if (in.k == K::fccmp && in.cond < 14) {
            const unsigned f = static_cast<unsigned>(src.nzcv >> 28);
            const bool N = f & 8, Z = f & 4, C = f & 2, V = f & 1;
            bool r;
            switch (in.cond >> 1) {
            case 0: r = Z; break;
            case 1: r = C; break;
            case 2: r = N; break;
            case 3: r = V; break;
            case 4: r = C && !Z; break;
            case 5: r = N == V; break;
            default: r = N == V && !Z; break;
            }
            if (in.cond & 1) r = !r;
            if (!r) nzcv = static_cast<std::uint64_t>(in.nzcv) << 28;
        }
        c.nzcv = nzcv;
        return true;
    }
    case K::fmov_from_gpr:
        c.v[in.rd][0] = (in.rn == 31 ? 0 : src.x[in.rn]) & 0xffff;
        c.v[in.rd][1] = 0;
        return true;
    case K::fmov_to_gpr:
        if (in.rd != 31) c.x[in.rd] = lane16(src, in.rn, 0);
        return true;
    default:
        return false;
    }
    if (in.sh.scalar) {
        c.v[in.rd][0] = out[0];
        c.v[in.rd][1] = 0;
    } else {
        c.v[in.rd][0] = c.v[in.rd][1] = 0;
        for (unsigned i = 0; i < lanes; ++i) set16(c, in.rd, i, out[i]);
    }
    return true;
}

Spec half_spec(const char* name, std::function<std::uint32_t(Rng&)> gen, bool fp_result, bool snan = true) {
    Spec s{name, std::move(gen), 16, fp_result ? fp_opt(16) : exact_opt()};
    s.opt.reference = ref_half;
    s.snan = snan;
    s.words = 96;
    s.contexts = 5;
    return s;
}

} // namespace

FX_TEST(fp_half_reference_conversions_are_exact) {
    // d2h against the oracle's FCVT H, D on random doubles (every mode).
    Rng r(77);
    unsigned bad = 0;
    for (unsigned i = 0; i < 4000; ++i) {
        GuestContext c = testing::random_context(i);
        c.v[1][0] = random_fp(r, 64);
        c.fpcr = (r.below(4) << 22);
        GuestContext e = c;
        FX_REQUIRE(testing::oracle_step(e, enc::fp1(7, 1, 0, 1)));   // FCVT H0, D1
        double d;
        std::memcpy(&d, &c.v[1][0], 8);
        const std::uint16_t mine = d2h(d, (c.fpcr >> 22) & 3);
        const auto theirs = static_cast<std::uint16_t>(e.v[0][0]);
        if (mine != theirs && !(h_nan(mine) && h_nan(theirs))) {
            if (++bad < 5) std::printf("    d2h mismatch %016llx mode %u: %04x vs %04x\n",
                                       static_cast<unsigned long long>(c.v[1][0]), (c.fpcr >> 22) & 3, mine, theirs);
        }
    }
    FX_CHECK_EQ(bad, 0u);
}

FX_TEST(fp_half_conversions_match_oracle) {
    run_specs({
        Spec{"fcvt H<-S", [](Rng& r) { return enc::fp1(7, 0, reg(r), reg(r)); }, 32, fp_opt(16)},
        Spec{"fcvt H<-D", [](Rng& r) { return enc::fp1(7, 1, reg(r), reg(r)); }, 64, fp_opt(16)},
        Spec{"fcvt S<-H", [](Rng& r) { return enc::fp1(4, 3, reg(r), reg(r)); }, 16, fp_opt(32)},
        Spec{"fcvt D<-H", [](Rng& r) { return enc::fp1(5, 3, reg(r), reg(r)); }, 16, fp_opt(64)},
        Spec{"fcvtl/fcvtl2 H->S", [](Rng& r) { return enc::v2(r.below(2), 0, 0, 0x17, reg(r), reg(r)); }, 16, fp_opt(32)},
        Spec{"fcvtn/fcvtn2 S->H", [](Rng& r) { return enc::v2(r.below(2), 0, 0, 0x16, reg(r), reg(r)); }, 32, fp_opt(16)},
    });
}

FX_TEST(fp_half_arith_matches_reference) {
    run_specs({
        half_spec("fadd/fsub/fmul/fdiv/fnmul H", [](Rng& r) {
            static const unsigned ops[] = {0, 1, 2, 3, 8};
            return enc::fp2(ops[r.below(5)], 3, reg(r), reg(r), reg(r));
        }, true),
        half_spec("fmax/fmin/nm H", [](Rng& r) { return enc::fp2(4 + r.below(4), 3, reg(r), reg(r), reg(r)); }, true, false),
        half_spec("fsqrt/frint H", [](Rng& r) {
            static const unsigned ops[] = {3, 8, 9, 10, 11, 12, 14, 15};
            return enc::fp1(ops[r.below(8)], 3, reg(r), reg(r));
        }, true),
        half_spec("fmadd family H", [](Rng& r) { return enc::fp3(r.below(2), r.below(2), 3, reg(r), reg(r), reg(r), reg(r)); }, true),
        half_spec("fcmp/fccmp H", [](Rng& r) {
            return r.below(2) ? enc::fcmp(3, reg(r), reg(r), r.below(4) * 8)
                              : enc::fccmp(3, reg(r), reg(r), r.below(16), r.below(2), r.below(16));
        }, false),
        half_spec("fmov H<->W/X", [](Rng& r) { return enc::fpint(r.below(2), 3, 0, 6 + r.below(2), reg(r), reg(r)); }, false),
    });
}

FX_TEST(fp_half_vector_matches_reference) {
    // three same FP16: 0 Q U 01110 a 10 Rm 00 opcode 1 Rn Rd
    const auto v3h = [](unsigned q, unsigned u, unsigned a, unsigned opc, unsigned rd, unsigned rn, unsigned rm) {
        return q << 30 | u << 29 | 0x0e400400u | a << 23 | rm << 16 | opc << 11 | rn << 5 | rd;
    };
    // two-reg misc FP16: 0 Q U 01110 a 1111 00 opcode 10 Rn Rd
    const auto v2h = [](unsigned q, unsigned u, unsigned a, unsigned opc, unsigned rd, unsigned rn) {
        return q << 30 | u << 29 | 0x0e780800u | a << 23 | opc << 12 | rn << 5 | rd;
    };
    struct Op { unsigned u, a, opc; };
    static const Op arith[] = {{0, 0, 2}, {0, 1, 2}, {1, 0, 3}, {1, 0, 7}, {1, 1, 2}};
    static const Op minmax[] = {{0, 0, 6}, {0, 1, 6}, {0, 0, 0}, {0, 1, 0}, {1, 0, 6}, {1, 1, 6}, {1, 0, 0},
                                {1, 1, 0}, {1, 0, 2}};
    static const Op cmp[] = {{0, 0, 4}, {1, 0, 4}, {1, 1, 4}, {1, 0, 5}, {1, 1, 5}};
    static const Op unary[] = {{0, 0, 0x18}, {0, 0, 0x19}, {0, 1, 0x18}, {0, 1, 0x19}, {1, 0, 0x18},
                               {1, 0, 0x19}, {1, 1, 0x19}, {1, 1, 0x1f}, {0, 1, 0x0f}, {1, 1, 0x0f}};
    static const Op cmp0[] = {{0, 1, 0x0c}, {0, 1, 0x0d}, {0, 1, 0x0e}, {1, 1, 0x0c}, {1, 1, 0x0d}};
    static const Op cvt[] = {{0, 0, 0x1a}, {0, 0, 0x1b}, {0, 0, 0x1c}, {0, 1, 0x1a}, {0, 1, 0x1b},
                             {1, 0, 0x1a}, {1, 0, 0x1b}, {1, 0, 0x1c}, {1, 1, 0x1a}, {1, 1, 0x1b},
                             {0, 0, 0x1d}, {1, 0, 0x1d}};
    run_specs({
        half_spec("vector arith 8H/4H", [=](Rng& r) { const Op& o = arith[r.below(5)]; return v3h(r.below(2), o.u, o.a, o.opc, reg(r), reg(r), reg(r)); }, true),
        half_spec("vector min/max/pairwise H", [=](Rng& r) { const Op& o = minmax[r.below(9)]; return v3h(r.below(2), o.u, o.a, o.opc, reg(r), reg(r), reg(r)); }, true, false),
        half_spec("vector compares H", [=](Rng& r) { const Op& o = cmp[r.below(5)]; return v3h(r.below(2), o.u, o.a, o.opc, reg(r), reg(r), reg(r)); }, false),
        half_spec("vector fmla/fmls H", [=](Rng& r) { return v3h(r.below(2), 0, r.below(2), 1, reg(r), reg(r), reg(r)); }, true),
        half_spec("vector unary H", [=](Rng& r) { const Op& o = unary[r.below(10)]; return v2h(r.below(2), o.u, o.a, o.opc, reg(r), reg(r)); }, true),
        half_spec("vector compare0 H", [=](Rng& r) { const Op& o = cmp0[r.below(5)]; return v2h(r.below(2), o.u, o.a, o.opc, reg(r), reg(r)); }, false),
        half_spec("vector conversions H", [=](Rng& r) { const Op& o = cvt[r.below(12)]; return v2h(r.below(2), o.u, o.a, o.opc, reg(r), reg(r)); }, false),
        half_spec("by element H", [](Rng& r) {
            static const unsigned ops[][2] = {{0, 1}, {0, 5}, {0, 9}};
            const auto& o = ops[r.below(3)];
            const unsigned idx = r.below(8);
            return enc::velem(r.below(2), o[0], 0, (idx >> 1) & 1, idx & 1, r.below(16), o[1], idx >> 2, reg(r), reg(r));
        }, true),
        half_spec("across lanes H", [](Rng& r) {
            static const unsigned ops[][2] = {{0, 0x0c}, {0, 0x0f}, {1, 0x0c}, {1, 0x0f}};
            const auto& o = ops[r.below(4)];
            return enc::vacross(r.below(2), 0, o[0] << 1, o[1], reg(r), reg(r));
        }, true, false),
        half_spec("scalar pairwise H", [](Rng& r) {
            static const unsigned ops[][2] = {{0, 0x0c}, {0, 0x0d}, {0, 0x0f}, {1, 0x0c}, {1, 0x0f}};
            const auto& o = ops[r.below(5)];
            return enc::spair(0, o[0] << 1, o[1], reg(r), reg(r));
        }, true, false),
        half_spec("fixed point vector H", [](Rng& r) {
            return enc::vshift(r.below(2), r.below(2), 16 + r.below(16), r.below(2) ? 0x1c : 0x1f, reg(r), reg(r));
        }, false),
    });
}

FX_TEST(fp_half_scalar_simd_and_gpr_conversions_match_reference) {
    run_specs({
        half_spec("fcvt* H->gpr", [](Rng& r) {
            static const unsigned rm_opc[][2] = {{0, 0}, {0, 1}, {0, 4}, {0, 5}, {1, 0}, {1, 1}, {2, 0}, {2, 1}, {3, 0}, {3, 1}};
            const auto& p = rm_opc[r.below(10)];
            return enc::fpint(r.below(2), 3, p[0], p[1], reg(r), reg(r));
        }, false),
        half_spec("scvtf/ucvtf gpr->H", [](Rng& r) { return enc::fpint(r.below(2), 3, 0, 2 + r.below(2), reg(r), reg(r)); }, true),
        half_spec("scvtf/ucvtf W->H fixed", [](Rng& r) { return enc::fpfix(0, 3, 0, 2 + r.below(2), 32 + r.below(32), reg(r), reg(r)); }, true),
        // AdvSIMD scalar two-reg misc FP16: 01 U 11110 a 1111 00 opcode 10 Rn Rd
        half_spec("scalar simd H misc", [](Rng& r) {
            static const unsigned ops[][3] = {{0, 1, 0x1f}, {0, 0, 0x1a}, {0, 1, 0x1b}, {1, 0, 0x1c}, {1, 1, 0x1b},
                                              {0, 0, 0x1d}, {1, 0, 0x1d}, {0, 1, 0x0c}, {1, 1, 0x0d}};
            const auto& o = ops[r.below(9)];
            return 0x5e780800u | o[0] << 29 | o[1] << 23 | o[2] << 12 | reg(r) << 5 | reg(r);
        }, false),
    });
}


FX_TEST(fp_half_special_pairs_match_reference) {
    const auto v3h = [](unsigned u, unsigned a, unsigned opc) {
        return 1u << 30 | u << 29 | 0x0e400400u | a << 23 | 2u << 16 | opc << 11 | 1u << 5 | 3u;
    };
    struct Op { const char* name; std::uint32_t word; bool snan; bool fp; };
    const Op ops[] = {
        {"fadd 8H", v3h(0, 0, 2), true, true},    {"fsub 8H", v3h(0, 1, 2), true, true},
        {"fmul 8H", v3h(1, 0, 3), true, true},    {"fdiv 8H", v3h(1, 0, 7), true, true},
        {"fabd 8H", v3h(1, 1, 2), true, true},    {"fmax 8H", v3h(0, 0, 6), true, true},
        {"fmin 8H", v3h(0, 1, 6), true, true},    {"fmaxnm 8H", v3h(0, 0, 0), false, true},
        {"fminnm 8H", v3h(0, 1, 0), false, true}, {"fcmge 8H", v3h(1, 0, 4), true, false},
        {"facgt 8H", v3h(1, 1, 5), true, false},  {"fmla 8H", v3h(0, 0, 1), true, true},
        {"fmls 8H", v3h(0, 1, 1), true, true},
    };
    for (const auto& o : ops) {
        std::vector<std::uint64_t> list;
        for (auto v : specials(16))
            if (o.snan || !is_snan(v, 16)) list.push_back(v);
        std::vector<GuestContext> ctxs;
        Rng rng(o.word);
        std::size_t k = 0;
        const std::size_t pairs = list.size() * list.size();
        for (const auto fpcr : fpcr_modes()) {
            for (std::size_t i = 0; i < pairs; i += 8) {
                GuestContext c = testing::random_context(rng.next());
                fill_vectors(c, rng, 16, o.snan);
                c.fpcr = fpcr;
                for (unsigned l = 0; l < 8; ++l, ++k) {
                    const std::size_t p = (i + l) % pairs;
                    set16(c, 1, l, static_cast<std::uint16_t>(list[p / list.size()]));
                    set16(c, 2, l, static_cast<std::uint16_t>(list[p % list.size()]));
                    set16(c, 3, l, static_cast<std::uint16_t>(list[rng.below(static_cast<unsigned>(list.size()))]));
                }
                ctxs.push_back(c);
            }
        }
        CheckOptions opt = o.fp ? fp_opt(16) : exact_opt();
        opt.reference = ref_half;
        const unsigned n = check_batch(o.word, ctxs, opt, o.name);
        std::printf("    %-10s %6u contexts\n", o.name, n);
        FX_CHECK(n == ctxs.size());
    }
}
