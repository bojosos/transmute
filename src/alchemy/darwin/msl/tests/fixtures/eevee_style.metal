// Project-authored fixture (MIT) in the style of Blender's EEVEE Metal
// shaders: more samplers than [[sampler(n)]] slots, passed in an argument
// buffer of samplers (`constant SStruct& [[buffer(fc + n)]]`) and reached
// through `constant sampler*` members of texture wrappers; packed_float3
// members used as values (swizzles, compound assignment, standard library
// calls); modf with a thread reference.
#include <metal_stdlib>
using namespace metal;

constant int MTL_uniform_buffer_base_index [[function_constant(0)]];
#define ARGUMENT_BUFFER_NUM_SAMPLERS 18

using sampler_ptr = constant sampler *;
struct SStruct {
  array<sampler, ARGUMENT_BUFFER_NUM_SAMPLERS> sampler_args [[id(0)]];
};

template<typename T> struct combined_sampler_2d {
  thread texture2d<T, access::sample> *texture;
  sampler_ptr samp;
};

struct ClosureLight {
  packed_float3 color;
  float weight;
  packed_float4 data;
};

struct Uniforms {
  packed_float3 light_color;
  float exposure;
};

class FragmentImpl {
public:
  combined_sampler_2d<float> albedo_tx;
  combined_sampler_2d<float> normal_tx;
  constant Uniforms *uniforms;

  float4 shade(float2 uv) {
    ClosureLight cl;
    cl.color = albedo_tx.texture->sample(*albedo_tx.samp, uv).rgb;
    cl.weight = 0.5;
    cl.data = float4(normal_tx.texture->sample(*normal_tx.samp, uv).xyz, 1.0);
    cl.color *= cl.weight * 2.0;
    cl.color += uniforms->light_color;
    cl.data.rgb = abs(cl.color) * cl.data.w;
    float layer = cl.data.x * 4.0;
    float layer_floored;
    float interp = modf(layer, layer_floored);
    float3 sum = (cl.weight > 0.25 ? cl.color : float3(0.5)) + float3(cl.data.xyz);
    sum /= max(distance(cl.color, uniforms->light_color), 1e-4);
    return float4(normalize(sum + float3(0.1)) * interp, layer_floored + dot(cl.color, sum));
  }
};

struct FragmentIn {
  float4 position [[position]];
  float2 uv [[user(uv)]];
};

fragment float4 eevee_style_frag(FragmentIn in [[stage_in]],
                                 constant Uniforms *uniforms [[buffer(MTL_uniform_buffer_base_index + 0)]],
                                 constant SStruct &samplers [[buffer(MTL_uniform_buffer_base_index + 1)]],
                                 texture2d<float, access::sample> albedo [[texture(0)]],
                                 texture2d<float, access::sample> normal [[texture(1)]]) {
  FragmentImpl impl;
  impl.uniforms = uniforms;
  impl.albedo_tx.texture = &albedo;
  impl.albedo_tx.samp = &samplers.sampler_args[3];
  impl.normal_tx.texture = &normal;
  impl.normal_tx.samp = &samplers.sampler_args[17];
  return impl.shade(in.uv);
}

// Framebuffer fetch (Blender's deferred tile classification): [[color(n)]]
// inputs, here through a struct parameter without attribute and directly.
typedef struct {
  uint gbuffer_header [[color(1), raster_order_group(0)]];
  float4 previous [[color(0)]];
} FragmentTileIn;

struct TileOut {
  uint classified [[color(2)]];
};

fragment TileOut tile_classify_frag(FragmentIn in [[stage_in]], FragmentTileIn tile_in,
                                    int4 ids [[color(3)]]) {
  TileOut out;
  out.classified = tile_in.gbuffer_header + uint(tile_in.previous.a * 255.0) + uint(ids.x);
  return out;
}
