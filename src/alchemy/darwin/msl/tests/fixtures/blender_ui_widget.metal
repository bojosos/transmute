// Modeled on Blender's mtl_shader_generator output (GPU_SHADER_2D_WIDGET-like).
#include <metal_stdlib>
#include <simd/simd.h>
using namespace metal;

#define vec2 float2
#define vec3 float3
#define vec4 float4
#define mat3 float3x3
#define mat4 float4x4
#define ivec2 int2
#define uvec4 uint4
#define bvec2 bool2
#define mix(a, b, t) metal::mix(a, b, t)
#define fract(x) metal::fract(x)
#define inversesqrt(x) metal::rsqrt(x)

constant int MTL_uniform_buffer_base_index [[function_constant(0)]];
constant bool MTL_global_pointsize [[function_constant(1)]];
constant int SPEC_shadow_samples [[function_constant(2)]];

template<typename T, access A = access::sample>
struct _mtl_combined_image_sampler_2d {
    thread texture2d<T, A> *texture;
    thread sampler *samp;
};
template<typename T, access A>
vec4 texture(thread _mtl_combined_image_sampler_2d<T, A> tex, vec2 uv) {
    return tex.texture->sample(*tex.samp, uv);
}
template<typename T, access A>
ivec2 textureSize(thread _mtl_combined_image_sampler_2d<T, A> tex, int lod) {
    return ivec2(tex.texture->get_width(lod), tex.texture->get_height(lod));
}

class ShaderClass {
public:
    struct VertexIn {
        vec2 pos [[attribute(0)]];
        vec4 color [[attribute(1)]];
        uint flags [[attribute(2)]];
    };
    struct PushConstantBlock {
        mat4 ModelViewProjectionMatrix;
        vec4 parameters[11];
        float alpha;
        int shape_type;
    };
    // Instance state the generated wrapper fills in.
    vec2 pos;
    vec4 color;
    uint flags;
    vec4 gl_Position;
    float gl_PointSize = 1.0;
    vec4 finalColor;
    vec2 uvInterp;
    int shapeFlat;
    const constant PushConstantBlock *push;
    _mtl_combined_image_sampler_2d<float, access::sample> image;

    void main_vertex() {
        vec4 rect = push->parameters[0];
        vec2 p = mix(rect.xy, rect.zw, pos);
        uvInterp = pos;
        finalColor = color * vec4(1.0, 1.0, 1.0, push->alpha);
        if ((flags & 1u) != 0u) {
            finalColor.rgb = mix(finalColor.rgb, vec3(1.0), 0.25);
        }
        shapeFlat = push->shape_type;
        gl_Position = push->ModelViewProjectionMatrix * vec4(p, 0.0, 1.0);
    }

    vec4 fragColor;
    vec4 gl_FragCoord;
    void main_fragment() {
        vec4 tex = texture(image, uvInterp);
        float d = length(uvInterp - 0.5);
        float accum = 0.0;
        for (int i = 0; i < SPEC_shadow_samples; i++) {
            accum += fract(d * float(i + 1));
        }
        if (shapeFlat == 3 && d > 0.5) {
            discard_fragment();
        }
        fragColor = tex * finalColor;
        fragColor.a *= smoothstep(0.5, 0.45, d) * (1.0 + accum);
        fragColor.rgb = pow(fragColor.rgb, vec3(inversesqrt(2.2)));
    }
};

struct VertexOut {
    vec4 _default_position_ [[position]];
    float _default_point_size_ [[point_size, function_constant(MTL_global_pointsize)]];
    vec4 finalColor [[user(locn0)]];
    vec2 uvInterp [[user(locn1)]];
    int shapeFlat [[user(locn2)]] [[flat]];
};

struct FragmentIn {
    vec4 _default_position_ [[position]];
    vec4 finalColor [[user(locn0)]];
    vec2 uvInterp [[user(locn1)]];
    int shapeFlat [[user(locn2)]] [[flat]];
};

struct FragmentOut {
    vec4 fragColor [[color(0)]];
};

vertex VertexOut vertex_function_entry(
    ShaderClass::VertexIn v_in [[stage_in]],
    constant ShaderClass::PushConstantBlock* uniforms [[buffer(MTL_uniform_buffer_base_index)]],
    uint gl_VertexID [[vertex_id]],
    uint gl_InstanceID [[instance_id]]) {
    ShaderClass vertex_shader_instance;
    vertex_shader_instance.pos = v_in.pos;
    vertex_shader_instance.color = v_in.color;
    vertex_shader_instance.flags = v_in.flags;
    vertex_shader_instance.push = uniforms;
    vertex_shader_instance.main_vertex();
    VertexOut output;
    output._default_position_ = vertex_shader_instance.gl_Position;
    output._default_position_.y = -output._default_position_.y;
    output._default_position_.z = (output._default_position_.z + output._default_position_.w) / 2.0;
    if (MTL_global_pointsize) output._default_point_size_ = vertex_shader_instance.gl_PointSize;
    output.finalColor = vertex_shader_instance.finalColor;
    output.uvInterp = vertex_shader_instance.uvInterp;
    output.shapeFlat = vertex_shader_instance.shapeFlat + int(gl_VertexID & 0u) + int(gl_InstanceID & 0u);
    return output;
}

fragment FragmentOut fragment_function_entry(
    FragmentIn v_in [[stage_in]],
    constant ShaderClass::PushConstantBlock* uniforms [[buffer(MTL_uniform_buffer_base_index)]],
    texture2d<float, access::sample> image [[texture(0)]],
    sampler image_sampler [[sampler(0)]],
    bool gl_FrontFacing [[front_facing]]) {
    ShaderClass fragment_shader_instance;
    fragment_shader_instance.push = uniforms;
    fragment_shader_instance.gl_FragCoord = v_in._default_position_;
    fragment_shader_instance.finalColor = v_in.finalColor;
    fragment_shader_instance.uvInterp = v_in.uvInterp;
    fragment_shader_instance.shapeFlat = v_in.shapeFlat;
    fragment_shader_instance.image.texture = &image;
    fragment_shader_instance.image.samp = &image_sampler;
    fragment_shader_instance.main_fragment();
    FragmentOut output;
    output.fragColor = fragment_shader_instance.fragColor;
    if (!gl_FrontFacing) output.fragColor.rgb *= 0.5;
    return output;
}
