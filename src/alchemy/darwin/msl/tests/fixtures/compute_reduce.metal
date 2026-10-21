// Compute: threadgroup memory, barriers, atomics, simdgroup reductions,
// function constants (Blender's compute shaders, e.g. EEVEE light culling).
#include <metal_stdlib>
using namespace metal;

constant uint group_size [[function_constant(0)]];
constant bool use_simd [[function_constant(1)]];

struct Params {
    uint count;
    float scale;
    packed_float3 bias;
    uint _pad;
};

kernel void reduce_sum(device const float* input [[buffer(0)]],
                       device atomic_uint* counter [[buffer(1)]],
                       device float* partial [[buffer(2)]],
                       constant Params& params [[buffer(3)]],
                       threadgroup float* scratch [[threadgroup(0)]],
                       uint gid [[thread_position_in_grid]],
                       uint lid [[thread_position_in_threadgroup]],
                       uint group [[threadgroup_position_in_grid]],
                       uint simd_lane [[thread_index_in_simdgroup]],
                       uint simd_id [[simdgroup_index_in_threadgroup]],
                       uint tg_size [[threads_per_threadgroup]]) {
    float value = gid < params.count ? input[gid] * params.scale + params.bias.x : 0.0f;
    if (use_simd) {
        value = simd_sum(value);
        if (simd_lane == 0) scratch[simd_id] = value;
        threadgroup_barrier(mem_flags::mem_threadgroup);
        if (lid == 0) {
            float total = 0.0f;
            for (uint i = 0; i < (tg_size + 31) / 32; ++i) total += scratch[i];
            partial[group] = total;
            atomic_fetch_add_explicit(counter, 1u, memory_order_relaxed);
        }
        return;
    }
    scratch[lid] = value;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint stride = tg_size / 2; stride > 0; stride >>= 1) {
        if (lid < stride) scratch[lid] += scratch[lid + stride];
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (lid == 0) {
        partial[group] = scratch[0];
        atomic_fetch_add_explicit(counter, 1u, memory_order_relaxed);
    }
}

kernel void histogram(texture2d<float, access::read> image [[texture(0)]],
                      device atomic_uint* bins [[buffer(0)]],
                      uint2 gid [[thread_position_in_grid]]) {
    threadgroup atomic_uint local_bins[64];
    if (gid.x >= image.get_width() || gid.y >= image.get_height()) return;
    const float4 c = image.read(gid);
    const float luma = dot(c.rgb, float3(0.2126f, 0.7152f, 0.0722f));
    const uint bin = min(uint(luma * 63.0f), 63u);
    atomic_fetch_add_explicit(&local_bins[bin], 1u, memory_order_relaxed);
    threadgroup_barrier(mem_flags::mem_threadgroup);
    uint count = atomic_load_explicit(&local_bins[bin], memory_order_relaxed);
    atomic_fetch_max_explicit(&bins[bin], count, memory_order_relaxed);
}
