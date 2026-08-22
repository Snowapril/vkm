// Copyright (c) 2026 Snowapril
//
// Bindless vertex-pulling triangle shader: the pipeline declares no vertex input
// attributes at all (see renderpass.json) -- SV_VertexID is used to fetch the real index
// from a bindless index buffer, then that index fetches the vertex data from a bindless
// vertex buffer. Both buffers live in the engine-global bindless set 0, reached through
// vkm_bindless.hlsli; which buffer within each array to use for this draw is passed via
// push constants.

#include "vkm_bindless.hlsli"

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

VKM_PUSH_CONSTANTS(PushConstants, g_PushConstants);
VKM_BINDLESS_VERTEX_PULLING(VertexData);

struct VSOutput
{
    float4 position : SV_POSITION;
    [[vk::location(0)]] float4 color : COLOR0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    uint index = VKM_LOAD_INDEX(g_PushConstants.indexBufferIndex, vertexId);
    VertexData v = VKM_LOAD_VERTEX(g_PushConstants.vertexBufferIndex, index);

    VSOutput output;
    output.position = float4(v.position, 1.0);
    output.color = v.color;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    return input.color;
}
