// Copyright (c) 2026 Snowapril
//
// Maps an HDR scene colour to a displayable range and encodes it for the swapchain.
//
// Replaces the loose GLSL tonemap.frag + quad_screen.vert this migration deletes. Those were dead
// -- no PSO JSON referenced them, so nothing compiled them -- and also wrong in two ways worth
// recording, since the intent has to be reconstructed from them:
//
//   - tonemap.frag defined a tonemap() helper doing exposure, white-point normalization and gamma,
//     but main() called Uncharted2Tonemap() directly instead. The helper was unreachable, so the
//     output was un-normalized (the curve never reaches 1.0, so whites came out grey) and never
//     gamma-encoded.
//   - quad_screen.vert scaled its positions by 0.8, so it did not actually cover the screen.
//
// Both are fixed here rather than faithfully reproduced.

#include "vkm_fullscreen.hlsli"

struct TonemapConstants
{
    // x = exposure multiplier applied before the curve, y = display gamma, zw unused.
    float4 exposureGamma;
};

[[vk::binding(0, 2)]] Texture2D                        g_SceneColor : register(t0, space2);
[[vk::binding(1, 2)]] SamplerState                     g_Sampler    : register(s0, space2);
[[vk::binding(2, 2)]] ConstantBuffer<TonemapConstants> g_Tonemap    : register(b0, space2);

typedef VkmFullscreenVSOutput VSOutput;

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    return vkmFullscreenTriangle(vertexId);
}

// Uncharted 2 filmic curve (Hable). Kept as the engine's operator because the original shader
// chose it; the constants are his.
float3 uncharted2Curve(float3 color)
{
    const float A = 0.15;
    const float B = 0.50;
    const float C = 0.10;
    const float D = 0.20;
    const float E = 0.02;
    const float F = 0.30;
    return ((color * (A * color + C * B) + D * E) / (color * (A * color + B) + D * F)) - E / F;
}

float4 PSMain(VSOutput input) : SV_TARGET0
{
    const float exposure = g_Tonemap.exposureGamma.x;
    const float gamma = g_Tonemap.exposureGamma.y;

    const float3 sceneColor = g_SceneColor.Sample(g_Sampler, input.uv).rgb;

    float3 mapped = uncharted2Curve(sceneColor * exposure);
    // Normalize by the curve's value at the white point, so an input of W maps to exactly 1.0.
    // Without this the curve never reaches 1 and the image reads washed out -- the bug in the
    // shader this replaces.
    const float kWhitePoint = 11.2;
    mapped = mapped / uncharted2Curve(float3(kWhitePoint, kWhitePoint, kWhitePoint));

    // Encode for a non-linear display. The swapchain format is UNORM rather than SRGB, so this
    // shader owns the transfer function; an _SRGB target would apply it in hardware and this would
    // double-correct.
    return float4(pow(saturate(mapped), 1.0 / gamma), 1.0);
}
