#ifndef ALCHEMY_MATH_H
#define ALCHEMY_MATH_H
#define FP_NAN 1
#define FP_INFINITE 2
#define FP_ZERO 3
#define FP_NORMAL 4
#define FP_SUBNORMAL 5
#define HUGE_VAL __builtin_huge_val()
#define HUGE_VALF __builtin_huge_valf()
#define HUGE_VALL __builtin_huge_vall()
#define INFINITY __builtin_inff()
#define NAN __builtin_nanf("")
#define MATH_ERRNO 1
#define MATH_ERREXCEPT 2
#define math_errhandling MATH_ERRNO
#ifdef __cplusplus
extern "C" {
#endif
#define ALCHEMY_MATH_UNARY(name) double name(double); float name##f(float); long double name##l(long double);
#define ALCHEMY_MATH_BINARY(name) double name(double, double); float name##f(float, float); long double name##l(long double, long double);
ALCHEMY_MATH_UNARY(acos) ALCHEMY_MATH_UNARY(asin) ALCHEMY_MATH_UNARY(atan)
ALCHEMY_MATH_UNARY(cos) ALCHEMY_MATH_UNARY(sin) ALCHEMY_MATH_UNARY(tan)
ALCHEMY_MATH_UNARY(acosh) ALCHEMY_MATH_UNARY(asinh) ALCHEMY_MATH_UNARY(atanh)
ALCHEMY_MATH_UNARY(cosh) ALCHEMY_MATH_UNARY(sinh) ALCHEMY_MATH_UNARY(tanh)
ALCHEMY_MATH_UNARY(exp) ALCHEMY_MATH_UNARY(exp2) ALCHEMY_MATH_UNARY(expm1)
ALCHEMY_MATH_UNARY(log) ALCHEMY_MATH_UNARY(log10) ALCHEMY_MATH_UNARY(log2) ALCHEMY_MATH_UNARY(log1p)
ALCHEMY_MATH_UNARY(sqrt) ALCHEMY_MATH_UNARY(cbrt) ALCHEMY_MATH_UNARY(ceil) ALCHEMY_MATH_UNARY(floor)
ALCHEMY_MATH_UNARY(fabs) ALCHEMY_MATH_UNARY(trunc) ALCHEMY_MATH_UNARY(round) ALCHEMY_MATH_UNARY(nearbyint) ALCHEMY_MATH_UNARY(rint)
ALCHEMY_MATH_UNARY(erf) ALCHEMY_MATH_UNARY(erfc) ALCHEMY_MATH_UNARY(tgamma) ALCHEMY_MATH_UNARY(lgamma)
ALCHEMY_MATH_BINARY(atan2) ALCHEMY_MATH_BINARY(pow) ALCHEMY_MATH_BINARY(fmod) ALCHEMY_MATH_BINARY(remainder)
ALCHEMY_MATH_BINARY(copysign) ALCHEMY_MATH_BINARY(fdim) ALCHEMY_MATH_BINARY(fmax) ALCHEMY_MATH_BINARY(fmin)
ALCHEMY_MATH_BINARY(hypot) ALCHEMY_MATH_BINARY(nextafter)
double frexp(double, int*); float frexpf(float, int*); long double frexpl(long double, int*);
double ldexp(double, int); float ldexpf(float, int); long double ldexpl(long double, int);
double modf(double, double*); float modff(float, float*); long double modfl(long double, long double*);
double scalbn(double, int); float scalbnf(float, int); long double scalbnl(long double, int);
double scalbln(double, long); float scalblnf(float, long); long double scalblnl(long double, long);
int ilogb(double); int ilogbf(float); int ilogbl(long double);
long lrint(double); long lrintf(float); long lrintl(long double);
long lround(double); long lroundf(float); long lroundl(long double);
long long llrint(double); long long llrintf(float); long long llrintl(long double);
long long llround(double); long long llroundf(float); long long llroundl(long double);
double fma(double, double, double); float fmaf(float, float, float); long double fmal(long double, long double, long double);
double nan(const char*); float nanf(const char*); long double nanl(const char*);
double nexttoward(double, long double); float nexttowardf(float, long double); long double nexttowardl(long double, long double);
double remquo(double, double, int*); float remquof(float, float, int*); long double remquol(long double, long double, int*);
#undef ALCHEMY_MATH_UNARY
#undef ALCHEMY_MATH_BINARY
#ifdef __cplusplus
}
#endif
#endif
