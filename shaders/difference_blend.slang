[[vk::binding(0, 0)]] Texture2D<float4> baseTexture;
[[vk::binding(1, 0)]] Texture2D<float4> maskTexture;

[[vk::binding(0, 1)]] RWTexture2D<float4> diffTexture;

[numthreads(8, 8, 1)]
void main(uint3 threadID : SV_DispatchThreadID) {
    int3 coords = int3(threadID.xy, 0);
    float4 baseColor = baseTexture.Load(coords);
    float4 mask = maskTexture.Load(coords);
    if (mask.a == 0.0) {
        diffTexture[threadID.xy] = baseColor;
        return;
    }
    float3 diffColor = lerp(
        baseColor.rgb, abs(mask.rgb - baseColor.rgb), mask.a);
    diffTexture[threadID.xy] = float4(diffColor, baseColor.a);
}
