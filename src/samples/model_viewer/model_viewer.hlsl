// Copyright (c) 2025 Snowapril
//
// Bindless vertex-pulling glTF mesh shader. Like triangle.hlsl, the pipeline declares no
// vertex input attributes: SV_VertexID indexes a bindless index buffer, whose value then
// indexes a bindless vertex buffer, both reached through vkm_bindless.hlsli.
//
// The camera comes from descriptor set 1, which the engine rewrites once per frame
// (vkm_frame_constants.hlsli), so the push constants only carry what is genuinely per-draw:
// the two bindless slot indices, this draw's model matrix, its base color, and the light
// direction already rotated into object space -- shading happens in object space so no
// per-draw normal matrix is needed, which keeps everything inside one push-constant range
// (see kVkmBindlessPushConstantSize).

#include "vkm_bindless.hlsli"
#include "vkm_frame_constants.hlsli"

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
    float4x4 model;
    uint     vertexBufferIndex;
    uint     indexBufferIndex;
    float2   _pad0;
    float4   baseColor;
    float4   lightDirectionObjectSpace; // xyz: normalized, points towards the light
};

VKM_PUSH_CONSTANTS(PushConstants, g_PushConstants);
VKM_BINDLESS_VERTEX_PULLING(VertexData);
VKM_FRAME_CONSTANTS(g_VkmFrame);

struct VSOutput
{
    float4 position : SV_POSITION;
    [[vk::location(0)]] float3 normal : NORMAL0;
    [[vk::location(1)]] float3 lightDirection : TEXCOORD0;
    [[vk::location(2)]] float4 baseColor : COLOR0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    uint index = VKM_LOAD_INDEX(g_PushConstants.indexBufferIndex, vertexId);
    VertexData v = VKM_LOAD_VERTEX(g_PushConstants.vertexBufferIndex, index);

    VSOutput output;
    // Two mat4 x vec4 products rather than folding VP*M per draw on the CPU: the camera is
    // shared by every draw, so only the model matrix has to travel in the push constants.
    output.position = mul(g_VkmFrame.viewProjection, mul(g_PushConstants.model, float4(v.position, 1.0)));
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
