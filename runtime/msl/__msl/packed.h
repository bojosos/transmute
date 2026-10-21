// Alchemy metal_stdlib: packed vector arguments for the standard library
// functions. Project-authored (MIT).
// MSL's packed vectors convert implicitly to vectors wherever a vector is
// expected, standard library calls included (`abs(closure.color)` with a
// packed_float3 member). Most functions here are templates, which never
// convert their arguments, so each name gets an overload taking any argument
// list with a packed vector that unpacks it and calls the vector overload.
#pragma once

namespace metal {

template<typename T> constexpr T __msl_unpack(T x) { return x; }
template<typename T, int N> constexpr vec<T, N> __msl_unpack(packed_vec<T, N> x) { return vec<T, N>(x); }
template<typename... A> struct __msl_any_packed : bool_constant<(__is_packed<remove_cv_t<A>>::value || ...)> {};

#define __MSL_PACKED_FORWARD(NAME) \
    template<typename... A, enable_if_t<__msl_any_packed<A...>::value, int> = 0> \
    inline auto NAME(A... a) -> decltype(NAME(__msl_unpack(a)...)) { return NAME(__msl_unpack(a)...); }

__MSL_PACKED_FORWARD(abs) __MSL_PACKED_FORWARD(fabs) __MSL_PACKED_FORWARD(sign) __MSL_PACKED_FORWARD(min)
__MSL_PACKED_FORWARD(max) __MSL_PACKED_FORWARD(clamp) __MSL_PACKED_FORWARD(mix) __MSL_PACKED_FORWARD(saturate)
__MSL_PACKED_FORWARD(step) __MSL_PACKED_FORWARD(smoothstep) __MSL_PACKED_FORWARD(fmin) __MSL_PACKED_FORWARD(fmax)
__MSL_PACKED_FORWARD(fma) __MSL_PACKED_FORWARD(mad) __MSL_PACKED_FORWARD(fmod) __MSL_PACKED_FORWARD(copysign)
__MSL_PACKED_FORWARD(fdim) __MSL_PACKED_FORWARD(select)
__MSL_PACKED_FORWARD(sin) __MSL_PACKED_FORWARD(cos) __MSL_PACKED_FORWARD(tan) __MSL_PACKED_FORWARD(asin)
__MSL_PACKED_FORWARD(acos) __MSL_PACKED_FORWARD(atan) __MSL_PACKED_FORWARD(atan2) __MSL_PACKED_FORWARD(sinh)
__MSL_PACKED_FORWARD(cosh) __MSL_PACKED_FORWARD(tanh) __MSL_PACKED_FORWARD(asinh) __MSL_PACKED_FORWARD(acosh)
__MSL_PACKED_FORWARD(atanh) __MSL_PACKED_FORWARD(exp) __MSL_PACKED_FORWARD(exp2) __MSL_PACKED_FORWARD(exp10)
__MSL_PACKED_FORWARD(log) __MSL_PACKED_FORWARD(log2) __MSL_PACKED_FORWARD(log10) __MSL_PACKED_FORWARD(pow)
__MSL_PACKED_FORWARD(powr) __MSL_PACKED_FORWARD(sqrt) __MSL_PACKED_FORWARD(rsqrt) __MSL_PACKED_FORWARD(floor)
__MSL_PACKED_FORWARD(ceil) __MSL_PACKED_FORWARD(trunc) __MSL_PACKED_FORWARD(round) __MSL_PACKED_FORWARD(rint)
__MSL_PACKED_FORWARD(fract) __MSL_PACKED_FORWARD(isnan) __MSL_PACKED_FORWARD(isinf)
__MSL_PACKED_FORWARD(isfinite)
__MSL_PACKED_FORWARD(dot) __MSL_PACKED_FORWARD(cross) __MSL_PACKED_FORWARD(length)
__MSL_PACKED_FORWARD(length_squared) __MSL_PACKED_FORWARD(distance) __MSL_PACKED_FORWARD(distance_squared)
__MSL_PACKED_FORWARD(normalize) __MSL_PACKED_FORWARD(reflect) __MSL_PACKED_FORWARD(refract)
__MSL_PACKED_FORWARD(faceforward)
__MSL_PACKED_FORWARD(all) __MSL_PACKED_FORWARD(any)
__MSL_PACKED_FORWARD(dfdx) __MSL_PACKED_FORWARD(dfdy) __MSL_PACKED_FORWARD(fwidth)

#undef __MSL_PACKED_FORWARD

} // namespace metal
