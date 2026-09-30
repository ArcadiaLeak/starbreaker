[[vk::binding(0, 2)]]
Sampler2D textTexture : register(t0, space2) : register(s0, space2);

struct VSOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

[shader("vertex")]
VSOutput vs_main(uint vertexID : SV_VulkanVertexID)
{
    float2 pos;
    if (vertexID == 0)      pos = float2(-1.0, -1.0);
    else if (vertexID == 1) pos = float2( 3.0, -1.0);
    else                    pos = float2(-1.0,  3.0);

    VSOutput o;
    o.position = float4(pos, 0.0, 1.0);
    o.uv       = float2((pos.x + 1.0) * 0.5, (1.0 - pos.y) * 0.5);
    return o;
}

[shader("fragment")]
float4 fs_main(VSOutput input) : SV_Target
{
    return textTexture.Sample(input.uv);
}
