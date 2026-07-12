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

[[vk::binding(1, 0)]] StructuredBuffer<VertexData> g_BindlessVertexBuffers[] : register(t0, space0);
[[vk::binding(2, 0)]] StructuredBuffer<uint>        g_BindlessIndexBuffers[]  : register(t1, space0);

struct PushConstants
{
    uint vertexBufferIndex;
    uint indexBufferIndex;
};
[[vk::push_constant]] PushConstants g_PushConstants;

struct VSOutput
{
    float4 position : SV_POSITION;
    [[vk::location(0)]] float4 color : COLOR0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    uint index = g_BindlessIndexBuffers[g_PushConstants.indexBufferIndex][vertexId];
    VertexData v = g_BindlessVertexBuffers[g_PushConstants.vertexBufferIndex][index];

    VSOutput output;
    output.position = float4(v.position, 1.0);
    output.color = v.color;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    return input.color;
}
