// Less common outputs of Dawn's WGSL compiler (Tint) for Skia Graphite and
// Dawn's own internal pipelines (design: Tint's public MSL writer): clip
// distances as a tint_array builtin, dual-source blending, framebuffer
// fetch (chromium_experimental_framebuffer_fetch), primitive ids, the layer
// output, textureSampleBaseClampToEdge's half-texel clamp and immediate data
// passed as a constant buffer.
#include <metal_stdlib>
using namespace metal;

template<typename T, size_t N>
struct tint_array {
  const constant T& operator[](size_t i) const constant { return elements[i]; }
  device T& operator[](size_t i) device { return elements[i]; }
  const device T& operator[](size_t i) const device { return elements[i]; }
  thread T& operator[](size_t i) thread { return elements[i]; }
  const thread T& operator[](size_t i) const thread { return elements[i]; }
  threadgroup T& operator[](size_t i) threadgroup { return elements[i]; }
  const threadgroup T& operator[](size_t i) const threadgroup { return elements[i]; }
  T elements[N];
};

struct Immediates {
  /* 0x0000 */ float4 rect;
  /* 0x0010 */ uint layer;
  /* 0x0014 */ tint_array<int8_t, 12> tint_pad;
};

struct clip_vs_outputs {
  float4 tint_symbol [[position]];
  float2 uv [[user(locn0)]];
  tint_array<float, 2> clip_distances [[clip_distance]];
  uint layer [[render_target_array_index]];
};

vertex clip_vs_outputs clip_vs(uint vi [[vertex_id]], const constant Immediates* imm [[buffer(5)]]) {
  float2 const p = float2(float((vi & 1u)), float((vi >> 1u)));
  clip_vs_outputs tint_wrapper_result = {};
  tint_wrapper_result.tint_symbol = float4(mix((*imm).rect.xy, (*imm).rect.zw, p), 0.0f, 1.0f);
  tint_wrapper_result.uv = p;
  tint_wrapper_result.clip_distances[0u] = (p.x - 0.25f);
  tint_wrapper_result.clip_distances[1u] = (0.75f - p.y);
  tint_wrapper_result.layer = (*imm).layer;
  return tint_wrapper_result;
}

struct blend_fs_outputs {
  float4 color [[color(0)]] [[index(0)]];
  float4 blend [[color(0)]] [[index(1)]];
};

struct blend_fs_inputs {
  float2 uv [[user(locn0)]];
};

float4 tint_sample_clamped(texture2d<float, access::sample> t, sampler s, float2 coords) {
  float2 const dims = float2(uint2(t.get_width(0u), t.get_height(0u)));
  float2 const half_texel = (float2(0.5f) / dims);
  float2 const clamped = clamp(coords, half_texel, (1.0f - half_texel));
  return t.sample(s, clamped, level(0.0f));
}

fragment blend_fs_outputs blend_fs(blend_fs_inputs inputs [[stage_in]], uint prim [[primitive_id]],
                                   texture2d<float, access::sample> t [[texture(0)]], sampler s [[sampler(0)]]) {
  float4 const c = tint_sample_clamped(t, s, inputs.uv);
  blend_fs_outputs tint_wrapper_result = {};
  tint_wrapper_result.color = c;
  tint_wrapper_result.blend = float4(float((prim & 1u)), 0.0f, 0.0f, c.w);
  return tint_wrapper_result;
}

// Framebuffer fetch: the current colour attachment 0 as an input.
struct fetch_fs_outputs {
  float4 tint_symbol [[color(0)]];
};

fragment fetch_fs_outputs fetch_fs(float4 dst [[color(0)]], float4 pos [[position]]) {
  fetch_fs_outputs tint_wrapper_result = {};
  tint_wrapper_result.tint_symbol = float4((dst.xyz * 0.5f) + float3(fract(pos.x / 8.0f)), dst.w);
  return tint_wrapper_result;
}
