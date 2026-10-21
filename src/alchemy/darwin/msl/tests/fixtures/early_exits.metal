// Early returns, discards and switches with shared case targets
// (project-authored fixture). Shapes from GLSL-derived vertex/fragment code:
// a helper with several early `return`s inlined into the entry point (all
// returns share one exit block in the IR), nested conditionals that rejoin
// after an early exit, and a switch whose cases share targets that merge
// values through phis.
#include <metal_stdlib>
using namespace metal;

struct VOut {
  float4 position [[position]];
  float4 color;
  float size [[point_size]];
};

struct Params {
  float4 tint;
  int mode;
  int kind;
  uint flags;
  float scale;
};

float4 classify(int kind, float4 base)
{
  float4 result;
  switch (kind) {
    case 0:
    case 3:
      result = base * 0.5f;
      break;
    case 1:
    case 4:
    case 7:
      result = base.wzyx;
      break;
    case 2:
      result = float4(base.x);
      break;
    default:
      result = float4(0.0f);
      break;
  }
  return result;
}

void vertex_main(constant Params& p, uint vid, thread VOut& out)
{
  out.position = float4(float(vid & 1u), float(vid >> 1), 0.0f, 1.0f);
  out.color = p.tint;
  out.size = 1.0f;
  if ((p.flags & 1u) != 0u) {
    if (p.mode == 2) {
      out.position = float4(0.0f);
      return;
    }
    out.color *= 2.0f;
  }
  else if (p.mode == 3) {
    if ((p.flags & 4u) != 0u) {
      return;
    }
    out.color = out.color.zyxw;
  }
  out.size = p.scale;
  if (vid > 64u) {
    return;
  }
  out.color = classify(p.kind, out.color);
  if ((p.flags & 2u) != 0u && p.mode > 4) {
    out.position.z = 0.5f;
    return;
  }
  out.position.xy *= p.scale;
}

vertex VOut exits_vs(uint vid [[vertex_id]], constant Params& p [[buffer(0)]])
{
  VOut out;
  vertex_main(p, vid, out);
  return out;
}

fragment float4 exits_fs(VOut in [[stage_in]], constant Params& p [[buffer(0)]])
{
  if (in.color.a < 0.01f) {
    discard_fragment();
  }
  float4 c = in.color;
  if (p.mode == 1) {
    if (c.r > 0.5f) {
      return float4(1.0f);
    }
    c = c.bgra;
  }
  switch (p.kind) {
    case 0: case 2: c *= 0.5f; break;
    case 1: if (c.g > 0.9f) discard_fragment(); c += 0.25f; break;
    default: break;
  }
  return c;
}
