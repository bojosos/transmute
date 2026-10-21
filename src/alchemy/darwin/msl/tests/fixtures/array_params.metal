// Array parameters (project-authored fixture). `float4 offset[3]` as a
// parameter is a pointer; the argument may be an array member of a shader
// class reached through `this` (Blender's SMAA passes `offset` this way),
// a local array or a threadgroup-free thread array.
#include <metal_stdlib>
using namespace metal;

float3 gather_sum(float2 uv, float4 offset[3]);
float weights(float2 table[2][2], int row);

struct Shader {
  float4 offset[3];
  float2 table[2][2];
  float2 uv;
  float4 out_color;

  float scaled(float4 values[3], float s) const { return (values[0].x + values[2].w) * s; }

  void main()
  {
    offset[0] = float4(uv, 0.0f, 1.0f);
    offset[1] = offset[0] * 2.0f;
    offset[2] = offset[1].wzyx;
    table[0][0] = uv;
    table[0][1] = uv.yx;
    table[1][0] = float2(1.0f);
    table[1][1] = float2(0.5f);
    const float3 sum = gather_sum(uv, offset);
    out_color = float4(sum, scaled(offset, 0.5f) + weights(table, 1));
  }
};

float3 gather_sum(float2 uv, float4 offset[3])
{
  return offset[0].xyz + offset[1].xyz * uv.x + offset[2].xyz * uv.y;
}

float weights(float2 table[2][2], int row)
{
  return table[row][0].x + table[row][1].y;
}

struct In {
  float4 position [[position]];
  float2 uv;
};

fragment float4 array_params_fs(In in [[stage_in]])
{
  Shader shader;
  shader.uv = in.uv;
  shader.main();
  float4 local[3] = {float4(1.0f), float4(2.0f), float4(3.0f)};
  return shader.out_color + float4(gather_sum(in.uv, local), 0.0f);
}
