// Copyright (c) 2025 Snowapril
//
// Draws one unit sphere per draw call, positioned and scaled entirely by push constants, so the
// ball and the six hand proxies all come from the same mesh and the same pipeline. Vertex data
// is pulled from the bindless set the way triangle.hlsl does it; the pipeline declares no vertex
// input attributes.
//
// Colour travels to the fragment stage as a varying because the push-constant range is
// vertex-only (see VkmPipelineStateVulkan::createInner).

#include "vkm_bindless.hlsli"
#include "vkm_frame_constants.hlsli"

// Mirrors vkm::SphereVertex (sphere_mesh.h): float3 at offset 0, float3 at offset 16, stride 32.
struct SphereVertex
{
    float3 position;
    float3 normal;
};

struct PushConstants
{
    float4 centerAndRadius; // xyz = world-space centre, w = world-space radius
    float4 color;           // rgb = base colour, a unused
    uint vertexBufferIndex;
    uint indexBufferIndex;
};

VKM_PUSH_CONSTANTS(PushConstants, g_PushConstants);
VKM_BINDLESS_VERTEX_PULLING(SphereVertex);
VKM_FRAME_CONSTANTS(g_VkmFrame);

struct VSOutput
{
    float4 position : SV_POSITION;
    [[vk::location(0)]] float3 normal : NORMAL0;
    [[vk::location(1)]] nointerpolation float4 color : COLOR0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    const uint index = VKM_LOAD_INDEX(g_PushConstants.indexBufferIndex, vertexId);
    const SphereVertex vertex = VKM_LOAD_VERTEX(g_PushConstants.vertexBufferIndex, index);

    const float3 worldPosition =
        g_PushConstants.centerAndRadius.xyz + vertex.position * g_PushConstants.centerAndRadius.w;

    VSOutput output;
    output.position = mul(g_VkmFrame.viewProjection, float4(worldPosition, 1.0));
    // The transform is a uniform scale plus a translation, so the normal survives it unchanged.
    output.normal = vertex.normal;
    output.color = g_PushConstants.color;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    const float3 normal = normalize(input.normal);
    const float3 lightDirection = normalize(float3(-0.35, 0.65, 0.55));
    const float diffuse = saturate(dot(normal, lightDirection));

    // The camera looks down -Z, so a surface facing it has a normal near +Z and this falls off
    // towards the silhouette. The rim keeps the ball readable against a busy camera image.
    const float rim = pow(1.0 - saturate(normal.z), 3.0);

    const float3 lit = input.color.rgb * (0.25 + 0.75 * diffuse) + rim * 0.22;
    return float4(lit, 1.0);
}
