// Copyright (c) 2025 Snowapril
//
// First pass of the memory_aliasing sample's chain: writes a moving pattern from constants
// alone, with no texture input, so the chain has a source.
//
// Deliberately a separate file from mem_process.hlsl even though the vertex half is identical:
// a shader cache file is named `<shader>[<option>].<stage>.<backend>` and carries no entry
// point, so two PSOs sharing one HLSL file and option name would silently overwrite each
// other's cache (see TODO.md).

#include "vkm_fullscreen.hlsli"

struct MemStageConstants
{
    // x = animation phase in radians, y = stage index, z = stage count, w unused.
    float4 params;
};

[[vk::binding(0, 2)]] ConstantBuffer<MemStageConstants> g_Stage : register(b0, space2);

typedef VkmFullscreenVSOutput VSOutput;

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    return vkmFullscreenTriangle(vertexId);
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    // Concentric rings that move with the phase. Any pattern would do; a moving one makes a
    // dropped or stale pass obvious by eye, which is the point of a sample used to eyeball
    // whether aliasing broke anything.
    const float2 centered = input.uv * 2.0 - 1.0;
    const float radius = length(centered);
    const float rings = 0.5 + 0.5 * sin(radius * 24.0 - g_Stage.params.x * 3.0);
    return float4(rings, rings * 0.35, 1.0 - rings, 1.0);
}
