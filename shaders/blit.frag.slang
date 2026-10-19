struct VSOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

[[vk::binding(0, 2)]]
Sampler2D blitTexture;

[shader("fragment")]
float4 fs_main(VSOutput input) : SV_Target {
    return blitTexture.Sample(input.uv);
}
