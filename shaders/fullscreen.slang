[[vk::binding(0, 2)]]
Sampler2D atlasTexture : register(t0, space2) : register(s0, space2);

struct VSInput {
    float2 position : TEXCOORD0;
    float2 uv       : TEXCOORD1;
};

struct VSOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

[shader("vertex")]
VSOutput vs_main(VSInput input)
{
    VSOutput o;
    o.position = float4(input.position, 0.0, 1.0);
    o.uv       = input.uv;
    return o;
}

[shader("fragment")]
float4 fs_main(VSOutput input) : SV_Target
{
    return atlasTexture.Sample(input.uv);
}
