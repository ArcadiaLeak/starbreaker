struct DifferenceBlendArea {
    uint4 rect;
    float4 color;
};
struct DifferenceBlendUniform {
    uint length;
};

[[vk::binding(0, 0)]] Texture2D<float4> baseTexture;
[[vk::binding(1, 0)]] StructuredBuffer<DifferenceBlendArea> areaBuf;

[[vk::binding(0, 1)]] RWTexture2D<float4> diffTexture;
[[vk::binding(0, 2)]] ConstantBuffer<DifferenceBlendUniform> uni;

[numthreads(8, 8, 1)]
void main(uint3 threadID : SV_DispatchThreadID) {
    var baseCoords = int3(threadID.xy, 0);
    var baseColor = baseTexture.Load(baseCoords);
    for (uint i = 0; i < uni.length; ++i) {
        DifferenceBlendArea area = areaBuf[i];
        uint2 cornerMin = area.rect.xy;
        uint2 cornerMax = area.rect.xy + area.rect.zw;
        bool insideMin = all(threadID.xy >= cornerMin);
        bool insideMax = all(threadID.xy < cornerMax);
        if (!insideMin || !insideMax)
            continue;
        diffTexture[threadID.xy] = float4(
            abs(area.color.rgb - baseColor.rgb),
            baseColor.a);
        return;
    }
    diffTexture[threadID.xy] = baseColor;
}
