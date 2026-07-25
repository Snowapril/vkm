// Copyright (c) 2025 Snowapril
//
// Cubemap skybox drawn as a single oversized clip-space triangle. Like triangle.hlsl and
// model_viewer.hlsl the pipeline declares no vertex input attributes -- but unlike them
// there is no geometry to pull either: SV_VertexID alone generates the three corners, so
// this shader touches neither the bindless vertex nor the index buffer array.
//
// The view ray is built in the vertex stage and interpolated. That is not just a
// convenience: the fragment stage cannot read the push constants (the range is vertex-only,
// see VkmPipelineStateVulkan::createInner), and reconstructing the ray from SV_Position in
// the pixel shader would be backend-dependent, because Vulkan's fragment SV_Position is in
// framebuffer coordinates *after* the -fvk-invert-y flip that vkm-compiler applies to the
// vertex stage. Deriving the ray here, from NDC coordinates this shader builds itself,
// keeps it identical on every backend -- the flip moves the position and its interpolants
// together.
//
// This sample never builds under WebGPU (it is excluded from that configuration in CMake)
// because WGSL has no unsized texture arrays for the bindless cubemap array to live in.

#include "vkm_bindless.hlsli"

struct PushConstants
{
    // Inverse of projection * mat4(mat3(view)): the view matrix with its translation
    // stripped, so unprojecting a clip-space corner yields a direction directly rather than
    // a world position that would still need the eye subtracted from it.
    float4x4 inverseViewRotationProjection;
    uint     cubemapSlot;
};

VKM_BINDLESS_TEXTURE_CUBE_ARRAY(g_BindlessTextureCubes);
VKM_BINDLESS_SAMPLER(g_DefaultSampler);

VKM_PUSH_CONSTANTS(PushConstants, g_PushConstants);

struct VSOutput
{
    float4 position : SV_POSITION;
    [[vk::location(0)]] float3 direction : TEXCOORD0;
    // Flat: an integer varying cannot be interpolated, and the slot is uniform per draw.
    [[vk::location(1)]] nointerpolation uint cubemapSlot : TEXCOORD1;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    // One triangle large enough to cover the viewport: (-1,-1), (3,-1), (-1,3) in NDC.
    // Cheaper than a quad and avoids the diagonal seam two triangles would introduce.
    const float2 ndc = float2((vertexId << 1) & 2, vertexId & 2) * 2.0 - 1.0;

    // z = 1 is the far plane under the engine's 0..1 depth convention, so this unprojects
    // the corner of the far plane -- the direction the pixel looks in.
    const float4 farPoint = mul(g_PushConstants.inverseViewRotationProjection, float4(ndc, 1.0, 1.0));

    VSOutput output;
    output.position = float4(ndc, 1.0, 1.0);
    output.direction = farPoint.xyz / farPoint.w;
    output.cubemapSlot = g_PushConstants.cubemapSlot;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    // Renormalized here rather than in the vertex stage: linear interpolation of three
    // normalized corner directions does not stay normalized across the triangle.
    const float3 direction = normalize(input.direction);
    return g_BindlessTextureCubes[input.cubemapSlot].Sample(g_DefaultSampler, direction);
}
