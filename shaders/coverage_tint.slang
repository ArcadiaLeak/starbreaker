struct VSOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

struct BlitUniform {
    float4 tint;
};

[[vk::binding(0, 2)]]
Sampler2D<float> coverageTexture;

[[vk::binding(0, 3)]]
ConstantBuffer<BlitUniform> u;

[shader("fragment")]
float4 main(VSOutput input) : SV_Target {
    float coverage = coverageTexture.Sample(input.uv);
    return float4(u.tint.rgb, u.tint.a * coverage);
}
