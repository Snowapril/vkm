// Copyright (c) 2025 Snowapril
//
// Frustum-culls one draw batch and writes the survivors as a compacted list of ObjectData indices
// plus their count.
//
// This is the shared half of the GPU-driven path: every backend runs this exact shader, and only
// the emit pass that turns this list into something drawable differs (scene_emit_draws.hlsl fills
// indirect arguments on Vulkan/WebGPU; Metal fills an MTLIndirectCommandBuffer from MSL). Both
// dispatches are recorded into the same compute pass, so the compaction below is visible to the
// emit pass without an intervening barrier beyond the pass's own bracketing.
//
// The caller must have zeroed this batch's count word before dispatching.

#include "scene_common.hlsli"

// Largest column length of the upper 3x3, i.e. the most any axis is scaled. Using the maximum makes
// the transformed bounding-sphere radius conservative under non-uniform scale, so culling never
// removes geometry that is actually on screen.
float maxAxisScale(float4x4 transform)
{
    const float3 x = float3(transform[0][0], transform[1][0], transform[2][0]);
    const float3 y = float3(transform[0][1], transform[1][1], transform[2][1]);
    const float3 z = float3(transform[0][2], transform[1][2], transform[2][2]);
    return sqrt(max(dot(x, x), max(dot(y, y), dot(z, z))));
}

[numthreads(64, 1, 1)] // must equal kVkmComputeThreadGroupSizeX
void CSMain(uint3 threadId : SV_DispatchThreadID)
{
    if (threadId.x >= g_Batch.objectCount)
    {
        return;
    }

    const uint objectIndex = g_Batch.firstObject + threadId.x;
    const ObjectData obj = g_ObjectData[objectIndex];

    // An object with no geometry would emit an empty draw; drop it here rather than downstream.
    if (obj.indexCount == 0)
    {
        return;
    }

    const float3 centre = mul(obj.worldTransform, float4(obj.boundsCenterRadius.xyz, 1.0)).xyz;
    const float radius = obj.boundsCenterRadius.w * maxAxisScale(obj.worldTransform);

    for (uint plane = 0; plane < 6; ++plane)
    {
        const float4 frustumPlane = g_FrameData[0].frustumPlanes[plane];
        if (dot(frustumPlane.xyz, centre) + frustumPlane.w < -radius)
        {
            return; // Entirely outside this half-space.
        }
    }

    uint slot;
    InterlockedAdd(g_Visible[g_Batch.countWordOffset], 1, slot);
    g_Visible[g_Batch.visibleWordOffset + slot] = objectIndex;
}
