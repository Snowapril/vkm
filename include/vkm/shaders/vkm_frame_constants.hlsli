// Copyright (c) 2025 Snowapril
//
// Shader-side mirror of the engine-global per-frame constant set (set 1). See
// renderer/backend/common/frame_constants.h -- the single source of truth for the struct
// layout, the set/binding numbers and the per-frame-slot stride used here.
//
// Descriptor-set convention, ordered by update frequency:
//   set 0  bindless resources (vkm_bindless.hlsli)
//   set 1  per-frame camera constants (this header)
//   set 2  per-pass  -- reserved
//   set 3  per-draw  -- reserved
// Per-draw data that fits travels as push constants instead of through set 3.
//
// Unlike set 0 this needs no per-backend spelling: it is one ordinary constant buffer, which
// dxc (Vulkan SPIR-V), spirv-cross (Metal) and tint (WGSL) all carry through from SPIR-V
// set 1 / binding 0 unchanged. On Metal, vkm-compiler declares set 1 discrete so it lands as
// a plain buffer binding rather than a second argument buffer.
//
// Unlike push constants -- which are vertex-stage only, see vkm_bindless.hlsli -- these
// constants are readable from every stage.
//
// Usage:
//     #include "vkm_frame_constants.hlsli"
//
//     VKM_FRAME_CONSTANTS(g_VkmFrame);
//
//     ... mul(g_VkmFrame.viewProjection, worldPosition) ...

#ifndef VKM_FRAME_CONSTANTS_HLSLI
#define VKM_FRAME_CONSTANTS_HLSLI

// Mirrors vkm::VkmFrameConstants. Every member is 16-byte aligned, so this matches the C++
// struct byte for byte (pinned by a static_assert on that side).
struct VkmFrameConstants
{
    float4x4 view;
    float4x4 projection;
    float4x4 viewProjection;
    float4x4 inverseViewProjection;
    float4   cameraPositionWorld; // xyz = world-space eye, w = 1
};

#define VKM_FRAME_CONSTANTS(name) \
    [[vk::binding(0, 1)]] ConstantBuffer<VkmFrameConstants> name : register(b0, space1)

#endif // VKM_FRAME_CONSTANTS_HLSLI
