struct Transform2D {
    float2 scale;    // (2/W, 2/H)
    float2 offset;   // (-1, -1) plus centering term
};

[[vk::binding(0, 1)]]
ConstantBuffer<Transform2D> transform : register(b0, space1);

struct VSInput {
    float2 position : TEXCOORD0;
    float2 uv       : TEXCOORD1;
};

struct VSOutput {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

[shader("vertex")]
VSOutput vs_main(VSInput input)
{
    VSOutput o;
    o.position = float4(input.position * transform.scale + transform.offset,
                        0.0, 1.0);
    o.uv       = input.uv;
    return o;
}
