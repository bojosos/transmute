// Alchemy metal_stdlib: common, math, integer, geometric, relational and
// pack functions. Project-authored (MIT) from the MSL specification.
#pragma once

namespace metal {

// GLSL.std.450 opcode numbers used below.
enum : uint {
    __GLSL_Round = 1, __GLSL_RoundEven = 2, __GLSL_Trunc = 3, __GLSL_FAbs = 4, __GLSL_SAbs = 5,
    __GLSL_FSign = 6, __GLSL_SSign = 7, __GLSL_Floor = 8, __GLSL_Ceil = 9, __GLSL_Fract = 10,
    __GLSL_Sin = 13, __GLSL_Cos = 14, __GLSL_Tan = 15, __GLSL_Asin = 16, __GLSL_Acos = 17,
    __GLSL_Atan = 18, __GLSL_Sinh = 19, __GLSL_Cosh = 20, __GLSL_Tanh = 21, __GLSL_Asinh = 22,
    __GLSL_Acosh = 23, __GLSL_Atanh = 24, __GLSL_Atan2 = 25, __GLSL_Pow = 26, __GLSL_Exp = 27,
    __GLSL_Log = 28, __GLSL_Exp2 = 29, __GLSL_Log2 = 30, __GLSL_Sqrt = 31, __GLSL_InverseSqrt = 32,
    __GLSL_FMin = 37, __GLSL_UMin = 38, __GLSL_SMin = 39, __GLSL_FMax = 40, __GLSL_UMax = 41,
    __GLSL_SMax = 42, __GLSL_FClamp = 43, __GLSL_UClamp = 44, __GLSL_SClamp = 45, __GLSL_FMix = 46,
    __GLSL_Step = 48, __GLSL_SmoothStep = 49, __GLSL_Fma = 50, __GLSL_Ldexp = 53,
    __GLSL_PackSnorm4x8 = 54, __GLSL_PackUnorm4x8 = 55, __GLSL_PackSnorm2x16 = 56,
    __GLSL_PackUnorm2x16 = 57, __GLSL_PackHalf2x16 = 58, __GLSL_UnpackSnorm2x16 = 60,
    __GLSL_UnpackUnorm2x16 = 61, __GLSL_UnpackHalf2x16 = 62, __GLSL_UnpackSnorm4x8 = 63,
    __GLSL_UnpackUnorm4x8 = 64, __GLSL_Length = 66, __GLSL_Distance = 67, __GLSL_Cross = 68,
    __GLSL_Normalize = 69, __GLSL_FaceForward = 70, __GLSL_Reflect = 71, __GLSL_Refract = 72,
    __GLSL_FindILsb = 73, __GLSL_FindSMsb = 74, __GLSL_FindUMsb = 75,
};
// SPIR-V opcodes used below.
enum : uint {
    __SPV_FRem = 140, __SPV_Dot = 148, __SPV_IsNan = 156, __SPV_IsInf = 157,
    __SPV_BitFieldInsert = 201, __SPV_BitFieldSExtract = 202, __SPV_BitFieldUExtract = 203,
    __SPV_BitReverse = 204, __SPV_BitCount = 205, __SPV_DPdx = 207, __SPV_DPdy = 208, __SPV_Fwidth = 209,
    __SPV_DPdxFine = 210, __SPV_DPdyFine = 211, __SPV_FwidthFine = 212,
    __SPV_DPdxCoarse = 213, __SPV_DPdyCoarse = 214, __SPV_FwidthCoarse = 215,
};

// Raw value (scalar or ext vector) of a scalar or vec<T, N>.
template<typename T> constexpr T __raw(T x) { return x; }
template<typename T, int N> constexpr __ext<T, N> __raw(vec<T, N> x) { return x.v; }
template<typename T> struct __float_like : bool_constant<is_floating_point_v<__scalar_t<T>>> {};

// ---- one-argument float functions -------------------------------------------------
#define __MSL_F1(NAME, OP) \
    inline float NAME(float x) { return __msl_glsl1<float>(OP, x); } \
    inline half NAME(half x) { return __msl_glsl1<half>(OP, x); } \
    template<int N> inline vec<float, N> NAME(vec<float, N> x) { return __msl_glsl1<__ext<float, N>>(OP, x.v); } \
    template<int N> inline vec<half, N> NAME(vec<half, N> x) { return __msl_glsl1<__ext<half, N>>(OP, x.v); }
__MSL_F1(sin, __GLSL_Sin) __MSL_F1(cos, __GLSL_Cos) __MSL_F1(tan, __GLSL_Tan)
__MSL_F1(asin, __GLSL_Asin) __MSL_F1(acos, __GLSL_Acos) __MSL_F1(atan, __GLSL_Atan)
__MSL_F1(sinh, __GLSL_Sinh) __MSL_F1(cosh, __GLSL_Cosh) __MSL_F1(tanh, __GLSL_Tanh)
__MSL_F1(asinh, __GLSL_Asinh) __MSL_F1(acosh, __GLSL_Acosh) __MSL_F1(atanh, __GLSL_Atanh)
__MSL_F1(exp, __GLSL_Exp) __MSL_F1(exp2, __GLSL_Exp2) __MSL_F1(log, __GLSL_Log) __MSL_F1(log2, __GLSL_Log2)
__MSL_F1(sqrt, __GLSL_Sqrt) __MSL_F1(rsqrt, __GLSL_InverseSqrt)
__MSL_F1(floor, __GLSL_Floor) __MSL_F1(ceil, __GLSL_Ceil) __MSL_F1(trunc, __GLSL_Trunc)
__MSL_F1(round, __GLSL_Round) __MSL_F1(rint, __GLSL_RoundEven) __MSL_F1(fract, __GLSL_Fract)
__MSL_F1(fabs, __GLSL_FAbs)
#undef __MSL_F1

// ---- float functions written in terms of others ------------------------------------
#define __MSL_FDERIVED1(NAME, EXPR) \
    inline float NAME(float x) { return EXPR; } \
    inline half NAME(half x) { return EXPR; } \
    template<int N> inline vec<float, N> NAME(vec<float, N> x) { return EXPR; } \
    template<int N> inline vec<half, N> NAME(vec<half, N> x) { return EXPR; }
__MSL_FDERIVED1(exp10, exp2(x * decltype(x)(3.32192809488736234787f)))
__MSL_FDERIVED1(log10, log2(x) * decltype(x)(0.30102999566398119521f))
__MSL_FDERIVED1(sinpi, sin(x * decltype(x)(3.14159265358979323846f)))
__MSL_FDERIVED1(cospi, cos(x * decltype(x)(3.14159265358979323846f)))
__MSL_FDERIVED1(tanpi, tan(x * decltype(x)(3.14159265358979323846f)))
__MSL_FDERIVED1(asinpi, asin(x) * decltype(x)(0.31830988618379067154f))
__MSL_FDERIVED1(acospi, acos(x) * decltype(x)(0.31830988618379067154f))
__MSL_FDERIVED1(atanpi, atan(x) * decltype(x)(0.31830988618379067154f))
__MSL_FDERIVED1(saturate, __msl_glsl3<decltype(__raw(x))>(__GLSL_FClamp, __raw(x),
    __raw(decltype(x)(0.0f)), __raw(decltype(x)(1.0f))))
#undef __MSL_FDERIVED1

// ---- two/three-argument float functions --------------------------------------------
#define __MSL_F2(NAME, OP) \
    inline float NAME(float x, float y) { return __msl_glsl2<float>(OP, x, y); } \
    inline half NAME(half x, half y) { return __msl_glsl2<half>(OP, x, y); } \
    template<int N> inline vec<float, N> NAME(vec<float, N> x, type_identity_t<vec<float, N>> y) { \
        return __msl_glsl2<__ext<float, N>>(OP, x.v, y.v); } \
    template<int N> inline vec<half, N> NAME(vec<half, N> x, type_identity_t<vec<half, N>> y) { \
        return __msl_glsl2<__ext<half, N>>(OP, x.v, y.v); }
__MSL_F2(atan2, __GLSL_Atan2) __MSL_F2(pow, __GLSL_Pow) __MSL_F2(powr, __GLSL_Pow)
__MSL_F2(fmin, __GLSL_FMin) __MSL_F2(fmax, __GLSL_FMax) __MSL_F2(step, __GLSL_Step)
#undef __MSL_F2
#define __MSL_F3(NAME, OP) \
    inline float NAME(float x, float y, float z) { return __msl_glsl3<float>(OP, x, y, z); } \
    inline half NAME(half x, half y, half z) { return __msl_glsl3<half>(OP, x, y, z); } \
    template<int N> inline vec<float, N> NAME(vec<float, N> x, type_identity_t<vec<float, N>> y, \
        type_identity_t<vec<float, N>> z) { return __msl_glsl3<__ext<float, N>>(OP, x.v, y.v, z.v); } \
    template<int N> inline vec<half, N> NAME(vec<half, N> x, type_identity_t<vec<half, N>> y, \
        type_identity_t<vec<half, N>> z) { return __msl_glsl3<__ext<half, N>>(OP, x.v, y.v, z.v); }
__MSL_F3(fma, __GLSL_Fma) __MSL_F3(mad, __GLSL_Fma) __MSL_F3(smoothstep, __GLSL_SmoothStep)
#undef __MSL_F3
// Scalar edges with a vector x: Tn step(T edge, Tn x), Tn smoothstep(T, T, Tn).
#define __MSL_EDGES(T) \
    template<int N> inline vec<T, N> step(T edge, vec<T, N> x) { return step(vec<T, N>(edge), x); } \
    template<int N> inline vec<T, N> smoothstep(T edge0, T edge1, vec<T, N> x) { \
        return smoothstep(vec<T, N>(edge0), vec<T, N>(edge1), x); }
__MSL_EDGES(float) __MSL_EDGES(half)
#undef __MSL_EDGES

inline float fmod(float x, float y) { return __msl_spv2<float>(__SPV_FRem, x, y); }
inline half fmod(half x, half y) { return __msl_spv2<half>(__SPV_FRem, x, y); }
template<int N> inline vec<float, N> fmod(vec<float, N> x, type_identity_t<vec<float, N>> y) {
    return __msl_spv2<__ext<float, N>>(__SPV_FRem, x.v, y.v); }
template<int N> inline vec<half, N> fmod(vec<half, N> x, type_identity_t<vec<half, N>> y) {
    return __msl_spv2<__ext<half, N>>(__SPV_FRem, x.v, y.v); }
template<typename T, enable_if_t<__float_like<T>::value, int> = 0> inline T fdim(T x, type_identity_t<T> y) {
    return fmax(x - y, T(0)); }
template<typename T, enable_if_t<__float_like<T>::value, int> = 0> inline T copysign(T x, type_identity_t<T> y) {
    return __builtin_elementwise_copysign(__raw(x), __raw(y)); }
template<typename T, enable_if_t<__float_like<T>::value, int> = 0> inline T modf(T x, type_identity_t<T>& ip) {
    ip = trunc(x); return x - ip; }
template<typename T, enable_if_t<__float_like<T>::value, int> = 0> inline T divide(T x, type_identity_t<T> y) {
    return x / y; }
template<typename T, enable_if_t<__float_like<T>::value, int> = 0> inline T sincos(T x, type_identity_t<T>& c) {
    c = cos(x); return sin(x); }

inline float ldexp(float x, int e) { return __msl_glsl2<float>(__GLSL_Ldexp, x, e); }
template<int N> inline vec<float, N> ldexp(vec<float, N> x, vec<int, N> e) {
    return __msl_glsl2<__ext<float, N>>(__GLSL_Ldexp, x.v, e.v); }
inline float frexp(float x, int& e) {
    if (x == 0.0f) { e = 0; return x; }
    const uint bits = __builtin_bit_cast(uint, x);
    e = int((bits >> 23) & 0xffu) - 126;
    return __builtin_bit_cast(float, (bits & 0x807fffffu) | 0x3f000000u);
}

// ---- abs/min/max/clamp/mix/sign (float and integer) ----------------------------
template<typename T> constexpr uint __pick(uint f, uint s, uint u) {
    return is_floating_point_v<__scalar_t<T>> ? f : is_signed_v<__scalar_t<T>> ? s : u;
}
template<typename T, enable_if_t<is_arithmetic_v<T>, int> = 0> inline T abs(T x) {
    if constexpr (is_unsigned_v<T>) return x;
    else return __msl_glsl1<T>(__pick<T>(__GLSL_FAbs, __GLSL_SAbs, 0), x);
}
template<typename T, int N> inline vec<T, N> abs(vec<T, N> x) {
    if constexpr (is_unsigned_v<T>) return x;
    else return __msl_glsl1<__ext<T, N>>(__pick<T>(__GLSL_FAbs, __GLSL_SAbs, 0), x.v);
}
template<typename T, enable_if_t<is_arithmetic_v<T>, int> = 0> inline T min(T x, T y) {
    return __msl_glsl2<T>(__pick<T>(__GLSL_FMin, __GLSL_SMin, __GLSL_UMin), x, y); }
template<typename T, enable_if_t<is_arithmetic_v<T>, int> = 0> inline T max(T x, T y) {
    return __msl_glsl2<T>(__pick<T>(__GLSL_FMax, __GLSL_SMax, __GLSL_UMax), x, y); }
template<typename T, enable_if_t<is_arithmetic_v<T>, int> = 0> inline T clamp(T x, T lo, T hi) {
    return __msl_glsl3<T>(__pick<T>(__GLSL_FClamp, __GLSL_SClamp, __GLSL_UClamp), x, lo, hi); }
template<typename T, int N> inline vec<T, N> min(vec<T, N> x, type_identity_t<vec<T, N>> y) {
    return __msl_glsl2<__ext<T, N>>(__pick<T>(__GLSL_FMin, __GLSL_SMin, __GLSL_UMin), x.v, y.v); }
template<typename T, int N> inline vec<T, N> max(vec<T, N> x, type_identity_t<vec<T, N>> y) {
    return __msl_glsl2<__ext<T, N>>(__pick<T>(__GLSL_FMax, __GLSL_SMax, __GLSL_UMax), x.v, y.v); }
template<typename T, int N> inline vec<T, N> clamp(vec<T, N> x, type_identity_t<vec<T, N>> lo,
    type_identity_t<vec<T, N>> hi) {
    return __msl_glsl3<__ext<T, N>>(__pick<T>(__GLSL_FClamp, __GLSL_SClamp, __GLSL_UClamp), x.v, lo.v, hi.v); }
template<typename T, enable_if_t<is_floating_point_v<T>, int> = 0> inline T mix(T x, T y, T a) {
    return __msl_glsl3<T>(__GLSL_FMix, x, y, a); }
template<typename T, int N, enable_if_t<is_floating_point_v<T>, int> = 0>
inline vec<T, N> mix(vec<T, N> x, type_identity_t<vec<T, N>> y, type_identity_t<vec<T, N>> a) {
    return __msl_glsl3<__ext<T, N>>(__GLSL_FMix, x.v, y.v, a.v); }
template<typename T, enable_if_t<is_arithmetic_v<T>, int> = 0> inline T sign(T x) {
    return __msl_glsl1<T>(__pick<T>(__GLSL_FSign, __GLSL_SSign, __GLSL_SSign), x); }
template<typename T, int N> inline vec<T, N> sign(vec<T, N> x) {
    return __msl_glsl1<__ext<T, N>>(__pick<T>(__GLSL_FSign, __GLSL_SSign, __GLSL_SSign), x.v); }
template<typename T, enable_if_t<is_integral_v<__scalar_t<T>>, int> = 0> inline T absdiff(T x, T y) {
    return max(x, y) - min(x, y); }

// ---- relational ---------------------------------------------------------------------
inline bool all(bool x) { return x; }
inline bool any(bool x) { return x; }
template<int N> inline bool all(vec<bool, N> x) {
    bool r = x.v[0] != 0;
    for (int i = 1; i < N; ++i) r = r && x.v[i] != 0;
    return r;
}
template<int N> inline bool any(vec<bool, N> x) {
    bool r = x.v[0] != 0;
    for (int i = 1; i < N; ++i) r = r || x.v[i] != 0;
    return r;
}
template<typename T, enable_if_t<is_scalar_v<T>, int> = 0> inline T select(T a, T b, bool c) { return c ? b : a; }
template<typename T, int N> inline vec<T, N> select(vec<T, N> a, vec<T, N> b, bool c) { return c ? b : a; }
template<typename T, int N> inline vec<T, N> select(vec<T, N> a, vec<T, N> b, vec<bool, N> c) {
    vec<T, N> r = a;
    for (int i = 0; i < N; ++i) if (c.v[i]) r[i] = b[i];
    return r;
}
#define __MSL_FCLASS(NAME, OP) \
    inline bool NAME(float x) { return __msl_spvb1<bool>(OP, x); } \
    inline bool NAME(half x) { return __msl_spvb1<bool>(OP, x); } \
    template<typename T, int N> inline vec<bool, N> NAME(vec<T, N> x) { \
        return vec<bool, N>(__msl_spvb1<__ext<bool, N>>(OP, x.v)); }
__MSL_FCLASS(isnan, __SPV_IsNan)
__MSL_FCLASS(isinf, __SPV_IsInf)
#undef __MSL_FCLASS
template<typename T> inline auto isfinite(T x) { return !(isnan(x) || isinf(x)); }
template<typename T, int N> inline vec<bool, N> isfinite(vec<T, N> x) { return !(isnan(x) || isinf(x)); }
inline bool isordered(float x, float y) { return !isnan(x) && !isnan(y); }
inline bool isunordered(float x, float y) { return isnan(x) || isnan(y); }
inline bool signbit(float x) { return (__builtin_bit_cast(uint, x) >> 31) != 0u; }
template<int N> inline vec<bool, N> signbit(vec<float, N> x) {
    return vec<bool, N>(__builtin_bit_cast(vec<uint, N>, x) >> 31u); }
inline bool isnormal(float x) {
    const uint e = (__builtin_bit_cast(uint, x) >> 23) & 0xffu;
    return e != 0u && e != 0xffu;
}

// ---- integer functions --------------------------------------------------------------
template<typename T, enable_if_t<is_integral_v<__scalar_t<T>>, int> = 0> inline T popcount(T x) {
    return __msl_spv1<decltype(__raw(x))>(__SPV_BitCount, __raw(x)); }
template<typename T, enable_if_t<is_integral_v<__scalar_t<T>>, int> = 0> inline T reverse_bits(T x) {
    return __msl_spv1<decltype(__raw(x))>(__SPV_BitReverse, __raw(x)); }
template<typename T, enable_if_t<is_integral_v<__scalar_t<T>>, int> = 0> inline T clz(T x) {
    using S = __scalar_t<T>;
    const T msb = T(__msl_glsl1<decltype(__raw(x))>(__GLSL_FindUMsb, __raw(x)));
    return T(S(sizeof(S) * 8 - 1)) - msb;   // FindUMsb(0) = -1 gives the full width
}
template<typename T, enable_if_t<is_integral_v<__scalar_t<T>>, int> = 0> inline T ctz(T x) {
    using S = __scalar_t<T>;
    const T lsb = T(__msl_glsl1<decltype(__raw(x))>(__GLSL_FindILsb, __raw(x)));
    return select(lsb, T(S(sizeof(S) * 8)), x == T(0));
}
template<typename T, enable_if_t<is_integral_v<__scalar_t<T>>, int> = 0>
inline T extract_bits(T x, uint offset, uint bits) {
    return __msl_spv3<decltype(__raw(x))>(is_signed_v<__scalar_t<T>> ? __SPV_BitFieldSExtract
        : __SPV_BitFieldUExtract, __raw(x), offset, bits); }
template<typename T, enable_if_t<is_integral_v<__scalar_t<T>>, int> = 0>
inline T insert_bits(T base, T insert, uint offset, uint bits) {
    return __msl_spv4<decltype(__raw(base))>(__SPV_BitFieldInsert, __raw(base), __raw(insert), offset, bits); }
template<typename T, enable_if_t<is_integral_v<__scalar_t<T>>, int> = 0> inline T rotate(T v, T i) {
    using S = __scalar_t<T>;
    const T width = T(S(sizeof(S) * 8));
    const T n = i % width;
    return (v << n) | (v >> ((width - n) % width));
}
inline uint mulhi(uint a, uint b) { return uint((ulong(a) * ulong(b)) >> 32); }
inline int mulhi(int a, int b) { return int((long(a) * long(b)) >> 32); }
template<typename T> inline T mad24(T a, T b, T c) { return a * b + c; }
template<typename T> inline T mul24(T a, T b) { return a * b; }

// ---- geometric ------------------------------------------------------------------------
template<typename T, int N, enable_if_t<is_floating_point_v<T>, int> = 0> inline T dot(vec<T, N> x, vec<T, N> y) {
    return __msl_spv2<T>(__SPV_Dot, x.v, y.v); }
template<typename T, int N, enable_if_t<is_floating_point_v<T>, int> = 0> inline T length(vec<T, N> x) {
    return __msl_glsl1<T>(__GLSL_Length, x.v); }
template<typename T, int N, enable_if_t<is_floating_point_v<T>, int> = 0> inline T length_squared(vec<T, N> x) {
    return dot(x, x); }
template<typename T, int N, enable_if_t<is_floating_point_v<T>, int> = 0>
inline T distance(vec<T, N> x, vec<T, N> y) { return __msl_glsl2<T>(__GLSL_Distance, x.v, y.v); }
template<typename T, int N, enable_if_t<is_floating_point_v<T>, int> = 0>
inline T distance_squared(vec<T, N> x, vec<T, N> y) { return dot(x - y, x - y); }
template<typename T, int N, enable_if_t<is_floating_point_v<T>, int> = 0> inline vec<T, N> normalize(vec<T, N> x) {
    return __msl_glsl1<__ext<T, N>>(__GLSL_Normalize, x.v); }
template<typename T, enable_if_t<is_floating_point_v<T>, int> = 0> inline vec<T, 3> cross(vec<T, 3> x, vec<T, 3> y) {
    return __msl_glsl2<__ext<T, 3>>(__GLSL_Cross, x.v, y.v); }
template<typename T, int N, enable_if_t<is_floating_point_v<T>, int> = 0>
inline vec<T, N> faceforward(vec<T, N> n, vec<T, N> i, vec<T, N> nref) {
    return __msl_glsl3<__ext<T, N>>(__GLSL_FaceForward, n.v, i.v, nref.v); }
template<typename T, int N, enable_if_t<is_floating_point_v<T>, int> = 0>
inline vec<T, N> reflect(vec<T, N> i, vec<T, N> n) { return __msl_glsl2<__ext<T, N>>(__GLSL_Reflect, i.v, n.v); }
template<typename T, int N, enable_if_t<is_floating_point_v<T>, int> = 0>
inline vec<T, N> refract(vec<T, N> i, vec<T, N> n, T eta) {
    return __msl_glsl3<__ext<T, N>>(__GLSL_Refract, i.v, n.v, eta); }

// ---- reinterpretation ----------------------------------------------------------------
template<typename To, typename From> inline To as_type(From x) {
    static_assert(sizeof(To) == sizeof(From), "as_type requires types of equal size");
    return __builtin_bit_cast(To, x);
}

// ---- pack / unpack -------------------------------------------------------------------
inline uint pack_float_to_unorm4x8(float4 x) { return __msl_glsl1<uint>(__GLSL_PackUnorm4x8, x.v); }
inline uint pack_float_to_snorm4x8(float4 x) { return __msl_glsl1<uint>(__GLSL_PackSnorm4x8, x.v); }
inline uint pack_float_to_unorm2x16(float2 x) { return __msl_glsl1<uint>(__GLSL_PackUnorm2x16, x.v); }
inline uint pack_float_to_snorm2x16(float2 x) { return __msl_glsl1<uint>(__GLSL_PackSnorm2x16, x.v); }
inline uint pack_half_to_unorm4x8(half4 x) { return pack_float_to_unorm4x8(float4(x)); }
inline uint pack_half_to_snorm4x8(half4 x) { return pack_float_to_snorm4x8(float4(x)); }
inline uint pack_half_to_unorm2x16(half2 x) { return pack_float_to_unorm2x16(float2(x)); }
inline uint pack_half_to_snorm2x16(half2 x) { return pack_float_to_snorm2x16(float2(x)); }
inline float4 unpack_unorm4x8_to_float(uint x) { return __msl_glsl1<__ext<float, 4>>(__GLSL_UnpackUnorm4x8, x); }
inline float4 unpack_snorm4x8_to_float(uint x) { return __msl_glsl1<__ext<float, 4>>(__GLSL_UnpackSnorm4x8, x); }
inline float2 unpack_unorm2x16_to_float(uint x) { return __msl_glsl1<__ext<float, 2>>(__GLSL_UnpackUnorm2x16, x); }
inline float2 unpack_snorm2x16_to_float(uint x) { return __msl_glsl1<__ext<float, 2>>(__GLSL_UnpackSnorm2x16, x); }
inline half4 unpack_unorm4x8_to_half(uint x) { return half4(unpack_unorm4x8_to_float(x)); }
inline half4 unpack_snorm4x8_to_half(uint x) { return half4(unpack_snorm4x8_to_float(x)); }
inline half2 unpack_unorm2x16_to_half(uint x) { return half2(unpack_unorm2x16_to_float(x)); }
inline half2 unpack_snorm2x16_to_half(uint x) { return half2(unpack_snorm2x16_to_float(x)); }

// ---- matrix functions ------------------------------------------------------------------
template<typename T, int C, int R> inline matrix<T, R, C> transpose(matrix<T, C, R> m) {
    matrix<T, R, C> r;
    for (int c = 0; c < C; ++c)
        for (int k = 0; k < R; ++k) r.columns[k][c] = m.columns[c][k];
    return r;
}
template<typename T> inline T determinant(matrix<T, 2, 2> m) { return m[0][0] * m[1][1] - m[1][0] * m[0][1]; }
template<typename T> inline T determinant(matrix<T, 3, 3> m) {
    return m[0][0] * (m[1][1] * m[2][2] - m[2][1] * m[1][2]) - m[1][0] * (m[0][1] * m[2][2] - m[2][1] * m[0][2]) +
           m[2][0] * (m[0][1] * m[1][2] - m[1][1] * m[0][2]);
}
template<typename T> inline T determinant(matrix<T, 4, 4> m) {
    const T s0 = m[0][0] * m[1][1] - m[1][0] * m[0][1], s1 = m[0][0] * m[1][2] - m[1][0] * m[0][2];
    const T s2 = m[0][0] * m[1][3] - m[1][0] * m[0][3], s3 = m[0][1] * m[1][2] - m[1][1] * m[0][2];
    const T s4 = m[0][1] * m[1][3] - m[1][1] * m[0][3], s5 = m[0][2] * m[1][3] - m[1][2] * m[0][3];
    const T c5 = m[2][2] * m[3][3] - m[3][2] * m[2][3], c4 = m[2][1] * m[3][3] - m[3][1] * m[2][3];
    const T c3 = m[2][1] * m[3][2] - m[3][1] * m[2][2], c2 = m[2][0] * m[3][3] - m[3][0] * m[2][3];
    const T c1 = m[2][0] * m[3][2] - m[3][0] * m[2][2], c0 = m[2][0] * m[3][1] - m[3][0] * m[2][1];
    return s0 * c5 - s1 * c4 + s2 * c3 + s3 * c2 - s4 * c1 + s5 * c0;
}

namespace fast { using namespace metal; }
namespace precise { using namespace metal; }

} // namespace metal
