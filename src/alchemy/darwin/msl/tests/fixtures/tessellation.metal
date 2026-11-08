#include <metal_stdlib>
using namespace metal;

// Tessellation (M5): a kernel that writes quad factors, post-tessellation
// vertex functions for both patch kinds, and a fragment function.
struct ControlPoint {
    float4 position [[attribute(0)]];
    float2 uv [[attribute(1)]];
};

struct PatchIn {
    patch_control_point<ControlPoint> control_points;
    float4 tint [[attribute(2)]];
};

struct VertexOut {
    float4 position [[position]];
    float2 uv;
    float4 tint;
    uint id [[flat]];
};

kernel void quad_factors(device MTLQuadTessellationFactorsHalf* factors [[buffer(0)]],
                         constant float& level [[buffer(1)]],
                         uint patch [[thread_position_in_grid]]) {
    for (int edge = 0; edge < 4; ++edge) factors[patch].edgeTessellationFactor[edge] = half(level);
    factors[patch].insideTessellationFactor[0] = half(level);
    factors[patch].insideTessellationFactor[1] = half(level);
}

[[patch(quad, 4)]]
vertex VertexOut quad_patch(PatchIn in [[stage_in]], float2 uv [[position_in_patch]], uint patch [[patch_id]],
                            uint instance [[instance_id]]) {
    const auto points = in.control_points;
    const float4 bottom = mix(points[0].position, points[1].position, uv.x);
    const float4 top = mix(points[3].position, points[2].position, uv.x);
    VertexOut out;
    out.position = mix(bottom, top, uv.y);
    out.uv = mix(mix(points[0].uv, points[1].uv, uv.x), mix(points[3].uv, points[2].uv, uv.x), uv.y);
    out.tint = in.tint;
    out.id = patch * 16 + instance;
    return out;
}

[[patch(triangle, 3)]]
vertex float4 triangle_patch(patch_control_point<ControlPoint> points [[stage_in]], float3 weights [[position_in_patch]]) {
    float4 position = float4(0.0f);
    for (uint i = 0; i < points.size(); ++i) position += points[i].position * weights[i];
    return position;
}

fragment float4 patch_color(VertexOut in [[stage_in]]) {
    return in.tint * float4(in.uv, float(in.id & 1u), 1.0f);
}
