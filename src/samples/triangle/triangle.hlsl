// Copyright (c) 2025 Snowapril
//
// Minimal triangle shader matching renderpass.json's PSO:
//   input_layout: "float3float4"  -> location 0 = position, location 1 = color
//   color_attachments: single bgra8_unorm, no depth
// The vertex stage passes position straight through as clip space and forwards
// the per-vertex color to the fragment stage, which writes it unchanged.

struct VSInput
{
    [[vk::location(0)]] float3 position : POSITION;
    [[vk::location(1)]] float4 color : COLOR0;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    [[vk::location(0)]] float4 color : COLOR0;
};

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    output.position = float4(input.position, 1.0);
    output.color = input.color;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    return input.color;
}
