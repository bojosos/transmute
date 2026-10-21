// Render shaders in the style Dawn's WGSL compiler (Tint) emits for WebGPU
// pages and Skia Graphite (design: Tint's public MSL writer): a tint_array
// template whose operator[] is overloaded on the object's address space,
// a module-variables struct passed to the inner functions, designated
// initializers, entry-point input/output structs with flattened members,
// [[invariant]] positions, packed vec3 struct members, explicit padding,
// integer polyfills built on select(), and discard through a flag.
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

struct tint_packed_vec3_f32_array_element {
  /* 0x0000 */ packed_float3 packed;
  /* 0x000c */ tint_array<int8_t, 4> tint_pad;
};

struct Uniforms_packed_vec3 {
  /* 0x0000 */ float4x4 mvp;
  /* 0x0040 */ float3x3 normal;
  /* 0x0070 */ float4 tint;
  /* 0x0080 */ tint_array<float4, 4> offsets;
  /* 0x00c0 */ packed_float3 light;
  /* 0x00cc */ uint count;
  /* 0x00d0 */ float scale;
  /* 0x00d4 */ tint_array<int8_t, 12> tint_pad_1;
  /* 0x00e0 */ tint_array<tint_packed_vec3_f32_array_element, 3> corners;
};

struct VIn {
  float3 pos;
  float2 uv;
  float4 color;
  uint id;
};

struct VOut {
  float4 pos;
  float2 uv;
  float4 color;
  uint id;
  float3 n;
};

struct tint_module_vars_struct {
  const constant Uniforms_packed_vec3* u;
  texture2d<float, access::sample> tex;
  sampler samp;
  thread bool* continue_execution;
};

struct vs_outputs {
  float4 VOut_pos [[position]] [[invariant]];
  float2 VOut_uv [[user(locn0)]];
  float4 VOut_color [[user(locn1)]];
  uint VOut_id [[user(locn2)]] [[flat]];
  float3 VOut_n [[user(locn3)]] [[centroid_no_perspective]];
};

struct vs_inputs {
  float3 VIn_pos [[attribute(0)]];
  float2 VIn_uv [[attribute(1)]];
  float4 VIn_color [[attribute(2)]];
  uint VIn_id [[attribute(3)]];
};

struct fs_outputs {
  float4 tint_symbol [[color(0)]];
};

struct fs_inputs {
  float2 VOut_uv [[user(locn0)]];
  float4 VOut_color [[user(locn1)]];
  uint VOut_id [[user(locn2)]] [[flat]];
  float3 VOut_n [[user(locn3)]] [[centroid_no_perspective]];
};

uint tint_mod_u32(uint lhs, uint rhs) {
  uint const v = select(rhs, 1u, (rhs == 0u));
  return (lhs - ((lhs / v) * v));
}

int tint_div_i32(int lhs, int rhs) {
  return (lhs / select(rhs, 1, ((rhs == 0) | ((lhs == (-2147483647 - 1)) & (rhs == -1)))));
}

int tint_f32_to_i32(float value) {
  return select(2147483647, select((-2147483647 - 1), int(value), (value >= -2147483648.0f)), (value <= 2147483520.0f));
}

float3 corner(uint i, tint_module_vars_struct tint_module_vars) {
  return float3((*tint_module_vars.u).corners[min(i, 2u)].packed);
}

VOut vs_inner(VIn v, uint vi, uint ii, tint_module_vars_struct tint_module_vars) {
  VOut o = {};
  float4 const off = (*tint_module_vars.u).offsets[tint_mod_u32((vi + ii), 4u)];
  o.pos = ((*tint_module_vars.u).mvp * float4(((v.pos + off.xyz) + corner(vi, tint_module_vars)), 1.0f));
  o.uv = (v.uv * (*tint_module_vars.u).scale);
  o.color = (v.color * (*tint_module_vars.u).tint);
  o.id = (v.id + ii);
  o.n = ((*tint_module_vars.u).normal * (v.pos - float3((*tint_module_vars.u).light)));
  int const k = tint_div_i32(tint_f32_to_i32(o.pos.x), as_type<int>(o.id));
  o.pos.z = (o.pos.z + float(k) * 0.0f);
  return o;
}

vertex vs_outputs vs(uint vi [[vertex_id]], uint ii [[instance_id]], vs_inputs inputs [[stage_in]],
                     const constant Uniforms_packed_vec3* u [[buffer(0)]]) {
  tint_module_vars_struct const tint_module_vars = tint_module_vars_struct{.u=u};
  VOut const v_1 = vs_inner(VIn{.pos=inputs.VIn_pos, .uv=inputs.VIn_uv, .color=inputs.VIn_color, .id=inputs.VIn_id}, vi, ii, tint_module_vars);
  vs_outputs tint_wrapper_result = {};
  tint_wrapper_result.VOut_pos = v_1.pos;
  tint_wrapper_result.VOut_uv = v_1.uv;
  tint_wrapper_result.VOut_color = v_1.color;
  tint_wrapper_result.VOut_id = v_1.id;
  tint_wrapper_result.VOut_n = v_1.n;
  return tint_wrapper_result;
}

float4 fs_inner(VOut i, bool ff, tint_module_vars_struct tint_module_vars) {
  float4 c = (tint_module_vars.tex.sample(tint_module_vars.samp, i.uv) * i.color);
  if ((c.w < 0.00999999977648258209f)) {
    (*tint_module_vars.continue_execution) = false;
  }
  if (!(ff)) {
    c = float4(c.zyx, c.w);
  }
  float const d = ((fwidth(i.uv.x) + dfdx(i.uv.y)) - dfdy(i.n.z));
  float4 const v_2 = (c + float4(d, float((i.id & 1u)), normalize(i.n).x, 0.0f));
  if (!((*tint_module_vars.continue_execution))) {
    discard_fragment();
  }
  return v_2;
}

fragment fs_outputs fs(float4 VOut_pos [[position]], bool ff [[front_facing]], fs_inputs inputs [[stage_in]],
                       texture2d<float, access::sample> tex [[texture(0)]], sampler samp [[sampler(0)]]) {
  thread bool continue_execution = true;
  tint_module_vars_struct const tint_module_vars = tint_module_vars_struct{.u=nullptr, .tex=tex, .samp=samp, .continue_execution=(&continue_execution)};
  fs_outputs tint_wrapper_result = {};
  tint_wrapper_result.tint_symbol = fs_inner(VOut{.pos=VOut_pos, .uv=inputs.VOut_uv, .color=inputs.VOut_color, .id=inputs.VOut_id, .n=inputs.VOut_n}, ff, tint_module_vars);
  return tint_wrapper_result;
}

// A full-screen triangle with multiple render targets, depth and sample-mask
// outputs, and a sample-rate fragment (Graphite/Dawn blit-style passes).
struct mrt_outputs {
  float4 Out_a [[color(0)]];
  uint4 Out_b [[color(1)]];
  float Out_depth [[depth(any)]];
  uint Out_mask [[sample_mask]];
};

struct Out {
  float4 a;
  uint4 b;
  float depth;
  uint mask;
};

struct mrt_vs_outputs {
  float4 tint_symbol [[position]];
};

vertex mrt_vs_outputs mrt_vs(uint i [[vertex_id]]) {
  tint_array<float2, 3> p = tint_array<float2, 3>{float2(-1.0f), float2(3.0f, -1.0f), float2(-1.0f, 3.0f)};
  mrt_vs_outputs tint_wrapper_result = {};
  tint_wrapper_result.tint_symbol = float4(p[min(i, 2u)], 0.0f, 1.0f);
  return tint_wrapper_result;
}

Out mrt_fs_inner(float4 pos, uint si, uint inMask) {
  Out o = {};
  uint2 const ip = uint2(pos.xy);
  o.a = float4(fract((pos.xy / 16.0f)), 0.5f, 1.0f);
  o.b = uint4(ip, si, popcount(inMask));
  o.depth = clamp((pos.z * 0.5f), 0.0f, 1.0f);
  o.mask = (inMask & 15u);
  return o;
}

fragment mrt_outputs mrt_fs(float4 pos [[position]], uint si [[sample_id]], uint inMask [[sample_mask]]) {
  Out const v = mrt_fs_inner(pos, si, inMask);
  mrt_outputs tint_wrapper_result = {};
  tint_wrapper_result.Out_a = v.a;
  tint_wrapper_result.Out_b = v.b;
  tint_wrapper_result.Out_depth = v.depth;
  tint_wrapper_result.Out_mask = v.mask;
  return tint_wrapper_result;
}
