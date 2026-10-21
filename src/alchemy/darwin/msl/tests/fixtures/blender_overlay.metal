// Vertex pulling from storage buffers, SSBO-style structs, early fragment
// tests, sample mask/depth outputs, simd ops in fragments, bit casts
// (Blender overlay/workbench shaders compiled through mtl_shader_generator).
#include <metal_stdlib>
using namespace metal;

#define vec2 float2
#define vec3 float3
#define vec4 float4
#define uint2_t uint2
#define floatBitsToUint(x) as_type<uint>(x)
#define uintBitsToFloat(x) as_type<float>(x)

constant int MTL_uniform_buffer_base_index [[function_constant(0)]];
constant int MTL_storage_buffer_base_index [[function_constant(1)]];

struct DrawData {
    float4x4 model;
    float4x4 model_inverse;
    uint resource_id;
    uint flag;
    float2 _pad;
};

struct ViewMatrices {
    float4x4 viewmat;
    float4x4 viewinv;
    float4x4 winmat;
    float4x4 wininv;
    float4 clip_planes[6];
    float4 viewport_size;
};

struct VOut {
    vec4 pos [[position]];
    vec3 nor [[user(locn0)]] [[sample_perspective]];
    vec2 uv [[user(locn1)]] [[centroid_perspective]];
    uint id [[user(locn2)]] [[flat]];
    float psize [[point_size]];
};

vertex VOut overlay_vert(const device float* positions [[buffer(MTL_storage_buffer_base_index + 0)]],
                         const device uint* normals [[buffer(MTL_storage_buffer_base_index + 1)]],
                         const device DrawData* draws [[buffer(MTL_storage_buffer_base_index + 2)]],
                         constant ViewMatrices& view [[buffer(MTL_uniform_buffer_base_index + 0)]],
                         uint vid [[vertex_id]],
                         uint iid [[instance_id]]) {
    VOut out;
    const device DrawData& d = draws[iid];
    vec3 p = vec3(positions[vid * 3 + 0], positions[vid * 3 + 1], positions[vid * 3 + 2]);
    uint packed = normals[vid];
    vec3 n = vec3(float(packed & 0x3ffu), float((packed >> 10) & 0x3ffu), float((packed >> 20) & 0x3ffu)) / 511.5 - 1.0;
    vec4 world = d.model * vec4(p, 1.0);
    out.pos = view.winmat * (view.viewmat * world);
    out.nor = normalize((transpose(d.model_inverse) * vec4(n, 0.0)).xyz);
    out.uv = p.xy * 0.5 + 0.5;
    out.id = d.resource_id ^ floatBitsToUint(1.0f);
    out.psize = select(1.0f, 3.0f, (d.flag & 2u) != 0u);
    return out;
}

struct FOut {
    vec4 color [[color(0)]];
    vec4 blend [[color(0), index(1)]];
    uint sample_mask [[sample_mask]];
    float depth [[depth(less)]];
};

[[early_fragment_tests]] fragment vec4 prepass_frag(VOut in [[stage_in]]) {
    return vec4(in.nor * 0.5 + 0.5, 1.0);
}

fragment FOut outline_frag(VOut in [[stage_in]],
                           vec4 frag_coord [[position]],
                           uint mask_in [[sample_mask]],
                           uint lane [[thread_index_in_simdgroup]],
                           device atomic_uint* stats [[buffer(MTL_storage_buffer_base_index + 3)]]) {
    FOut out;
    float edge = fwidth(in.uv.x) + abs(dfdx(in.uv.y));
    bool any_edge = simd_any(edge > 0.1);
    float neighbor = simd_shuffle_xor(edge, 1u);
    out.color = vec4(in.nor, any_edge ? 1.0 : neighbor);
    out.blend = vec4(edge);
    out.sample_mask = mask_in & 0x1u;
    out.depth = frag_coord.z * 0.999;
    if (simd_is_first()) atomic_fetch_add_explicit(stats, simd_sum(1u), memory_order_relaxed);
    if (lane == 0 && popcount(in.id) > 20) out.color.a = uintBitsToFloat(0x3f000000u);
    return out;
}

kernel void prefix_sum(device uint* data [[buffer(0)]],
                       constant uint& n [[buffer(1)]],
                       uint tid [[thread_index_in_threadgroup]],
                       uint gid [[thread_position_in_grid]],
                       uint simd_size [[threads_per_simdgroup]])
    [[max_total_threads_per_threadgroup(256)]] {
    threadgroup uint shared_sums[256];
    uint v = gid < n ? data[gid] : 0u;
    uint scan = simd_prefix_exclusive_sum(v);
    shared_sums[tid] = scan + v;
    threadgroup_barrier(mem_flags::mem_threadgroup);
    for (uint offset = 1; offset < 256; offset <<= 1) {
        uint add = tid >= offset ? shared_sums[tid - offset] : 0u;
        threadgroup_barrier(mem_flags::mem_threadgroup);
        shared_sums[tid] += add;
        threadgroup_barrier(mem_flags::mem_threadgroup);
    }
    if (gid < n) data[gid] = shared_sums[tid] - v + simd_broadcast(v, 0) * 0u + simd_size * 0u;
}
