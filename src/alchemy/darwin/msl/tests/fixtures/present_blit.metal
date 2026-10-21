// A window presentation blit as a host application might embed it: no
// #include (Metal makes metal_stdlib implicit), an [[attribute(n)]] member
// in the vertex output struct (ignored on outputs), a brace-initialised
// default constexpr sampler.
using namespace metal;

struct BlitVertex {
    float4 position [[position]];
    float2 uv [[attribute(0)]];
};

vertex BlitVertex blit_vertex(uint vid [[vertex_id]]) {
    BlitVertex out;
    out.position = float4(float(vid & 1) * 4.0 - 1.0, float(vid >> 1) * 4.0 - 1.0, 0.0, 1.0);
    out.uv = out.position.xy * 0.5 + 0.5;
    return out;
}

constexpr sampler blit_sampler {};

fragment float4 blit_fragment(BlitVertex in [[stage_in]], texture2d<float> source [[texture(0)]]) {
    float4 color = source.sample(blit_sampler, in.uv);
    color.a = 1.0;
    return color;
}
