// Copyright (c) 2025 Snowapril
//
// Shader-side mirror of the engine-global bindless set 0 (see
// renderer/backend/common/bindless_resource_manager.h, the single source of truth for the
// binding numbers and capacities used here).
//
// Backends disagree about how bindless resources and push constants are expressed, so this
// header is the one place that knows the difference. Shaders including it declare their
// resources and pull vertices through the macros below and never test VKM_BACKEND_*
// themselves.
//
//   Vulkan / Metal: real descriptor indexing -- unsized StructuredBuffer arrays at set 0
//     bindings 1 and 2 -- plus a real push-constant block. On Metal the arrays become a
//     Tier-2 argument buffer; the SPIR-V -> MSL binding layout is pinned by vkm-compiler.
//
//   WebGPU: WGSL has neither unsized descriptor arrays nor push constants, so the runtime
//     emulates the arrays with one "mega-buffer" per array plus a slot table of element
//     offsets, and push constants with a dynamic-offset uniform ring (see
//     VkmBindlessResourceManagerWebGPU). Slot table layout: [slot] = vertex-buffer element
//     offset, [VKM_BINDLESS_BUFFER_CAPACITY + slot] = index-buffer element offset.
//     VKM_BINDLESS_BUFFER_CAPACITY is defined by vkm-compiler from
//     kVkmBindlessBufferCapacity.
//
// Push constants are vertex-stage only (see VkmPipelineStateVulkan::createInner), so
// anything the fragment stage needs must travel as an interpolant.
//
// Usage:
//     #include "vkm_bindless.hlsli"
//
//     struct VertexData { ... };
//     struct PushConstants { uint vertexBufferIndex; uint indexBufferIndex; ... };
//
//     VKM_PUSH_CONSTANTS(PushConstants, g_PushConstants);
//     VKM_BINDLESS_VERTEX_PULLING(VertexData);
//
//     VSOutput VSMain(uint vertexId : SV_VertexID)
//     {
//         uint index = VKM_LOAD_INDEX(g_PushConstants.indexBufferIndex, vertexId);
//         VertexData v = VKM_LOAD_VERTEX(g_PushConstants.vertexBufferIndex, index);
//         ...
//     }
//
// VKM_BINDLESS_VERTEX_PULLING declares engine-owned globals prefixed with g_Vkm; a shader
// may use it at most once per translation unit.

#ifndef VKM_BINDLESS_HLSLI
#define VKM_BINDLESS_HLSLI

#if defined(VKM_BACKEND_WEBGPU)

#define VKM_PUSH_CONSTANTS(PushConstantType, name) \
    [[vk::binding(0, 0)]] ConstantBuffer<PushConstantType> name : register(b0, space0)

#define VKM_BINDLESS_VERTEX_PULLING(VertexType)                                                    \
    [[vk::binding(1, 0)]] StructuredBuffer<VertexType> g_VkmVertexMegaBuffer  : register(t0, space0); \
    [[vk::binding(2, 0)]] StructuredBuffer<uint>       g_VkmIndexMegaBuffer   : register(t1, space0); \
    [[vk::binding(3, 0)]] StructuredBuffer<uint>       g_VkmBindlessSlotTable : register(t2, space0)

#define VKM_LOAD_INDEX(indexBufferSlot, elementIndex) \
    (g_VkmIndexMegaBuffer[g_VkmBindlessSlotTable[VKM_BINDLESS_BUFFER_CAPACITY + (indexBufferSlot)] + (elementIndex)])

#define VKM_LOAD_VERTEX(vertexBufferSlot, elementIndex) \
    (g_VkmVertexMegaBuffer[g_VkmBindlessSlotTable[(vertexBufferSlot)] + (elementIndex)])

// Fixed singleton buffers (set 0, bindings kVkmBindlessFirstSingletonBinding..). These need no
// emulation -- WGSL has ordinary storage bindings -- so only the numbers differ from the native
// branch, shifted by one for the push-constant ring this backend keeps at b0.
#define VKM_BINDLESS_OBJECT_DATA(ObjectType, name) \
    [[vk::binding(5, 0)]] StructuredBuffer<ObjectType> name : register(t3, space0)

#define VKM_BINDLESS_FRAME_DATA(FrameType, name) \
    [[vk::binding(6, 0)]] StructuredBuffer<FrameType> name : register(t4, space0)

#define VKM_BINDLESS_INDIRECT_ARGUMENTS(name) \
    [[vk::binding(7, 0)]] RWStructuredBuffer<uint> name : register(u0, space0)

#define VKM_BINDLESS_VISIBLE_LIST(name) \
    [[vk::binding(8, 0)]] RWStructuredBuffer<uint> name : register(u1, space0)

#else

#define VKM_PUSH_CONSTANTS(PushConstantType, name) \
    [[vk::push_constant]] PushConstantType name

#define VKM_BINDLESS_VERTEX_PULLING(VertexType)                                                        \
    [[vk::binding(1, 0)]] StructuredBuffer<VertexType> g_VkmBindlessVertexBuffers[] : register(t0, space0); \
    [[vk::binding(2, 0)]] StructuredBuffer<uint>       g_VkmBindlessIndexBuffers[]  : register(t1, space0)

#define VKM_LOAD_INDEX(indexBufferSlot, elementIndex) \
    (g_VkmBindlessIndexBuffers[indexBufferSlot][elementIndex])

#define VKM_LOAD_VERTEX(vertexBufferSlot, elementIndex) \
    (g_VkmBindlessVertexBuffers[vertexBufferSlot][elementIndex])

// Sampled textures (set 0 binding 0) and the one engine sampler (binding 3). Deliberately
// absent from the WebGPU branch above: WGSL has no runtime-sized texture arrays, so that
// backend's bindless layer models no texture array at all (see
// VkmDriverCapabilityFlags::TextureUpload) and a shader needing these cannot build there --
// such samples are excluded from WebGPU configurations in CMake rather than failing here.
//
// Binding 0 carries no dimensionality of its own -- the shader's declaration supplies it --
// so a shader picks exactly one of these per translation unit and only indexes slots holding
// views of that type. Declaring two would alias them onto one another in the generated MSL.
#define VKM_BINDLESS_TEXTURE_CUBE_ARRAY(name) \
    [[vk::binding(0, 0)]] TextureCube name[] : register(t0, space0)

#define VKM_BINDLESS_TEXTURE_2D_ARRAY(name) \
    [[vk::binding(0, 0)]] Texture2D name[] : register(t0, space0)

#define VKM_BINDLESS_SAMPLER(name) \
    [[vk::binding(3, 0)]] SamplerState name : register(s0, space0)

/*
* Fixed singleton buffers, set 0 bindings kVkmBindlessFirstSingletonBinding.. -- one per
* VkmBindlessSingletonBuffer, in that enum's order. Unlike the arrays above they carry no slot
* index: the GPU-driven scene draw path pushes no constants at all, so a shader has to reach them
* without one.
*
* The two read-write ones are COMPUTE-ONLY on purpose. WebGPU forbids writable storage in the
* vertex stage, and a buffer used as writable storage inside a render pass's usage scope may not
* also be fetched as an indirect buffer in that scope -- which is exactly what the draw does. A
* render pipeline's shader must therefore never declare them.
*/
#define VKM_BINDLESS_OBJECT_DATA(ObjectType, name) \
    [[vk::binding(4, 0)]] StructuredBuffer<ObjectType> name : register(t3, space0)

#define VKM_BINDLESS_FRAME_DATA(FrameType, name) \
    [[vk::binding(5, 0)]] StructuredBuffer<FrameType> name : register(t4, space0)

#define VKM_BINDLESS_INDIRECT_ARGUMENTS(name) \
    [[vk::binding(6, 0)]] RWStructuredBuffer<uint> name : register(u0, space0)

#define VKM_BINDLESS_VISIBLE_LIST(name) \
    [[vk::binding(7, 0)]] RWStructuredBuffer<uint> name : register(u1, space0)

#endif // VKM_BACKEND_WEBGPU

#endif // VKM_BINDLESS_HLSLI
