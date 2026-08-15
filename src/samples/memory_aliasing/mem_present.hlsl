// Copyright (c) 2025 Snowapril
//
// Final pass of the memory_aliasing sample's chain: copies the last stage's target to the back
// buffer. Identical in body to mem_process.hlsl, but its PSO declares no depth attachment (the
// back buffer has none), and a shader cache file is keyed by file name rather than entry point,
// so the two cannot share one.

#include "vkm_fullscreen.hlsli"

[[vk::binding(0, 2)]] Texture2D    g_Input   : register(t0, space2);
[[vk::binding(1, 2)]] SamplerState g_Sampler : register(s0, space2);

typedef VkmFullscreenVSOutput VSOutput;

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    return vkmFullscreenTriangle(vertexId);
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    return float4(g_Input.SampleLevel(g_Sampler, input.uv, 0).rgb, 1.0);
}
