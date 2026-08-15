// Copyright (c) 2025 Snowapril
//
// Middle passes of the memory_aliasing sample's chain: reads the previous stage's target and
// writes a slightly transformed copy into the next one. Each stage's output is consumed only by
// the following stage, which is what makes alternating stages' lifetimes disjoint and therefore
// aliasable.
//
// See mem_generate.hlsl for why this is a separate file rather than another entry point.

#include "vkm_fullscreen.hlsli"

struct MemStageConstants
{
    // x = animation phase in radians, y = stage index, z = stage count, w unused.
    float4 params;
};

[[vk::binding(0, 2)]] Texture2D                         g_Input   : register(t0, space2);
[[vk::binding(1, 2)]] SamplerState                      g_Sampler : register(s0, space2);
[[vk::binding(2, 2)]] ConstantBuffer<MemStageConstants> g_Stage   : register(b0, space2);

typedef VkmFullscreenVSOutput VSOutput;

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    return vkmFullscreenTriangle(vertexId);
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    // A per-stage hue rotation, so the final image encodes how many stages actually ran: if an
    // aliased target were read after its bytes had been handed to another stage, the result
    // would be visibly wrong rather than subtly so.
    const float3 source = g_Input.SampleLevel(g_Sampler, input.uv, 0).rgb;
    const float stageIndex = g_Stage.params.y;
    const float weight = 0.15 + 0.1 * stageIndex;
    const float3 rotated = float3(source.g, source.b, source.r);
    return float4(lerp(source, rotated, weight), 1.0);
}
