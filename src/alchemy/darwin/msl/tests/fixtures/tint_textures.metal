// Texture built-ins in the style Dawn's WGSL compiler (Tint) emits (design:
// Tint's public MSL writer): every WGSL texture type as an entry-point
// argument, carried in a module-variables struct, with sampling offsets,
// bias/level/gradient options, depth comparison, gathers with a component,
// reads of integer and multisampled textures, and size queries per level.
#include <metal_stdlib>
using namespace metal;

struct tint_module_vars_struct {
  texture2d<float, access::sample> t2;
  texture2d_array<float, access::sample> ta;
  texturecube<float, access::sample> tc;
  texture3d<float, access::sample> t3;
  depth2d<float, access::sample> td;
  texture2d<uint, access::read> ti;
  sampler s;
  sampler sc;
  texture2d_ms<float, access::read> tm;
  depth2d_array<float, access::sample> tda;
  texturecube_array<float, access::sample> tca;
};

struct vs_outputs {
  float4 tint_symbol [[position]];
};

struct fs_outputs {
  float4 tint_symbol_1 [[color(0)]];
};

vertex vs_outputs vs(uint i [[vertex_id]]) {
  vs_outputs tint_wrapper_result = {};
  tint_wrapper_result.tint_symbol = float4(((float((i & 1u)) * 2.0f) - 1.0f), ((float((i >> 1u)) * 2.0f) - 1.0f), 0.0f, 1.0f);
  return tint_wrapper_result;
}

float4 fs_inner(float4 p, tint_module_vars_struct tint_module_vars) {
  float2 const uv = (p.xy / 64.0f);
  float4 r = tint_module_vars.t2.sample(tint_module_vars.s, uv, int2(1, -1));
  r = (r + tint_module_vars.ta.sample(tint_module_vars.s, uv, max(min(1, int((tint_module_vars.ta.get_array_size() - 1u))), 0), bias(clamp(0.5f, -16.0f, 15.9899997711181640625f))));
  r = (r + tint_module_vars.tc.sample(tint_module_vars.s, float3(uv, 1.0f), level(2.0f)));
  r = (r + tint_module_vars.t3.sample(tint_module_vars.s, float3(uv, 0.5f), gradient3d(float3(0.00999999977648258209f), float3(0.01999999955296516418f))));
  r = (r + float4(tint_module_vars.td.sample_compare(tint_module_vars.sc, uv, 0.5f)));
  r = (r + float4(tint_module_vars.td.sample_compare(tint_module_vars.sc, uv, 0.5f, level(0), int2(0, 1))));
  r = (r + tint_module_vars.t2.gather(tint_module_vars.s, uv, int2(0), component::y));
  r = (r + tint_module_vars.td.gather_compare(tint_module_vars.sc, uv, 0.25f));
  uint const ti_lod = min(0u, (tint_module_vars.ti.get_num_mip_levels() - 1u));
  r = (r + float4(tint_module_vars.ti.read(uint2(min(uint2(int2(p.xy)), (uint2(tint_module_vars.ti.get_width(ti_lod), tint_module_vars.ti.get_height(ti_lod)) - uint2(1u)))), ti_lod)));
  r = (r + tint_module_vars.tm.read(uint2(int2(p.xy)), 2u));
  r = (r + float4(tint_module_vars.tda.sample_compare(tint_module_vars.sc, uv, 1u, 0.5f)));
  r = (r + tint_module_vars.tca.sample(tint_module_vars.s, float3(uv, 1.0f), 2u, bias(0.25f)));
  uint const v = min(1u, (tint_module_vars.t2.get_num_mip_levels() - 1u));
  uint2 const dims = uint2(tint_module_vars.t2.get_width(v), tint_module_vars.t2.get_height(v));
  r.x = (r.x + float((((dims.x + tint_module_vars.t2.get_num_mip_levels()) + tint_module_vars.ta.get_array_size()) + tint_module_vars.tm.get_num_samples())));
  uint3 const d3 = uint3(tint_module_vars.t3.get_width(0u), tint_module_vars.t3.get_height(0u), tint_module_vars.t3.get_depth(0u));
  r.y = (r.y + float(((d3.x + d3.y) + d3.z) + tint_module_vars.tc.get_width(0u)));
  return r;
}

fragment fs_outputs fs(float4 p [[position]], texture2d<float, access::sample> t2 [[texture(0)]],
                       texture2d_array<float, access::sample> ta [[texture(1)]],
                       texturecube<float, access::sample> tc [[texture(2)]],
                       texture3d<float, access::sample> t3 [[texture(3)]],
                       depth2d<float, access::sample> td [[texture(4)]],
                       texture2d<uint, access::read> ti [[texture(5)]], sampler s [[sampler(0)]],
                       sampler sc [[sampler(1)]], texture2d_ms<float, access::read> tm [[texture(6)]],
                       depth2d_array<float, access::sample> tda [[texture(7)]],
                       texturecube_array<float, access::sample> tca [[texture(8)]]) {
  tint_module_vars_struct const tint_module_vars = tint_module_vars_struct{.t2=t2, .ta=ta, .tc=tc, .t3=t3, .td=td, .ti=ti, .s=s, .sc=sc, .tm=tm, .tda=tda, .tca=tca};
  fs_outputs tint_wrapper_result = {};
  tint_wrapper_result.tint_symbol_1 = fs_inner(p, tint_module_vars);
  return tint_wrapper_result;
}

// Storage textures in a compute shader: write-only, read-write with fence,
// and 1D/3D/array dimensions.
kernel void storage_main(uint3 gid [[thread_position_in_grid]],
                         texture2d<float, access::write> img [[texture(0)]],
                         texture2d<uint, access::read_write> counts [[texture(1)]],
                         texture2d_array<float, access::write> layers [[texture(2)]],
                         texture3d<float, access::read> volume [[texture(3)]],
                         texture1d<float, access::read_write> line [[texture(4)]]) {
  uint2 const c = gid.xy;
  float4 const v = volume.read(uint3(min(gid, (uint3(volume.get_width(), volume.get_height(), volume.get_depth()) - uint3(1u)))));
  img.write(v, c);
  layers.write(v.wzyx, c, gid.z);
  uint4 const n = counts.read(c);
  counts.write((n + uint4(1u)), c);
  counts.fence();
  float4 const l = line.read(gid.x);
  line.write((l * 0.5f), gid.x);
  line.fence();
}
