// WebGL 2 uniform blocks the way ANGLE's Metal back end passes them when it
// uses argument buffers (design: ANGLE's public translator): one argument
// buffer whose [[id(n)]] members are constant pointers to the blocks, each
// written by an MTLArgumentEncoder (setBuffer:offset:atIndex:), plus an array
// of block pointers for a uniform block array.
#include <metal_stdlib>
using namespace metal;

struct ANGLE_ubo_Lights
{
    metal::float4 _ucolor[4];
    metal::float4 _uposition[4];
    int _ucount;
};

struct ANGLE_ubo_Material
{
    metal::float4x4 _umodel;
    metal::float4 _ualbedo;
    float _ushininess;
};

struct ANGLE_ubo_Bones
{
    metal::float4x4 _ubone[2];
};

struct ANGLE_UBOArgumentBufferStruct
{
    constant ANGLE_ubo_Lights* _uLights [[id(0)]];
    constant ANGLE_ubo_Material* _uMaterial [[id(1)]];
    constant ANGLE_ubo_Bones* _uBones[2] [[id(2)]];
};

struct ANGLE_VertexOut
{
    metal::float4 _uvColor [[user(_uvColor)]];
    metal::float4 gl_Position [[position]];
};

struct ANGLE_VertexIn
{
    metal::float3 _uaPosition [[attribute(0)]];
    uint32_t _uaBone [[attribute(1)]];
};

vertex ANGLE_VertexOut ab_vertex(ANGLE_VertexIn ANGLE_vertexIn [[stage_in]],
    constant ANGLE_UBOArgumentBufferStruct & ANGLE_argumentBufs [[buffer(0)]],
    uint32_t ANGLE_instanceIdMod [[instance_id]])
{
    ANGLE_VertexOut ANGLE_vertexOut;
    constant ANGLE_ubo_Material& m = *ANGLE_argumentBufs._uMaterial;
    constant ANGLE_ubo_Bones& b = *ANGLE_argumentBufs._uBones[ANGLE_instanceIdMod & 1u];
    metal::float4 p = m._umodel * (b._ubone[metal::min(ANGLE_vertexIn._uaBone, 1u)] * metal::float4(ANGLE_vertexIn._uaPosition, 1.0f));
    ANGLE_vertexOut.gl_Position = p;
    metal::float4 c = metal::float4(0.0f);
    for (int i = 0; i < metal::clamp(ANGLE_argumentBufs._uLights->_ucount, 0, 4); ++i)
    {
        metal::float3 d = ANGLE_argumentBufs._uLights->_uposition[i].xyz - p.xyz;
        c += ANGLE_argumentBufs._uLights->_ucolor[i] / (1.0f + metal::dot(d, d));
    }
    ANGLE_vertexOut._uvColor = c * m._ualbedo * m._ushininess;
    return ANGLE_vertexOut;
}
