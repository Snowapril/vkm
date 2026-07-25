// Copyright (c) 2025 Snowapril
//
// Bindless vertex-pulling glTF mesh shader. Like triangle.hlsl, the pipeline declares no
// vertex input attributes: SV_VertexID indexes a bindless index buffer, whose value then
// indexes a bindless vertex buffer. On top of the two slot indices, the push constants
// carry this draw's model-view-projection matrix, its base color, and the light direction
// already rotated into object space -- shading happens in object space so no per-draw
// normal matrix is needed, which keeps everything inside one push-constant range (see
// kVkmBindlessPushConstantSize).
//
// The push-constant range is vertex-stage only (VkmPipelineStateVulkan::createInner), so
// everything the fragment stage needs travels as an interpolant.

// Mirrors vkm::VkmSceneVertex (include/vkm/renderer/scene/scene_model.h): the padding is
// part of the layout, not decoration.
struct VertexData
{
    float3 position;
    float  _pad0;
    float3 normal;
    float  _pad1;
    float2 uv0;
    float2 _pad2;
    float4 tangent;
};

struct PushConstants
{
    float4x4 modelViewProjection;
    uint     vertexBufferIndex;
    uint     indexBufferIndex;
    float2   _pad0;
    float4   baseColor;
    float4   lightDirectionObjectSpace; // xyz: normalized, points towards the light
};

#if defined(VKM_BACKEND_WEBGPU)
// See triangle.hlsl for why the WebGPU backend needs this separate binding shape.
[[vk::binding(0, 0)]] ConstantBuffer<PushConstants>  g_PushConstants     : register(b0, space0);
[[vk::binding(1, 0)]] StructuredBuffer<VertexData>   g_MegaVertexBuffer  : register(t0, space0);
[[vk::binding(2, 0)]] StructuredBuffer<uint>         g_MegaIndexBuffer   : register(t1, space0);
[[vk::binding(3, 0)]] StructuredBuffer<uint>         g_BindlessSlotTable : register(t2, space0);
#else
[[vk::binding(1, 0)]] StructuredBuffer<VertexData> g_BindlessVertexBuffers[] : register(t0, space0);
[[vk::binding(2, 0)]] StructuredBuffer<uint>       g_BindlessIndexBuffers[]  : register(t1, space0);

[[vk::push_constant]] PushConstants g_PushConstants;
#endif

struct VSOutput
{
    float4 position : SV_POSITION;
    [[vk::location(0)]] float3 normal : NORMAL0;
    [[vk::location(1)]] float3 lightDirection : TEXCOORD0;
    [[vk::location(2)]] float4 baseColor : COLOR0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
#if defined(VKM_BACKEND_WEBGPU)
    uint indexBase = g_BindlessSlotTable[VKM_BINDLESS_BUFFER_CAPACITY + g_PushConstants.indexBufferIndex];
    uint index = g_MegaIndexBuffer[indexBase + vertexId];
    uint vertexBase = g_BindlessSlotTable[g_PushConstants.vertexBufferIndex];
    VertexData v = g_MegaVertexBuffer[vertexBase + index];
#else
    uint index = g_BindlessIndexBuffers[g_PushConstants.indexBufferIndex][vertexId];
    VertexData v = g_BindlessVertexBuffers[g_PushConstants.vertexBufferIndex][index];
#endif

    VSOutput output;
    output.position = mul(g_PushConstants.modelViewProjection, float4(v.position, 1.0));
    output.normal = v.normal;
    output.lightDirection = g_PushConstants.lightDirectionObjectSpace.xyz;
    output.baseColor = g_PushConstants.baseColor;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    float3 normal = normalize(input.normal);
    float3 lightDirection = normalize(input.lightDirection);

    // Half-Lambert wrap so backfacing-but-visible geometry stays readable instead of black.
    float diffuse = saturate(dot(normal, lightDirection)) * 0.8 + 0.2;
    return float4(input.baseColor.rgb * diffuse, input.baseColor.a);
}
