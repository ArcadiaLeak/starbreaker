struct VSOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

[[vk::binding(0, 2)]]
Sampler2D<float> textTex;

struct BlitUniforms {
    float4 tint;
};

[[vk::binding(0, 3)]]
ConstantBuffer<BlitUniforms> u;

[shader("fragment")]
float4 fs_main(VSOutput input) : SV_Target
{
    float coverage = textTex.Sample(input.uv);
    return float4(u.tint.rgb, u.tint.a * coverage);
}
