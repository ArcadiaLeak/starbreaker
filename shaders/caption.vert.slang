struct VSOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

struct GlyphUniforms {
    float2 viewport;
};

[[vk::binding(0, 1)]]
ConstantBuffer<GlyphUniforms> u;

struct InstanceIn {
    float2 rect_pos  : TEXCOORD1;
    float2 rect_size : TEXCOORD2;
    float4 uv_rect   : TEXCOORD3;
};

[shader("vertex")]
VSOutput vs_main(uint vertexID : SV_VulkanVertexID, InstanceIn inst)
{
    uint i = vertexID & 3u;
    float2 corner = float2(float(i & 1u), float(i >> 1u));
    float2 pixel = inst.rect_pos + corner * inst.rect_size;
    float2 ndc = pixel * (2.0 / u.viewport) - 1.0;

    VSOutput o;
    o.position = float4(ndc, 0.0, 1.0);
    o.uv = lerp(inst.uv_rect.xy, inst.uv_rect.zw, corner);
    return o;
}
