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

#endif // VKM_BACKEND_WEBGPU

#endif // VKM_BINDLESS_HLSLI
