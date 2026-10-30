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
    float2 pLocal = (input.uv * 2.0 - 1.0) * input.half_size;
    float smaller = min(input.half_size.x, input.half_size.y);
    float nearest = sd_round_box(
        pLocal, input.half_size, smaller * u.radius);
    float aaFactor = length(float2(ddx(nearest), ddy(nearest))) * 0.75;
    float coverage = 1.0 - smoothstep(-aaFactor, aaFactor, nearest);
    if (coverage == 0.0)
        discard;
    return float4(u.blend_color.rgb, coverage);
}
