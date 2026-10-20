struct SolidUniform {
    float4 color;
};

[[vk::binding(0, 3)]]
ConstantBuffer<SolidUniform> u;

[shader("fragment")]
float4 main() : SV_Target {
    return u.color;
}
