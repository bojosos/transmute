// Floating-point math: libm functions, conversions, classification, rounding
// modes and flush-to-zero through the CRT's control functions.
#include <fenv.h>
#include <float.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static volatile double inputs[] = {0.5, 1.0, -2.25, 3.141592653589793, 10.0, 1e-3, 123.456, -0.0};

static uint64_t bits_of(double d) {
    uint64_t u;
    memcpy(&u, &d, sizeof u);
    return u;
}

static uint32_t bits_of_f(float f) {
    uint32_t u;
    memcpy(&u, &f, sizeof u);
    return u;
}

// NaN sign/payload is implementation-defined; print "nan" for every NaN.
static void show(const char* name, double v) {
    if (isnan(v)) {
        printf("%s=nan\n", name);
    } else {
        printf("%s=%.15g [%016llx]\n", name, v, (unsigned long long)bits_of(v));
    }
}

static void show_f(const char* name, float v) {
    if (isnan(v)) {
        printf("%s=nan\n", name);
    } else {
        printf("%s=%.8g [%08x]\n", name, v, bits_of_f(v));
    }
}

int main(void) {
    for (int i = 0; i < 8; i++) {
        double x = inputs[i];
        printf("x=%g sin=%.15g cos=%.15g tan=%.15g exp=%.15g\n", x, sin(x), cos(x), tan(x), exp(x));
        printf("  atan=%.15g sinh=%.15g tanh=%.15g cbrt=%.15g erf=%.15g\n", atan(x), sinh(x), tanh(x), cbrt(x), erf(x));
        if (x > 0) {
            printf("  log=%.15g log10=%.15g log2=%.15g sqrt=%.17g lgamma=%.15g tgamma=%.12g\n", log(x), log10(x),
                   log2(x), sqrt(x), lgamma(x), tgamma(x));
        }
    }
    volatile double two = 2.0, half = 0.5, neg8 = -8.0, third = 1.0 / 3.0, big = 1e22, tiny = 1e-310;
    show("pow(2,0.5)", pow(two, half));
    show("pow(2,-1074)", pow(two, -1074.0));
    show("pow(-8,1/3)", pow(neg8, third));
    show("pow(-8,3)", pow(neg8, 3.0));
    show("pow(10,22)", pow(10.0, 22.0 * half * two));
    show("sin(1e22)", sin(big));
    show("exp(-745)", exp(-745.0 * half * two));
    show("exp(710)", exp(710.0 * half * two));
    show("log(0)", log(tiny - tiny));
    show("log(-1)", log(-half));
    show("sqrt(-1)", sqrt(-half));
    show("atan2(1,-1)", atan2(half, -half));
    show("atan2(-0,-1)", atan2(-0.0 * half, -half));
    show("hypot(3e200,4e200)", hypot(3e200 * half * two, 4e200));
    show("fmod(10.5,3)", fmod(10.5 * half * two, 3.0));
    show("remainder(10.5,3)", remainder(10.5 * half * two, 3.0));
    show("fma(0.1,10,-1)", fma(0.1 * half * two, 10.0, -1.0));
    show("expm1(1e-10)", expm1(1e-10 * half * two));
    show("log1p(1e-10)", log1p(1e-10 * half * two));
    show("nextafter(1,2)", nextafter(half * two, two));
    show("ldexp(0.75,-1060)", ldexp(0.75 * half * two, -1060));
    int exponent = 0;
    double mantissa = frexp(tiny, &exponent);
    printf("frexp(1e-310)=%.17g,%d ilogb=%d logb=%g\n", mantissa, exponent, ilogb(tiny), logb(tiny));
    double whole = 0;
    double frac = modf(-123.456 * half * two, &whole);
    printf("modf=%.15g,%.15g\n", frac, whole);

    double rounding[] = {2.5, 3.5, -2.5, -0.4, 0.49999999999999994, 1e16 + 1, -7.5};
    for (int i = 0; i < 7; i++) {
        volatile double r = rounding[i];
        printf("round %.17g: floor=%g ceil=%g trunc=%g round=%g rint=%g nearbyint=%g lround=%ld llrint=%lld\n", r,
               floor(r), ceil(r), trunc(r), round(r), rint(r), nearbyint(r), fabs(r) < 2e9 ? lround(r) : 0L, llrint(r));
    }

    volatile float f1 = 1.1f, f2 = 3.0f;
    show_f("sinf", sinf(f1));
    show_f("expf", expf(f1));
    show_f("powf", powf(f1, f2));
    show_f("sqrtf", sqrtf(f1));
    show_f("f1/f2", f1 / f2);
    show_f("fmaf", fmaf(f1, f2, -3.3f));
    show_f("(float)1/3", (float)third);
    show_f("(float)16777217", (float)(16777216 + (int)(half * two)));

    volatile double dv = 1e18;
    volatile double neg = -2.9;
    volatile double u63 = 9223372036854775808.0 * 1.5;
    printf("conv: %lld %d %llu %u %lld\n", (long long)dv, (int)neg, (unsigned long long)u63, (unsigned)(dv / 1e9),
           (long long)(float)dv);
    volatile long long i53 = (1LL << 53) + 1;
    volatile unsigned long long umax = UINT64_MAX;
    printf("int->fp: %.17g %.17g %.9g\n", (double)i53, (double)umax, (float)i53);

    double cls[] = {0.0, -0.0, 1.0, tiny, INFINITY, -INFINITY, NAN};
    for (int i = 0; i < 7; i++) {
        double c = cls[i];
        printf("classify[%d] fpclassify=%d isfinite=%d isinf=%d isnan=%d isnormal=%d signbit=%d\n", i, fpclassify(c),
               isfinite(c), isinf(c), isnan(c), isnormal(c), signbit(c) != 0);
    }
    printf("compare nan: lt=%d eq=%d ne=%d unordered=%d\n", NAN < 1.0, NAN == NAN, NAN != NAN, isunordered(NAN, 1.0));

    // Rounding modes through fesetround: division and conversion honor them.
    int modes[] = {FE_TONEAREST, FE_UPWARD, FE_DOWNWARD, FE_TOWARDZERO};
    const char* names[] = {"nearest", "upward", "downward", "towardzero"};
    for (int i = 0; i < 4; i++) {
        fesetround(modes[i]);
        volatile double one = 1.0, three = 3.0, minus = -1.0;
        volatile double q1 = one / three, q2 = minus / three;
        volatile float fq = (float)one / (float)three;
        volatile double r25 = 2.5;
        printf("mode %s: getround-match=%d 1/3=%.17g -1/3=%.17g f=%.9g rint(2.5)=%g rint(-2.5)=%g lrint(-1.5)=%ld\n",
               names[i], fegetround() == modes[i], q1, q2, fq, rint(r25), rint(-r25), lrint(-1.5 * one));
    }
    fesetround(FE_TONEAREST);

    unsigned int control = 0;
    _controlfp_s(&control, 0, 0);
    printf("controlfp rounding=%x denormal=%x\n", control & _MCW_RC, control & _MCW_DN);
    volatile double denorm_in = 1e-310;
    volatile double scale = 1.0;
    double before_ftz = denorm_in * scale;
    _controlfp_s(&control, _DN_FLUSH, _MCW_DN);
    double after_ftz = denorm_in * scale;
    volatile double min_normal = DBL_MIN;
    double underflow = min_normal * 0.25;
    _controlfp_s(&control, _DN_SAVE, _MCW_DN);
    double restored = denorm_in * scale;
    printf("ftz before=%g after=%g underflow=%g restored=%g\n", before_ftz, after_ftz, underflow, restored);

    feclearexcept(FE_ALL_EXCEPT);
    volatile double zero = 0.0;
    volatile double three_v = 3.0;
    volatile double inexact = 1.0 / three_v;
    (void)inexact;
    printf("fetestexcept inexact=%d\n", fetestexcept(FE_INEXACT) != 0);
    feclearexcept(FE_ALL_EXCEPT);
    volatile double dz = 1.0 / zero;
    (void)dz;
    printf("fetestexcept divbyzero=%d inexact=%d\n", fetestexcept(FE_DIVBYZERO) != 0, fetestexcept(FE_INEXACT) != 0);
    fflush(stdout);
    return 0;
}
