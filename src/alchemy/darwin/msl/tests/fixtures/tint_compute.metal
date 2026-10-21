// Compute shaders in the style Dawn's WGSL compiler (Tint) emits (design:
// Tint's public MSL writer): runtime-sized arrays whose lengths come from a
// storage-buffer-sizes uniform at buffer(30), workgroup variables gathered in
// one struct passed as a [[threadgroup(0)]] pointer and zeroed by the first
// invocations, explicit-order atomics including compare-exchange into a
// result struct, a volatile loop guard (TINT_ISOLATE_UB), overridable
// constants as function constants, SIMD-group operations, read_write
// textures with fence(), half storage, and packed integer dot products.
#include <metal_stdlib>
using namespace metal;

#define TINT_ISOLATE_UB(VOLATILE_NAME) \
  {volatile bool VOLATILE_NAME = false; if (VOLATILE_NAME) break;}

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

struct Data {
  /* 0x0000 */ tint_array<uint, 1> values;
};

struct Counters {
  /* 0x0000 */ atomic_uint hits;
  /* 0x0004 */ atomic_int total;
  /* 0x0008 */ atomic_uint maxv;
};

struct atomic_compare_exchange_result_u32 {
  uint old_value;
  bool exchanged;
};

struct tint_symbol_workgroup {
  tint_array<uint, 64> shared_vals;
  atomic_uint wg_count;
};

constant uint wgSize [[function_constant(0)]];
constant float threshold_spec [[function_constant(1)]];
constant float threshold = is_function_constant_defined(threshold_spec) ? threshold_spec : 0.5f;

struct tint_module_vars_struct {
  device Data* data;
  device Counters* counters;
  const device tint_array<float4, 1>* input;
  thread uint* seed;
  threadgroup tint_array<uint, 64>* shared_vals;
  threadgroup atomic_uint* wg_count;
  const constant tint_array<uint4, 1>* tint_storage_buffer_sizes;
};

uint hash(uint x) {
  uint h = ((x * 747796405u) + 2891336453u);
  h = (((h >> (((h >> 28u) + 4u) & 31u)) ^ h) * 277803737u);
  return ((h >> 22u) ^ h);
}

atomic_compare_exchange_result_u32 cmpxchg(device atomic_uint* atomic_ptr, uint cmp, uint val) {
  uint old_value = cmp;
  bool const exchanged = atomic_compare_exchange_weak_explicit(atomic_ptr, (&old_value), val, memory_order_relaxed, memory_order_relaxed);
  return atomic_compare_exchange_result_u32{.old_value=old_value, .exchanged=exchanged};
}

void main_inner(uint3 gid, uint li, uint3 wid, uint3 nwg, tint_module_vars_struct tint_module_vars) {
  if ((li < 64u)) {
    (*tint_module_vars.shared_vals)[li] = 0u;
  }
  if ((li < 1u)) {
    atomic_store_explicit(tint_module_vars.wg_count, 0u, memory_order_relaxed);
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);
  uint const n = ((*tint_module_vars.tint_storage_buffer_sizes)[0u].x / 4u);
  uint const in_len = ((*tint_module_vars.tint_storage_buffer_sizes)[0u].z / 16u);
  (*tint_module_vars.seed) = hash(((*tint_module_vars.seed) + gid.x));
  float4 const v = (*tint_module_vars.input)[min((gid.x % max(in_len, 1u)), (in_len - 1u))];
  (*tint_module_vars.shared_vals)[min(li, 63u)] = (select(0u, 1u, (v.x > threshold)) + ((*tint_module_vars.seed) & 1u));
  atomic_fetch_add_explicit(tint_module_vars.wg_count, 1u, memory_order_relaxed);
  threadgroup_barrier(mem_flags::mem_threadgroup);
  uint sum = 0u;
  {
    uint2 tint_loop_idx = uint2(4294967295u);
    uint i = 0u;
    while(true) {
      TINT_ISOLATE_UB(tint_volatile_false)
      if (all((tint_loop_idx == uint2(0u)))) {
        break;
      }
      if ((i < wgSize)) {
      } else {
        break;
      }
      if ((i == li)) {
        {
          uint const tint_low_inc = (tint_loop_idx.x - 1u);
          tint_loop_idx.x = tint_low_inc;
          uint const tint_carry = uint((tint_low_inc == 4294967295u));
          tint_loop_idx.y = (tint_loop_idx.y - tint_carry);
          i = (i + 1u);
        }
        continue;
      }
      sum = (sum + (*tint_module_vars.shared_vals)[min(i, 63u)]);
      if ((sum > 1000u)) {
        break;
      }
      {
        uint const tint_low_inc = (tint_loop_idx.x - 1u);
        tint_loop_idx.x = tint_low_inc;
        uint const tint_carry = uint((tint_low_inc == 4294967295u));
        tint_loop_idx.y = (tint_loop_idx.y - tint_carry);
        i = (i + 1u);
      }
      continue;
    }
  }
  threadgroup_barrier(mem_flags::mem_threadgroup);
  uint const total = atomic_load_explicit(tint_module_vars.wg_count, memory_order_relaxed);
  threadgroup_barrier(mem_flags::mem_threadgroup);
  if ((gid.x < n)) {
    (*tint_module_vars.data).values[min(gid.x, (n - 1u))] = ((sum + total) + (wid.x * nwg.x));
  }
  atomic_fetch_add_explicit((&(*tint_module_vars.counters).hits), 1u, memory_order_relaxed);
  atomic_fetch_sub_explicit((&(*tint_module_vars.counters).total), int(li), memory_order_relaxed);
  atomic_fetch_max_explicit((&(*tint_module_vars.counters).maxv), sum, memory_order_relaxed);
  atomic_compare_exchange_result_u32 const old = cmpxchg((&(*tint_module_vars.counters).maxv), 0u, 1u);
  if (old.exchanged) {
    atomic_fetch_or_explicit((&(*tint_module_vars.counters).hits), 2147483648u, memory_order_relaxed);
  }
  threadgroup_barrier(mem_flags::mem_device);
}

kernel void tint_main(uint3 gid [[thread_position_in_grid]], uint li [[thread_index_in_threadgroup]],
                      uint3 wid [[threadgroup_position_in_grid]], uint3 nwg [[threadgroups_per_grid]],
                      device Data* data [[buffer(0)]], device Counters* counters [[buffer(1)]],
                      const device tint_array<float4, 1>* input [[buffer(2)]],
                      threadgroup tint_symbol_workgroup* v_1 [[threadgroup(0)]],
                      const constant tint_array<uint4, 1>* tint_storage_buffer_sizes [[buffer(30)]]) {
  thread uint seed = 7u;
  tint_module_vars_struct const tint_module_vars = tint_module_vars_struct{.data=data, .counters=counters, .input=input,
      .seed=(&seed), .shared_vals=(&(*v_1).shared_vals), .wg_count=(&(*v_1).wg_count),
      .tint_storage_buffer_sizes=tint_storage_buffer_sizes};
  main_inner(gid, li, wid, nwg, tint_module_vars);
}

// Bit and packing built-ins, read_write storage textures, half data and
// SIMD-group operations.
struct Params {
  /* 0x0000 */ float4 a;
  /* 0x0010 */ int4 b;
  /* 0x0020 */ float2x2 m;
  /* 0x0030 */ uint flags;
  /* 0x0034 */ tint_array<int8_t, 12> tint_pad;
};

struct H {
  /* 0x0000 */ half4 a;
  /* 0x0008 */ half b;
  /* 0x000a */ tint_array<int8_t, 2> tint_pad_1;
  /* 0x000c */ half2x2 m;
};

struct frexp_result_f32 {
  float fract;
  int exp;
};

struct modf_result_f32 {
  float fract;
  float whole;
};

uint tint_dot4_u8_packed(uint accumulator, uint b) {
  uint4 const v = uint4(as_type<uchar4>(accumulator));
  uint4 const w = uint4(as_type<uchar4>(b));
  return ((((v[0u] * w[0u]) + (v[1u] * w[1u])) + (v[2u] * w[2u])) + (v[3u] * w[3u]));
}

int tint_dot4_i8_packed(uint accumulator, uint b) {
  int4 const v = int4(as_type<char4>(accumulator));
  int4 const w = int4(as_type<char4>(b));
  return ((((v[0u] * w[0u]) + (v[1u] * w[1u])) + (v[2u] * w[2u])) + (v[3u] * w[3u]));
}

int tint_first_leading_bit(int value) {
  uint x = select(uint(value), ~uint(value), (value < 0));
  return select(int(31u - clz(x)), -1, (x == 0u));
}

kernel void math_main(uint3 lid [[thread_position_in_threadgroup]], const constant Params* p [[buffer(0)]],
                      device tint_array<float4, 16>* out [[buffer(1)]],
                      texture2d<float, access::write> img [[texture(0)]],
                      texture2d<uint, access::read_write> img2 [[texture(1)]],
                      device tint_array<H, 1>* hb [[buffer(2)]],
                      uint lane [[thread_index_in_simdgroup]], uint width [[threads_per_simdgroup]],
                      const constant tint_array<uint4, 1>* tint_storage_buffer_sizes [[buffer(30)]]) {
  uint const i = ((lid.y * 4u) + lid.x);
  float4 v = (*p).a;
  v.x = (v.x + float(i));
  int e = 0;
  float const fr_fract = frexp(v.y, e);
  frexp_result_f32 const fr = frexp_result_f32{.fract=fr_fract, .exp=e};
  float whole = 0.0f;
  float const mf = modf(v.z, whole);
  modf_result_f32 const pr = modf_result_f32{.fract=mf, .whole=whole};
  float4 r = float4(fr.fract, float(fr.exp), pr.fract, pr.whole);
  r = (r + float4(((*p).m * v.xy), 0.0f, 0.0f));
  r.x = (r.x + as_type<float>((as_type<uint>(v.w) ^ 1u)));
  r.y = (r.y + unpack_unorm4x8_to_float(pack_float_to_snorm4x8(v)).x + float2(as_type<half2>(as_type<uint>(half2(v.xy)))).y);
  r.z = (r.z + float((tint_first_leading_bit((*p).b.x) + ctz((*p).b.y)) + reverse_bits((*p).b.z) + extract_bits((*p).b.w, 2u, 3u)));
  r.w = (r.w + float(insert_bits((*p).flags, 5u, 4u, 2u) + clz((*p).flags) + ctz(((*p).flags | 1u)) + popcount((*p).flags)));
  r = (r + float4(dot(v, r), length(v.xyz), distance(v.xy, r.xy), determinant((*p).m)));
  r = (r + float4(cross(v.xyz, r.xyz), faceforward(v.xy, r.xy, float2(1.0f)).x));
  r = (r + reflect(v, normalize((r + 1.0f))) + refract(normalize((v + 1.0f)), normalize((r + 2.0f)), 0.5f));
  r = (r + float4(smoothstep(0.0f, 1.0f, v.x), step(0.5f, v.y), mix(v.z, r.z, 0.25f), fma(v.w, r.w, 1.0f)));
  r = (r + float4(atan2(v.y, v.x), asinh(v.z), acosh((abs(v.w) + 1.0f)), atanh(clamp(v.x, -0.5f, 0.5f))));
  r = (r + float4((v.x * 57.295779513082322865f), (v.y * 0.017453292519943295474f), rsqrt((abs(v.z) + 1.0f)), ldexp(v.w, 2)));
  r = (((((((r + float4(half4(v))) + saturate(r)) + sign(v)) + trunc(v)) + rint(v)) + floor(v)) + ceil(v));
  r.x = (r.x + float((tint_dot4_u8_packed((*p).flags, 16909060u) + uint(tint_dot4_i8_packed((*p).flags, 2139062143u)))));
  switch((i % 5u)) {
    case 0u: { r.x = -(r.x); break; }
    case 1u:
    case 2u: { r.y = (r.y * 3.0f); break; }
    case 3u: { r = transpose(float4x4(r, r.yzwx, r.zwxy, r.wxyz))[1u]; break; }
    default: { r.w = 0.0f; break; }
  }
  (*out)[min(i, 15u)] = r;
  img.write(clamp(r, float4(0.0f), float4(1.0f)), uint2(int2(lid.xy)));
  uint4 const prev = img2.read(uint2(int2(lid.xy)));
  img2.write((prev + uint4(i)), uint2(int2(lid.xy)));
  img2.fence();
  uint const hlen = ((*tint_storage_buffer_sizes)[0u].y / 16u);
  uint const hi = min(i, (hlen - 1u));
  H h = (*hb)[hi];
  h.a = ((h.a * h.b) + half4(1.0h, 0.5h, 0.25h, 2.0h));
  h.m = (h.m * h.b);
  float const x = (float(h.a.x) + float(dot(h.a.xy, h.m[0u])));
  h.b = half(x);
  (*hb)[hi] = h;
  uint const s = simd_sum(i);
  uint const mx = simd_max(i);
  uint4 const ballot = uint4(as_type<uint2>(static_cast<ulong>(simd_ballot((i > 3u)))), 0u, 0u);
  uint const first = simd_broadcast_first(i);
  uint const sh = simd_shuffle(i, ((lane + 1u) % width));
  uint const ex = simd_prefix_exclusive_sum(i);
  (*out)[0u].x = (*out)[0u].x + float(s + mx + ballot.x + first + sh + ex + select(0u, 1u, simd_is_first()) + select(0u, 2u, simd_all((i < 100u))));
}
