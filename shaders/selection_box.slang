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

float sd_round_box(float2 point, float2 box, float r) {
    float2 q = abs(point) - box + r;
    return min(max(q.x, q.y), 0.0) + length(max(q, 0.0)) - r;
}

[shader("fragment")]
float4 main(VSOutput input) : SV_Target {
    float2 point = (input.uv * 2.0 - 1.0) * input.half_size;
    float smallerSide = min(input.half_size.x, input.half_size.y);
    float distance = sd_round_box(
        point, input.half_size, smallerSide * u.radius);
    float aaWidth = fwidth(distance) * 0.5;
    aaWidth = isnan(aaWidth) ? 1e-5 : max(aaWidth, 1e-5);
    float coverage = 1.0 - smoothstep(-aaWidth, aaWidth, distance);
    if (coverage == 0.0)
        discard;
    return float4(u.blend_color.rgb, coverage);
}
