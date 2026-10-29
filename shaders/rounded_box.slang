struct VSOutput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    nointerpolation float2 half_size : TEXCOORD1;
};

struct BoxUniform {
    float4 blend_color;
    float radius;
};

[[vk::binding(0, 3)]]
ConstantBuffer<BoxUniform> u;

float sd_round_box(float2 p, float2 b, float r) {
    float2 q = abs(p) - b + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, float2(0.0))) - r;
}

[shader("fragment")]
float4 main(VSOutput input) : SV_Target {
    float2 point = (input.uv * 2.0 - 1.0) * input.half_size;
    float distance = sd_round_box(
        point, input.half_size, u.radius);
    float aaWidth = fwidth(distance);
    float aa = isnan(aaWidth) ? 1e-5 : max(aaWidth, 1e-5);
    float coverage = 1.0 - smoothstep(-aa, aa, distance);
    if (coverage == 0.0)
        discard;
    return float4(u.blend_color.rgb, coverage);
}
