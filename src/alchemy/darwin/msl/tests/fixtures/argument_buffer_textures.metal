// Argument buffers holding textures and samplers (tier 2, bindless): texture,
// sampler and buffer members, arrays indexed at run time, a nested pointer
// to another argument buffer, and a writable texture in a kernel.
#include <metal_stdlib>
using namespace metal;

struct Material {
    texture2d<float> albedo [[id(0)]];
    sampler linear [[id(1)]];
    constant float4* tint [[id(2)]];
    array<texture2d<float>, 4> layers [[id(3)]];
    depth2d<float> shadow [[id(7)]];
    sampler compare [[id(8)]];
};

struct Scene {
    device Material* materials [[id(0)]];
    texturecube<float> sky [[id(1)]];
};

struct VertexOut {
    float4 position [[position]];
    float2 uv;
    uint material [[flat]];
    float3 shadow_uv;
};

fragment float4 shade(VertexOut in [[stage_in]], constant Scene& scene [[buffer(0)]],
                      constant uint& layer [[buffer(1)]]) {
    device Material& m = scene.materials[in.material];
    float4 colour = m.albedo.sample(m.linear, in.uv) * *m.tint;
    colour += m.layers[layer & 3].sample(m.linear, in.uv);
    colour *= m.shadow.sample_compare(m.compare, in.shadow_uv.xy, in.shadow_uv.z);
    colour += scene.sky.sample(m.linear, float3(in.uv, 1.0));
    return colour;
}

struct Targets {
    texture2d<float, access::write> output [[id(0)]];
    texture2d<uint, access::read> counts [[id(1)]];
};

kernel void resolve(constant Targets& targets [[buffer(0)]], uint2 gid [[thread_position_in_grid]]) {
    const uint n = targets.counts.read(gid).x;
    targets.output.write(float4(float(n) / 255.0), gid);
    if (gid.x == 0) targets.output.write(float4(float(targets.output.get_width())), gid);
}
