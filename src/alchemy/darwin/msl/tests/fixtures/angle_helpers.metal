// The helpers ANGLE's GLSL-to-MSL translator puts ahead of every WebGL program
// (design: ANGLE's public translator): integer arithmetic with GLSL's wrapping
// done through as_type<> in templates written before any `using namespace
// metal` (Apple's compiler takes as_type unqualified), the sample count
// its alpha-to-coverage emulation reads with metal::get_num_samples(), and
// float-to-int conversions clamped against numeric_limits of a vector type
// (texelFetch(t, ivec2(gl_FragCoord.xy), 0)).
#include <metal_stdlib>

#define ANGLE_ALWAYS_INLINE __attribute__((always_inline))

template <typename X, typename Y>
ANGLE_ALWAYS_INLINE X ANGLE_shiftLeft(X x, Y y)
{
    using UX = metal::make_unsigned_t<X>;
    return as_type<X>(UX(x) << (y & Y(31)));
}

template <typename X, typename Y, typename Z = metal::conditional_t<metal::is_scalar_v<Y>, X, Y>>
ANGLE_ALWAYS_INLINE Z ANGLE_wrappingAdd(X x, Y y)
{
    using U = metal::make_unsigned_t<Z>;
    return as_type<Z>(U(Z(x)) + U(Z(y)));
}

template <typename X, typename Y, typename Z = metal::conditional_t<metal::is_scalar_v<Y>, X, Y>>
ANGLE_ALWAYS_INLINE Z ANGLE_wrappingMultiply(X x, Y y)
{
    using U = metal::make_unsigned_t<Z>;
    return as_type<Z>(U(Z(x)) * U(Z(y)));
}

template <typename X, typename Y>
ANGLE_ALWAYS_INLINE X ANGLE_ftoi(Y y)
{
    Y min = Y(metal::numeric_limits<X>::min());
    Y max = Y(metal::numeric_limits<X>::max()) * Y(1.0f - 0x1.0p-24f);
    return X(metal::clamp(y, min, max));
}

using namespace metal;

struct Uniforms { int a; int b; float alpha; };
struct FragmentOut { float4 color [[color(0)]]; uint mask [[sample_mask]]; };

fragment FragmentOut helpers_main(constant Uniforms& u [[buffer(0)]], float4 position [[position]])
{
    FragmentOut out;
    const int2 texel = ANGLE_ftoi<int2>(position.xy);
    const int clamped = ANGLE_ftoi<int>(1.0e12f);
    const int shifted = ANGLE_shiftLeft(u.a, 3);
    const int2 summed = ANGLE_wrappingAdd(int2(u.a, u.b), int2(shifted, 1));
    const int product = ANGLE_wrappingMultiply(summed.x, summed.y);
    out.color = float4(float(product & 255) / 255.0f, float(texel.x + texel.y) / 255.0f, float(clamped > 0), u.alpha);
    // Alpha to coverage: a mask of round(alpha * samples) bits.
    const uint samples = metal::get_num_samples();
    const uint covered = uint(metal::saturate(u.alpha) * float(samples) + 0.5f);
    out.mask = samples == 1 ? 1u : ((1u << covered) - 1u);
    return out;
}
