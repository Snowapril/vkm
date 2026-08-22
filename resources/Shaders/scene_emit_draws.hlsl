// Copyright (c) 2026 Snowapril
//
// Turns scene_cull.hlsl's compacted visible-object list into native indirect draw arguments.
//
// This is the Vulkan/WebGPU half of the emit step; Metal replaces this shader with an MSL kernel
// that fills an MTLIndirectCommandBuffer instead, because an ICB command bakes its arguments at
// encode time and cannot read them from a buffer. Both run as the second dispatch of the same
// compute pass, over the same shared cull output.
//
// One thread per candidate slot, not per survivor: the slots past the visible count have to be
// zeroed, because an all-zero VkmDrawIndirectArguments record is what makes a culled slot a no-op
// draw on the backends with no GPU-side draw count (see VkmCommandBufferBase::drawIndirectCount).

#include "scene_common.hlsli"

[numthreads(64, 1, 1)] // must equal kVkmComputeThreadGroupSizeX: the dispatch site derives its group count from it
void CSMain(uint3 threadId : SV_DispatchThreadID)
{
    if (threadId.x >= g_Batch.objectCount)
    {
        return;
    }

    // VkmDrawIndirectArguments is 4 u32s: vertexCount, instanceCount, firstVertex, firstInstance.
    // The 4-word step is the NonIndexed stride (VkmIndirectArgumentLayout); an indexed emit shader
    // would step 5 words and write VkmDrawIndexedIndirectArguments instead.
    const uint base = g_Batch.argumentWordOffset + threadId.x * 4;
    const uint visibleCount = g_Visible[g_Batch.countWordOffset];

    // Publish the count where the draw side reads it, before any early return below: nothing clears
    // the argument buffer's count word, so a frame that culls everything must still overwrite the
    // previous frame's value rather than leaving it stale.
    if (threadId.x == 0)
    {
        g_Arguments[g_Batch.countWordOffset] = visibleCount;
    }

    if (threadId.x >= visibleCount)
    {
        g_Arguments[base + 0] = 0;
        g_Arguments[base + 1] = 0;
        g_Arguments[base + 2] = 0;
        g_Arguments[base + 3] = 0;
        return;
    }

    const uint objectIndex = g_Visible[g_Batch.visibleWordOffset + threadId.x];
    const ObjectData obj = g_ObjectData[objectIndex];

    g_Arguments[base + 0] = obj.indexCount; // vertexCount: indices are pulled in the vertex shader
    g_Arguments[base + 1] = 1;              // instanceCount
    g_Arguments[base + 2] = 0;              // firstVertex
    g_Arguments[base + 3] = objectIndex;    // firstInstance -> SV_InstanceID
}
