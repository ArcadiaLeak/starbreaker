[[vk::binding(0, 2)]]
Sampler2D captionTexture : register(t0, space2);

struct VSOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

[shader("fragment")]
float4 fs_main(VSOutput input) : SV_Target
{
    return captionTexture.Sample(input.uv);
}
