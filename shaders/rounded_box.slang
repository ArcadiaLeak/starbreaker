struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

struct BoxUniform {
    float4 blend_color;
    float radius;
};

[[vk::binding(0, 3)]]
ConstantBuffer<BoxUniform> u;

float find_distance(float2 p, float r) {
    float2 q = abs(p) - 1.0 + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

[shader("fragment")]
float4 main(VSOutput input) : SV_Target {
    float2 p = input.uv * 2.0 - 1.0;
    if (find_distance(p, 0.0) > 0.0)
        discard;
    return float4(u.blend_color.rgb, 1.0);
}
