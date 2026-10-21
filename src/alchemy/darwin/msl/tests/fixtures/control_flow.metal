// Control flow, matrices, half, packed types, structs in buffers.
#include <metal_stdlib>
using namespace metal;

struct Light {
    float4x4 matrix;
    packed_float3 color;
    float power;
    half4 tint;
    int type;
    uint flags;
};

struct ObjectInfos {
    float3x3 normal_matrix;
    float4 bounds[2];
};

constant int light_limit [[function_constant(3)]];
constant bool has_shadows [[function_constant(4)]];
constant float exposure [[function_constant(5)]];

float3 eval_light(const device Light& l, float3 P, float3 N) {
    float3 L = l.matrix[3].xyz - P;
    float dist = length(L);
    L /= dist;
    float ndotl = saturate(dot(N, L));
    switch (l.type) {
    case 0: return float3(l.color) * l.power * ndotl / (dist * dist);
    case 1: return float3(l.color) * ndotl;
    case 2: {
        float spot = smoothstep(0.8, 0.9, dot(-L, normalize(l.matrix[2].xyz)));
        return float3(l.color) * spot * ndotl;
    }
    default: break;
    }
    return float3(0.0);
}

struct VIn {
    float3 pos [[attribute(0)]];
    float3 nor [[attribute(1)]];
    half2 uv [[attribute(2)]];
};

struct VOut {
    float4 pos [[position]];
    float3 world_pos [[user(world)]];
    float3 normal [[user(nor)]] [[center_no_perspective]];
    half2 uv;
    float clip [[clip_distance]] [[function_constant(has_shadows)]];
};

vertex VOut mesh_vert(VIn in [[stage_in]],
                      constant float4x4& view_projection [[buffer(0)]],
                      const device ObjectInfos* infos [[buffer(1)]],
                      constant float4* clip_planes [[buffer(2), function_constant(has_shadows)]],
                      uint instance [[instance_id]],
                      uint base_instance [[base_instance]]) {
    VOut out;
    const device ObjectInfos& info = infos[instance + base_instance];
    float3 world = info.bounds[0].xyz + in.pos * info.bounds[1].xyz;
    out.pos = view_projection * float4(world, 1.0);
    out.world_pos = world;
    out.normal = normalize(info.normal_matrix * in.nor);
    out.uv = in.uv;
    if (has_shadows) out.clip = dot(float4(world, 1.0), clip_planes[0]);
    return out;
}

fragment half4 mesh_frag(VOut in [[stage_in]],
                         const device Light* lights [[buffer(0)]],
                         constant uint& light_count [[buffer(1)]],
                         device uint* debug [[buffer(2)]],
                         uint sample_id [[sample_id]]) {
    float3 N = normalize(in.normal);
    float3 radiance = 0.0;
    uint count = min(light_count, uint(light_limit));
    for (uint i = 0; i < count; ++i) {
        const device Light& l = lights[i];
        if ((l.flags & 1u) == 0u) continue;
        float3 c = eval_light(l, in.world_pos, N);
        if (any(isnan(c))) {
            debug[0] = i;
            break;
        }
        radiance += c * float3(l.tint.rgb);
        int bounce = 0;
        while (bounce < 4 && length(c) > 0.01) {
            c *= 0.5;
            radiance += c;
            bounce++;
            if (bounce == 3 && l.type == 2) return half4(1.0h, 0.0h, 1.0h, 1.0h);
        }
    }
    float3x3 m = float3x3(float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1));
    radiance = transpose(m) * radiance * exposure + float3(float(sample_id & 1u) * 0.001);
    return half4(half3(radiance), half(in.uv.x));
}
