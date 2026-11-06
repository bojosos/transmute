// [[barycentric_coord]] as a fragment parameter and as a [[stage_in]] member,
// with perspective (default) and center_no_perspective interpolation.
#include <metal_stdlib>
using namespace metal;

struct VertexIn {
    float3 position [[attribute(0)]];
    float4 color [[attribute(1)]];
};

struct VertexOut {
    float4 position [[position]];
    float4 color;
};

vertex VertexOut wire_vertex(VertexIn in [[stage_in]]) {
    VertexOut out;
    out.position = float4(in.position, 1.0);
    out.color = in.color;
    return out;
}

// A wireframe overlay: the distance to the nearest edge from the barycentrics.
fragment float4 wire_fragment(VertexOut in [[stage_in]], float3 bary [[barycentric_coord]],
                              constant float& width [[buffer(0)]]) {
    const float3 d = fwidth(bary);
    const float3 edge = smoothstep(float3(0.0), d * width, bary);
    const float line = 1.0 - min(min(edge.x, edge.y), edge.z);
    return mix(in.color, float4(1.0), line);
}

struct LinearIn {
    float4 position [[position]];
    float3 weights [[barycentric_coord, center_no_perspective]];
};

fragment float4 linear_weights(LinearIn in [[stage_in]]) { return float4(in.weights, 1.0); }
