// Copyright (c) 2025 Snowapril
//
// Bindless vertex-pulling triangle shader: the pipeline declares no vertex input
// attributes at all (see renderpass.json) -- SV_VertexID is used to fetch the real index
// from a bindless index buffer, then that index fetches the vertex data from a bindless
// vertex buffer. Both buffers live in the engine-global bindless set 0 (see
// VkmBindlessResourceManagerVulkan); which buffer within each array to use for this draw
// is passed via push constants.

struct VertexData
{
    float3 position;
    float4 color;
};

struct PushConstants
{
    uint vertexBufferIndex;
    uint indexBufferIndex;
};

#if defined(VKM_BACKEND_WEBGPU)
// WGSL has no unsized descriptor arrays and no push constants, so the WebGPU backend
// emulates the bindless set with per-array "mega-buffers" plus a slot table of element
// offsets, and push constants with a dynamic-offset uniform ring (see
// VkmBindlessResourceManagerWebGPU). Slot table layout: [slot] = vertex-buffer element
// offset, [4096 + slot] = index-buffer element offset (capacities in
// common/bindless_resource_manager.h).
[[vk::binding(0, 0)]] ConstantBuffer<PushConstants>  g_PushConstants     : register(b0, space0);
[[vk::binding(1, 0)]] StructuredBuffer<VertexData>   g_MegaVertexBuffer  : register(t0, space0);
[[vk::binding(2, 0)]] StructuredBuffer<uint>         g_MegaIndexBuffer   : register(t1, space0);
[[vk::binding(3, 0)]] StructuredBuffer<uint>         g_BindlessSlotTable : register(t2, space0);
#else
[[vk::binding(1, 0)]] StructuredBuffer<VertexData> g_BindlessVertexBuffers[] : register(t0, space0);
[[vk::binding(2, 0)]] StructuredBuffer<uint>        g_BindlessIndexBuffers[]  : register(t1, space0);

[[vk::push_constant]] PushConstants g_PushConstants;
#endif

struct VSOutput
{
    float4 position : SV_POSITION;
    [[vk::location(0)]] float4 color : COLOR0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
#if defined(VKM_BACKEND_WEBGPU)
    // VKM_BINDLESS_BUFFER_CAPACITY is passed by vkm-compiler from
    // common/bindless_resource_manager.h's kVkmBindlessBufferCapacity.
    uint indexBase = g_BindlessSlotTable[VKM_BINDLESS_BUFFER_CAPACITY + g_PushConstants.indexBufferIndex];
    uint index = g_MegaIndexBuffer[indexBase + vertexId];
    uint vertexBase = g_BindlessSlotTable[g_PushConstants.vertexBufferIndex];
    VertexData v = g_MegaVertexBuffer[vertexBase + index];
#else
    uint index = g_BindlessIndexBuffers[g_PushConstants.indexBufferIndex][vertexId];
    VertexData v = g_BindlessVertexBuffers[g_PushConstants.vertexBufferIndex][index];
#endif

    VSOutput output;
    output.position = float4(v.position, 1.0);
    output.color = v.color;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    return input.color;
}
