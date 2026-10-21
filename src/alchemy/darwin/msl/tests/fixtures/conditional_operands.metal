// Conditional operators mixing class prvalues and local lvalues
// (project-authored fixture). Clang's OpenCL C++ types a local as
// `__private T` and a temporary as plain `T`, so the compiler must make
// `c ? float2(0.0f) : table[i]` (ordinary MSL, common in GLSL-derived
// shaders) compile.
#include <metal_stdlib>
using namespace metal;

struct Shape {
  float2 corner;
  float radius;
};

Shape make_shape(float r) { return Shape{float2(r), r}; }
void bump(thread int& counter) { counter += 1; }
void skip(thread int& counter) { counter += 2; }

struct VOut {
  float4 position [[position]];
  float2 uv;
  float radius;
};

vertex VOut cond_vs(uint vid [[vertex_id]], constant float4* tint [[buffer(0)]])
{
  const float2 arrow_pos[4] = {float2(0.0f, 0.1f), float2(0.3f, 0.4f), float2(0.5f, 0.6f), float2(0.7f, 0.8f)};
  const bool tria = (vid & 1u) != 0u;
  float2 pos = tria ? float2(0.0f) : arrow_pos[vid & 3u];
  pos = !tria ? arrow_pos[min(vid, 2u)] : float2(1.0f, 0.0f);
  Shape local_shape = make_shape(2.0f);
  Shape shape = tria ? make_shape(1.0f) : local_shape;
  // Nested, with comparisons in the condition and a template-id operand.
  const float4 t = tint[0];
  float4 color = t.x > 0.5f ? t : (t.y > 0.5f ? float4(0.25f) : vec<float, 4>(t.zwxy));
  const float3 c3 = vid > 2u ? float3(color.xyz) : (vid == 1u ? float3(1.0f) : color.zyx);
  int counter = 0;
  tria ? bump(counter) : skip(counter);
  const float scale = tria ? (counter, 2.0f) : 3.0f;
  VOut out;
  out.position = float4(pos * scale + shape.corner, c3.x, 1.0f);
  out.uv = vid == 0u ? pos : shape.corner + float2(float(counter));
  out.radius = shape.radius;
  return out;
}

fragment float4 cond_fs(VOut in [[stage_in]], device const float4* palette [[buffer(1)]])
{
  float4 local = float4(in.uv, 0.0f, 1.0f);
  device const float4* row = in.radius > 1.5f ? palette : palette + 4;
  return in.uv.x > 0.5f ? row[1] : local;
}
