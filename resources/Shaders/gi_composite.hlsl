// Copyright (c) 2025 Snowapril
//
// The shared composite: direct lighting plus whatever the active GI technique produced, and the
// engine's debug views over both.
//
// This is deliberately the *only* pass that knows how a technique's output is combined. A technique
// owns its passes and its resources and hands back one indirect-radiance texture; everything
// downstream of that is here, so adding a second technique later means adding passes, not touching
// the composite (see restir.md section 5).
//
// Indirect radiance arrives as incoming irradiance, not as reflected light -- the probe volume
// stores what arrived at a point, and knows nothing about the surface it will be asked about. The
// albedo it reflects off is applied here, which is also what keeps the probe atlas independent of
// the materials that sample it.

#include "vkm_fullscreen.hlsli"
#include "vkm_gbuffer.hlsli"

// Mirrors vkm::VkmGiCompositeConstants (renderer/gi_composite.h).
struct GiCompositeConstants
{
    // x = indirect intensity, y = debug view (VkmGiDebugView), zw unused
    float4 params;
};

[[vk::binding(0, 2)]] Texture2D    g_Direct   : register(t0, space2);
[[vk::binding(1, 2)]] Texture2D    g_Indirect : register(t1, space2);
// Albedo in rgb, roughness in a -- the same G-buffer target the deferred lighting pass reads.
[[vk::binding(2, 2)]] Texture2D    g_BaseColorRoughness : register(t2, space2);
[[vk::binding(3, 2)]] Texture2D    g_Normal   : register(t3, space2);
// Motion in xy, metallic in z, camera distance in w.
[[vk::binding(4, 2)]] Texture2D    g_Motion   : register(t4, space2);
[[vk::binding(5, 2)]] SamplerState g_Sampler  : register(s0, space2);
[[vk::binding(6, 2)]] ConstantBuffer<GiCompositeConstants> g_Composite : register(b0, space2);

// Must match vkm::VkmGiDebugView.
#define VKM_GI_DEBUG_COMPOSITE 0
#define VKM_GI_DEBUG_DIRECT    1
#define VKM_GI_DEBUG_INDIRECT  2
#define VKM_GI_DEBUG_ALBEDO    3
#define VKM_GI_DEBUG_NORMAL    4
#define VKM_GI_DEBUG_GEOMETRIC_NORMAL 5
#define VKM_GI_DEBUG_ROUGHNESS 6
#define VKM_GI_DEBUG_METALLIC  7
#define VKM_GI_DEBUG_MOTION    8
#define VKM_GI_DEBUG_DISTANCE  9

typedef VkmFullscreenVSOutput VSOutput;

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    return vkmFullscreenTriangle(vertexId);
}

float4 PSMain(VSOutput input) : SV_TARGET0
{
    // SampleLevel rather than Sample: the debug branches below are non-uniform control flow, where
    // WGSL forbids an implicit LOD. Every input has one mip, so the level is 0 regardless.
    const float4 baseColorRoughness = g_BaseColorRoughness.SampleLevel(g_Sampler, input.uv, 0);
    const float4 motionMetallic = g_Motion.SampleLevel(g_Sampler, input.uv, 0);
    const float3 direct = g_Direct.SampleLevel(g_Sampler, input.uv, 0).rgb;
    const float3 indirect = g_Indirect.SampleLevel(g_Sampler, input.uv, 0).rgb;

    const uint debugView = uint(g_Composite.params.y);
    if (debugView != VKM_GI_DEBUG_COMPOSITE)
    {
        const float4 packedNormals = g_Normal.SampleLevel(g_Sampler, input.uv, 0);
        switch (debugView)
        {
            case VKM_GI_DEBUG_DIRECT:    return float4(direct, 1.0);
            case VKM_GI_DEBUG_INDIRECT:  return float4(indirect * g_Composite.params.x, 1.0);
            case VKM_GI_DEBUG_ALBEDO:    return float4(baseColorRoughness.rgb, 1.0);
            // Normals are remapped into [0,1] so the negative half is visible rather than clipped
            // to black by the tone mapper downstream.
            case VKM_GI_DEBUG_NORMAL:
                return float4(vkmUnpackShadingNormal(packedNormals) * 0.5 + 0.5, 1.0);
            case VKM_GI_DEBUG_GEOMETRIC_NORMAL:
                return float4(vkmUnpackGeometricNormal(packedNormals) * 0.5 + 0.5, 1.0);
            case VKM_GI_DEBUG_ROUGHNESS: return float4(baseColorRoughness.aaa, 1.0);
            case VKM_GI_DEBUG_METALLIC:  return float4(motionMetallic.zzz, 1.0);
            // Motion is in UV units per frame and tiny, so it is scaled hard to be visible at all;
            // red is +x, green is +y, and a static camera reads as black.
            case VKM_GI_DEBUG_MOTION:
                return float4(abs(motionMetallic.xy) * 50.0, 0.0, 1.0);
            // Reciprocal so near geometry is bright: the far plane would otherwise dominate.
            case VKM_GI_DEBUG_DISTANCE:
                return float4((1.0 / max(motionMetallic.w, 1e-3)).xxx, 1.0);
            default: break;
        }
    }

    // Lambert: irradiance reflects off the surface as albedo/pi. Metals have no diffuse response,
    // so the indirect term fades out with metallic rather than tinting them.
    const float3 diffuseAlbedo = baseColorRoughness.rgb * (1.0 - motionMetallic.z);
    const float3 reflected = indirect * diffuseAlbedo * (1.0 / 3.14159265) * g_Composite.params.x;

    return float4(direct + reflected, 1.0);
}
