// A WebGL program in the style ANGLE's GLSL-to-MSL translator emits for its
// Metal back end (design: ANGLE's public translator and Metal back end):
// ANGLE's function constants (some derived from others), outputs and
// arguments guarded by [[function_constant]], a vertex entry whose return
// type comes from a preprocessor macro (set through MTLCompileOptions), the
// driver-uniform struct and user uniforms passed by constant reference,
// textures and samplers gathered in a texture environment of thread
// pointers, GLSL helpers (mod, flips, depth-range and alpha-to-coverage
// emulation) and discard.
#include <metal_stdlib>
#include <simd/simd.h>
using namespace metal;

#pragma clang diagnostic ignored "-Wunused-value"
#define ANGLE_ALWAYS_INLINE __attribute__((always_inline))

#ifndef ANGLE_TRANSFORM_FEEDBACK_ENABLED
#define ANGLE_TRANSFORM_FEEDBACK_ENABLED 0
#endif
#if ANGLE_TRANSFORM_FEEDBACK_ENABLED
#define __VERTEX_OUT(args) void
#else
#define __VERTEX_OUT(args) args
#endif

constant bool ANGLEUseSampleCompareGradient [[function_constant(0)]];
constant bool ANGLEUseSampleCompareLod [[function_constant(1)]];
constant bool ANGLERasterizerDisabled [[function_constant(2)]];
constant bool ANGLEMultisampledRendering [[function_constant(3)]];
constant bool ANGLEDepthWriteEnabled [[function_constant(4)]];
constant bool ANGLEEmulateAlphaToCoverage [[function_constant(5)]];
constant bool ANGLEWriteHelperSampleMask [[function_constant(6)]];
constant bool ANGLESampleMaskWriteEnabled = ANGLEMultisampledRendering || ANGLEWriteHelperSampleMask;

template <typename T>
struct ANGLE_TextureEnv
{
    thread T *texture;
    thread metal::sampler *sampler;
};

template <typename T>
ANGLE_ALWAYS_INLINE thread T * ANGLE_addressof(thread T &ref)
{
    return &ref;
}

template <typename X, typename Y>
ANGLE_ALWAYS_INLINE X ANGLE_mod(X x, Y y)
{
    return x - y * metal::floor(x / y);
}

template <typename T>
ANGLE_ALWAYS_INLINE metal::vec<T, 4> ANGLE_texture(thread ANGLE_TextureEnv<metal::texture2d<T>> &env, metal::float2 const coord)
{
    return env.texture->sample(*env.sampler, coord);
}

template <typename T>
ANGLE_ALWAYS_INLINE metal::vec<T, 4> ANGLE_texture(thread ANGLE_TextureEnv<metal::texture2d<T>> &env, metal::float2 const coord, float bias)
{
    return env.texture->sample(*env.sampler, coord, metal::bias(bias));
}

template <typename T>
ANGLE_ALWAYS_INLINE metal::vec<T, 4> ANGLE_textureLod(thread ANGLE_TextureEnv<metal::texturecube<T>> &env, metal::float3 const coord, float level)
{
    return env.texture->sample(*env.sampler, coord, metal::level(level));
}

template <typename T>
ANGLE_ALWAYS_INLINE metal::vec<T, 4> ANGLE_texelFetch(thread ANGLE_TextureEnv<metal::texture2d<T>> &env, metal::int2 const coord, int level)
{
    return env.texture->read(uint2(coord), uint32_t(level));
}

template <typename T>
ANGLE_ALWAYS_INLINE metal::int2 ANGLE_textureSize(thread ANGLE_TextureEnv<metal::texture2d<T>> &env, int level)
{
    return int2(env.texture->get_width(uint32_t(level)), env.texture->get_height(uint32_t(level)));
}

template <typename T>
ANGLE_ALWAYS_INLINE float ANGLE_shadow(thread ANGLE_TextureEnv<metal::depth2d<T>> &env, metal::float3 const coord)
{
    return env.texture->sample_compare(*env.sampler, coord.xy, coord.z);
}

struct ANGLE_UniformsStruct
{
    metal::uint2 ANGLE_acbBufferOffsets;
    metal::float2 ANGLE_depthRange;
    uint32_t ANGLE_renderArea;
    uint32_t ANGLE_flipXY;
    uint32_t ANGLE_dither;
    uint32_t ANGLE_misc;
    metal::int4 ANGLE_xfbBufferOffsets;
    int ANGLE_xfbVerticesPerInstance;
    uint32_t ANGLE_coverageMask;
    metal::uint2 ANGLE_unused;
};

struct ANGLE_UserUniforms
{
    metal::float4x4 _uMVP;
    metal::float4 _uColor;
    metal::array<metal::float4, 4> _uOffsets;
    metal::float2 _uScale;
    int _uMode;
    float _uAlphaRef;
};

struct ANGLE_TextureEnvs
{
    ANGLE_TextureEnv<metal::texture2d<float>> _uTex;
    ANGLE_TextureEnv<metal::texturecube<float>> _uEnv;
    ANGLE_TextureEnv<metal::depth2d<float>> _uShadow;
};

struct ANGLE_VertexOut
{
    metal::float4 _uvColor [[user(_uvColor)]];
    metal::float2 _uvTexCoord [[user(_uvTexCoord)]];
    metal::float3 _uvNormal [[user(_uvNormal)]];
    metal::float4 gl_Position [[position]];
    float gl_PointSize [[point_size]];
};

struct ANGLE_VertexIn
{
    metal::float3 _uaPosition [[attribute(0)]];
    metal::float2 _uaTexCoord [[attribute(1)]];
    metal::float4 _uaColor [[attribute(2)]];
};

struct ANGLE_FragmentIn
{
    metal::float4 _uvColor [[user(_uvColor)]];
    metal::float2 _uvTexCoord [[user(_uvTexCoord)]];
    metal::float3 _uvNormal [[user(_uvNormal)]];
};

struct ANGLE_FragmentOut
{
    metal::float4 _ufragColor [[color(0)]];
    uint32_t ANGLE_metal_SampleMask [[sample_mask, function_constant(ANGLESampleMaskWriteEnabled)]];
    float gl_FragDepth [[depth(any), function_constant(ANGLEDepthWriteEnabled)]];
};

ANGLE_ALWAYS_INLINE metal::float2 ANGLE_flipXY(constant ANGLE_UniformsStruct &ANGLE_angleUniforms)
{
    return metal::unpack_snorm4x8_to_float(ANGLE_angleUniforms.ANGLE_flipXY).xy;
}

ANGLE_ALWAYS_INLINE uint32_t ANGLE_alphaToCoverage(float alpha, uint32_t sampleCount)
{
    uint32_t bits = uint32_t(metal::clamp(alpha, 0.0f, 1.0f) * float(sampleCount) + 0.5f);
    return (1u << bits) - 1u;
}

void _umain(thread ANGLE_VertexOut &ANGLE_vertexOut, thread ANGLE_VertexIn &ANGLE_vertexIn, constant ANGLE_UserUniforms &ANGLE_userUniforms, uint32_t gl_VertexID)
{
    int _ui = metal::clamp(int(gl_VertexID) % 4, 0, 3);
    ANGLE_vertexOut.gl_Position = (ANGLE_userUniforms._uMVP * metal::float4((ANGLE_vertexIn._uaPosition + ANGLE_userUniforms._uOffsets[_ui].xyz), 1.0f));
    ANGLE_vertexOut._uvTexCoord = (ANGLE_vertexIn._uaTexCoord * ANGLE_userUniforms._uScale);
    ANGLE_vertexOut._uvColor = (ANGLE_vertexIn._uaColor * ANGLE_userUniforms._uColor);
    ANGLE_vertexOut._uvNormal = metal::normalize(ANGLE_vertexIn._uaPosition);
    ANGLE_vertexOut.gl_PointSize = 1.0f;
    if (ANGLE_userUniforms._uMode == 1)
    {
        ANGLE_vertexOut._uvTexCoord = ANGLE_mod(ANGLE_vertexOut._uvTexCoord, 1.0f);
    }
}

vertex __VERTEX_OUT(ANGLE_VertexOut) main0(ANGLE_VertexIn ANGLE_vertexIn [[stage_in]],
    constant ANGLE_UniformsStruct & ANGLE_angleUniforms [[buffer(19)]],
    constant ANGLE_UserUniforms & ANGLE_userUniforms [[buffer(20)]],
    uint32_t ANGLE_instanceIdMod [[instance_id]], uint32_t ANGLE_baseInstance [[base_instance]],
    uint32_t ANGLE_vertexIDMetal [[vertex_id]])
{
    ANGLE_VertexOut ANGLE_vertexOut;
    {
        _umain(ANGLE_vertexOut, ANGLE_vertexIn, ANGLE_userUniforms, ANGLE_vertexIDMetal);
        ANGLE_vertexOut.gl_Position.y = (ANGLE_vertexOut.gl_Position.y * ANGLE_flipXY(ANGLE_angleUniforms).y);
        if ((ANGLE_angleUniforms.ANGLE_misc & 0x10000000u) != 0u)
        {
            ANGLE_vertexOut.gl_Position.z = ((ANGLE_vertexOut.gl_Position.z + ANGLE_vertexOut.gl_Position.w) * 0.5f);
        }
        if (ANGLERasterizerDisabled)
        {
            ANGLE_vertexOut.gl_Position = metal::float4(-3.0f, -3.0f, -3.0f, 1.0f);
        }
    }
#if ANGLE_TRANSFORM_FEEDBACK_ENABLED
    return;
#else
    return ANGLE_vertexOut;
#endif
}

void _umain(thread ANGLE_FragmentOut &ANGLE_fragmentOut, thread ANGLE_FragmentIn &ANGLE_fragmentIn, constant ANGLE_UserUniforms &ANGLE_userUniforms, thread ANGLE_TextureEnvs &ANGLE_textureEnvs, metal::float4 gl_FragCoord, bool gl_FrontFacing)
{
    metal::float4 _ucolor = (ANGLE_texture(ANGLE_textureEnvs._uTex, ANGLE_fragmentIn._uvTexCoord) * ANGLE_fragmentIn._uvColor);
    if (_ucolor.w < ANGLE_userUniforms._uAlphaRef)
    {
        metal::discard_fragment();
    }
    _ucolor += ANGLE_texture(ANGLE_textureEnvs._uTex, ANGLE_fragmentIn._uvTexCoord, -0.5f) * 0.25f;
    _ucolor.xyz += ANGLE_textureLod(ANGLE_textureEnvs._uEnv, ANGLE_fragmentIn._uvNormal, 2.0f).xyz;
    _ucolor.xyz *= ANGLE_shadow(ANGLE_textureEnvs._uShadow, metal::float3(gl_FragCoord.xy / 256.0f, 0.5f));
    metal::int2 _usize = ANGLE_textureSize(ANGLE_textureEnvs._uTex, 0);
    _ucolor += ANGLE_texelFetch(ANGLE_textureEnvs._uTex, metal::clamp(metal::int2(gl_FragCoord.xy), metal::int2(0), (_usize - 1)), 0) * 0.125f;
    if (!gl_FrontFacing)
    {
        _ucolor.xyz = _ucolor.zyx;
    }
    ANGLE_fragmentOut._ufragColor = _ucolor;
}

fragment ANGLE_FragmentOut main1(ANGLE_FragmentIn ANGLE_fragmentIn [[stage_in]],
    constant ANGLE_UniformsStruct & ANGLE_angleUniforms [[buffer(19)]],
    constant ANGLE_UserUniforms & ANGLE_userUniforms [[buffer(20)]],
    metal::texture2d<float> ANGLE__1_uTex_texture [[texture(0)]], metal::sampler ANGLE__2_uTex_sampler [[sampler(0)]],
    metal::texturecube<float> ANGLE__3_uEnv_texture [[texture(1)]], metal::sampler ANGLE__4_uEnv_sampler [[sampler(1)]],
    metal::depth2d<float> ANGLE__5_uShadow_texture [[texture(2)]], metal::sampler ANGLE__6_uShadow_sampler [[sampler(2)]],
    metal::float4 gl_FragCoord [[position]], bool gl_FrontFacing [[front_facing]],
    uint32_t ANGLE_sampleMask [[sample_mask, function_constant(ANGLESampleMaskWriteEnabled)]])
{
    ANGLE_FragmentOut ANGLE_fragmentOut;
    ANGLE_TextureEnvs ANGLE_textureEnvs;
    ANGLE_textureEnvs._uTex.texture = ANGLE_addressof(ANGLE__1_uTex_texture);
    ANGLE_textureEnvs._uTex.sampler = ANGLE_addressof(ANGLE__2_uTex_sampler);
    ANGLE_textureEnvs._uEnv.texture = ANGLE_addressof(ANGLE__3_uEnv_texture);
    ANGLE_textureEnvs._uEnv.sampler = ANGLE_addressof(ANGLE__4_uEnv_sampler);
    ANGLE_textureEnvs._uShadow.texture = ANGLE_addressof(ANGLE__5_uShadow_texture);
    ANGLE_textureEnvs._uShadow.sampler = ANGLE_addressof(ANGLE__6_uShadow_sampler);
    {
        if (ANGLEWriteHelperSampleMask)
        {
            ANGLE_fragmentOut.ANGLE_metal_SampleMask = 4294967295u;
        }
        _umain(ANGLE_fragmentOut, ANGLE_fragmentIn, ANGLE_userUniforms, ANGLE_textureEnvs, gl_FragCoord, gl_FrontFacing);
        if (ANGLEMultisampledRendering)
        {
            ANGLE_fragmentOut.ANGLE_metal_SampleMask = ANGLE_angleUniforms.ANGLE_coverageMask;
            if (ANGLEEmulateAlphaToCoverage)
            {
                ANGLE_fragmentOut.ANGLE_metal_SampleMask &= ANGLE_alphaToCoverage(ANGLE_fragmentOut._ufragColor.w, 4u);
            }
            ANGLE_fragmentOut.ANGLE_metal_SampleMask &= ANGLE_sampleMask;
        }
        if (ANGLEDepthWriteEnabled)
        {
            ANGLE_fragmentOut.gl_FragDepth = metal::clamp(gl_FragCoord.z, 0.0f, 1.0f);
        }
    }
    return ANGLE_fragmentOut;
}
