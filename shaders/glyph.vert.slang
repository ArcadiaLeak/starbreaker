struct VSOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

struct GlyphUniforms {
    float2 viewport;
    float2 rect_pos;
    float2 rect_size;
};

[[vk::binding(0, 1)]]
ConstantBuffer<GlyphUniforms> u;

[shader("vertex")]
VSOutput vertexMain(uint vertexID : SV_VertexID)
{
    VSOutput output;

    uint i = vertexID & 3u;
    float2 corner = float2(float(i & 1u), float(i >> 1u));
    float2 pixel = u.rect_pos + corner * u.rect_size;
    float2 ndc = pixel * (2.0 / u.viewport) - 1.0;

    output.position = float4(ndc, 0.0, 1.0);
    output.uv       = corner;
    return output;
}
