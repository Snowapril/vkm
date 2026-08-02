// Copyright (c) 2025 Snowapril
//
// Shared declarations for the engine's scene compute passes. The records here mirror their C++
// counterparts in include/vkm/renderer/scene/scene.h byte for byte; TestObjectDataLayout asserts
// the offsets, since nothing in the build fails if the two sides drift.

#ifndef VKM_SCENE_COMMON_HLSLI
#define VKM_SCENE_COMMON_HLSLI

// The set-0 binding numbers and the per-backend spelling of push constants live in one place; this
// header must never restate them (they moved once already, when the engine sampler took binding 3).
#include "vkm_bindless.hlsli"

// Mirrors vkm::VkmObjectData (176 bytes).
struct ObjectData
{
    float4x4 worldTransform;
    float4x4 normalTransform;
    uint     vertexPoolSlot;
    uint     indexPoolSlot;
    uint     vertexWordOffset;
    uint     materialIndex;
    uint     indexOffset;
    uint     indexCount;
    uint2    _pad0;
    float4   boundsCenterRadius; // object space: xyz = centre, w = radius
};

// Mirrors vkm::VkmFrameData (128 bytes). No camera: that is descriptor set 1's job.
struct FrameData
{
    float4 frustumPlanes[6]; // world space, normalized, xyz = normal, w = distance
    float4 lightDirection;
    uint   materialPoolSlot;
    uint   debugMode; // drawing-shader defined; the culling passes ignore it
    uint2  _pad0;
};

/*
* @brief Per-batch bounds, pushed once per dispatch.
*
* Both scene compute passes take the same constants: a batch is a contiguous run of ObjectData
* records, and its visible-list and argument-buffer regions are addressed in u32 words so one
* buffer can hold every batch's region back to back.
*/
struct SceneBatchConstants
{
    uint firstObject;        // first ObjectData index in this batch
    uint objectCount;        // candidates in this batch == the batch's maxDrawCount
    uint visibleWordOffset;  // first word of this batch's compacted object-index list
    uint countWordOffset;    // word holding this batch's visible count
    uint argumentWordOffset; // first word of this batch's VkmDrawIndirectArguments records
    uint frameDataIndex;     // which FrameData the cull tests against (one per cull view)
    // Scalars rather than a uint2: WebGPU lowers push constants to a uniform buffer, where a
    // vector aligns to its own size and would land at a different offset than it does here.
    uint _pad0;
    uint _pad1;
};

VKM_PUSH_CONSTANTS(SceneBatchConstants, g_Batch);
VKM_BINDLESS_OBJECT_DATA(ObjectData, g_ObjectData);
VKM_BINDLESS_FRAME_DATA(FrameData, g_FrameData);
VKM_BINDLESS_INDIRECT_ARGUMENTS(g_Arguments);
VKM_BINDLESS_VISIBLE_LIST(g_Visible);

#endif // VKM_SCENE_COMMON_HLSLI
