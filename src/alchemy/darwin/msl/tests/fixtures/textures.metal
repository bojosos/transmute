// Textures and samplers as Blender's draw/overlay shaders use them.
#include <metal_stdlib>
using namespace metal;

constexpr sampler linear_clamp(coord::normalized, address::clamp_to_edge, filter::linear, mip_filter::linear);
constexpr sampler shadow_cmp(filter::linear, compare_func::less_equal);

struct VOut {
    float4 position [[position]];
    float2 uv;
    float layer [[flat]];
};

vertex VOut fullscreen_vert(uint vid [[vertex_id]]) {
    VOut out;
    float2 p = float2((vid << 1) & 2, vid & 2);
    out.position = float4(p * 2.0 - 1.0, 0.0, 1.0);
    out.uv = p;
    out.layer = 0.0;
    return out;
}

struct FOut {
    float4 color [[color(0)]];
    float4 normal [[color(1)]];
    float depth [[depth(any)]];
};

fragment FOut composite_frag(VOut in [[stage_in]],
                             texture2d<float> color_tx [[texture(0)]],
                             depth2d<float> depth_tx [[texture(1)]],
                             texture2d_array<float> layers_tx [[texture(2)]],
                             texturecube<float> env_tx [[texture(3)]],
                             texture3d<half> volume_tx [[texture(4)]],
                             texture2d<uint, access::read> ids_tx [[texture(5)]],
                             array<texture2d<float>, 4> extra_tx [[texture(6)]],
                             texture2d_ms<float> ms_tx [[texture(10)]],
                             sampler smp [[sampler(0)]],
                             float4 frag_coord [[position]]) {
    FOut out;
    float4 c = color_tx.sample(smp, in.uv);
    c += color_tx.sample(linear_clamp, in.uv, level(2.0));
    c += color_tx.sample(smp, in.uv, bias(0.5));
    c += color_tx.sample(smp, in.uv, int2(1, -1));
    c += layers_tx.sample(smp, in.uv, uint(in.layer));
    c += env_tx.sample(smp, float3(in.uv, 1.0));
    c += float4(volume_tx.sample(smp, float3(in.uv, 0.5)));
    c += color_tx.gather(smp, in.uv);
    float shadow = depth_tx.sample_compare(shadow_cmp, in.uv, 0.5);
    float d = depth_tx.read(uint2(frag_coord.xy));
    uint id = ids_tx.read(uint2(frag_coord.xy)).r;
    for (int i = 0; i < 4; ++i) c += extra_tx[i].sample(smp, in.uv);
    c += ms_tx.read(uint2(frag_coord.xy), 0);
    float2 size = float2(color_tx.get_width(), color_tx.get_height());
    c.xy += size * 0.0001 + float(color_tx.get_num_mip_levels());
    out.color = c * shadow;
    out.normal = float4(float(id & 0xffu) / 255.0, dfdx(in.uv.x), dfdy(in.uv.y), fwidth(d));
    out.depth = d;
    return out;
}

kernel void blur_image(texture2d<float, access::read> src [[texture(0)]],
                       texture2d<float, access::write> dst [[texture(1)]],
                       texture2d<float, access::read_write> accum [[texture(2)]],
                       texture2d<uint, access::read_write> counts [[texture(3)]],
                       uint2 gid [[thread_position_in_grid]]) {
    if (gid.x >= dst.get_width() || gid.y >= dst.get_height()) return;
    float4 sum = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            sum += src.read(uint2(clamp(int2(gid) + int2(x, y), int2(0), int2(src.get_width() - 1, src.get_height() - 1))));
    dst.write(sum / 9.0, gid);
    accum.write(accum.read(gid) + sum, gid);
    counts.atomic_fetch_add(gid, 1u);
}

// texelFetch as GLSL-on-Metal layers write it: a pixel-coordinate nearest
// sampler at an explicit level, with offsets and array layers. Lowered to
// OpImageFetch (Vulkan forbids lod/offset/arrays with unnormalized samplers).
constexpr sampler fetch_samp(address::clamp_to_zero, filter::nearest, coord::pixel);

fragment float4 texel_fetch_frag(VOut in [[stage_in]],
                                 texture2d<float> color_tx [[texture(0)]],
                                 texture2d_array<uint> ids_tx [[texture(1)]],
                                 float4 frag_coord [[position]]) {
    const float2 p = floor(frag_coord.xy);
    float4 c = color_tx.sample(fetch_samp, p, level(1.0));
    c += color_tx.sample(fetch_samp, p, level(0.0), int2(-1, 1));
    const uint4 id = ids_tx.sample(fetch_samp, p, 2u, level(0.0));
    return c + float4(id) * 0.001;
}
