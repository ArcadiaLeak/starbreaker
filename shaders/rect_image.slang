struct VSOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

struct RectUniform {
    float4 uv_rect;
    float2 rect_size;
    float2 rect_position;
    float2 viewport;
};
[[vk::binding(0, 1)]]
ConstantBuffer<RectUniform> rectUni;

[shader("vertex")]
VSOutput main(uint vertexID : SV_VulkanVertexID) {
    uint i = vertexID & 3u;
    float2 corner = float2(float(i & 1u), float(i >> 1u));
    float2 pixelCoords = rectUni.rect_position + corner * rectUni.rect_size;
    float2 deviceCoords = pixelCoords * (2.0 / rectUni.viewport) - 1.0;

    VSOutput o;
    o.position = float4(deviceCoords, 0.0, 1.0);
    o.uv = lerp(rectUni.uv_rect.xw, rectUni.uv_rect.zy, corner);
    return o;
}
