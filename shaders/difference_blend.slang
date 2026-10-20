struct SelectionRect {
    uint4 rect;
    float4 color;
};

[[vk::binding(0, 0)]] Texture2D<float4> baseTexture;
[[vk::binding(0, 1)]] RWTexture2D<float4> outputTexture;
[[vk::binding(0, 2)]] ConstantBuffer<SelectionRect> selection;

[numthreads(8, 8, 1)]
void main(uint3 threadID : SV_DispatchThreadID) {
    if (threadID.x >= selection.rect.z || threadID.y >= selection.rect.w)
        return;
    uint2 baseCoords = selection.rect.xy + threadID.xy;
    float4 baseColor = baseTexture.Load(int3(baseCoords, 0));
    outputTexture[baseCoords] = float4(
        abs(baseColor.rgb - selection.color.rgb),
        baseColor.a);
}
