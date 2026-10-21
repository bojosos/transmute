// User code that overloads metal_stdlib names (project-authored fixture).
// GLSL-to-MSL generators such as Blender's declare extra overloads of `mix`,
// `clamp`, `min`, ... at global scope next to `using namespace metal;`. In
// Metal both the user overloads and metal::* stay visible to unqualified
// lookup and overload resolution picks between them; the compiler must not
// put user code in a scope that hides the standard library.
#include <metal_stdlib>
using namespace metal;

using vec2 = float2;
using vec4 = float4;
using ivec2 = int2;
using bvec4 = bool4;

// GLSL `mix(a, b, bvec)`: per-component select.
template<typename T, int N> vec<T, N> mix(vec<T, N> a, vec<T, N> b, vec<bool, N> s)
{
  vec<T, N> r;
  for (int i = 0; i < N; i++) {
    r[i] = s[i] ? b[i] : a[i];
  }
  return r;
}
// GLSL `mix(a, b, bool)`.
template<typename T, int N> vec<T, N> mix(vec<T, N> a, vec<T, N> b, bool s) { return s ? b : a; }
template<typename T> T mix(T a, T b, bool s) { return s ? b : a; }

// Mixed scalar/vector and int/float forms GLSL accepts.
template<typename T, int N> vec<T, N> clamp(vec<T, N> x, int lo, int hi) { return clamp(x, vec<T, N>(lo), vec<T, N>(hi)); }
inline int min(int a, uint b) { return min(a, int(b)); }
inline float max(float a, int b) { return max(a, float(b)); }
inline float dot(float a, float b) { return a * b; }
inline float length(float a) { return abs(a); }
template<typename T> T sign_or_one(T v) { return select(T(1), sign(v), v != T(0)); }

// A shader-class member function with a stdlib name must not hide the free
// functions either (calls inside the class use metal:: explicitly or rely on
// overload resolution against the global ones).
struct Glyph {
  float4 color;
  float2 uv;
  int flags;
  float smooth(float d) const { return metal::smoothstep(0.25f, 0.75f, d); }
  float4 mix(float4 other, float t) const { return metal::mix(color, other, t); }
};

struct Interp {
  float4 position [[position]];
  float4 color [[flat]];
  float2 uv;
  int flags [[flat]];
};

struct Params {
  float4 tint;
  float threshold;
  int lo;
  int hi;
};

vertex Interp overloads_vs(uint vid [[vertex_id]], constant Params& params [[buffer(0)]])
{
  Interp out;
  const float2 corner = float2(float(vid & 1u), float((vid >> 1) & 1u));
  out.position = float4(corner * 2.0f - 1.0f, 0.0f, 1.0f);
  out.uv = corner;
  out.color = mix(params.tint, float4(1.0f), bvec4(true, false, true, false));
  out.flags = min(params.lo, vid);
  return out;
}

fragment float4 overloads_fs(Interp in [[stage_in]], constant Params& params [[buffer(0)]],
                             texture2d<float> glyphs [[texture(0)]], sampler glyph_sampler [[sampler(0)]])
{
  Glyph g;
  g.color = in.color;
  g.uv = in.uv;
  g.flags = in.flags;
  // metal::mix(float, float, float) next to the user templates above.
  const float d = glyphs.sample(glyph_sampler, g.uv).r;
  const float a = mix(0.0f, 1.0f, g.smooth(d));
  const float b = mix(a, 0.5f, d > params.threshold);           // user mix(T, T, bool)
  const float4 c = mix(g.color, float4(b), 0.5f);              // metal::mix(vec, vec, float)
  const float4 e = mix(c, float4(0.0f), bvec4(d > 0.9f));      // user mix(vec, vec, vec<bool>)
  const float4 f = clamp(e, params.lo, params.hi);             // user clamp(vec, int, int)
  const float2 h = clamp(in.uv, 0.0f, 1.0f);                    // metal::clamp(vec, float, float)
  const float k = max(dot(h.x, h.y), params.lo) + length(-b) + sign_or_one(d - 0.5f);
  return g.mix(f, saturate(k * 0.125f));
}

kernel void overloads_cs(device float4* data [[buffer(0)]], constant Params& params [[buffer(1)]],
                         uint id [[thread_position_in_grid]])
{
  float4 v = data[id];
  v = mix(v, float4(abs(v.x)), v > float4(params.threshold));
  v = clamp(v, params.lo, params.hi);
  data[id] = mix(v, params.tint, fract(v.w));
}
