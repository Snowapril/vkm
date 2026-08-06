// Copyright (c) 2025 Snowapril
//
// Phase 7: one indirect ray per pixel, no reservoirs. Deliberately noisy -- this is the baseline
// ReSTIR has to beat, and the thing that proves the sampling and the BRDF are right *before*
// reservoirs make a bias far harder to see.
//
// The difference from the reference tracer is the primary hit and nothing else: this one comes
// from the rasterized G-buffer rather than from a traced ray, and the path continues from there
// through the same vkmTracePath(). Making a rasterizer and a ray query agree on where a surface is
// -- clip-space convention, distance reconstruction, normal packing, the offset a secondary ray
// leaves along -- is exactly the invariant this pass exists to pin, and accumulating it until it
// matches the reference is how that is checked.
//
// **What it computes is radiance leaving the G-buffer surface, EXCLUDING that surface's own
// emission.** The G-buffer carries no emissive channel (TODO.md), so the primary surface's
// emission is not recoverable here. That is not a fudge in the comparison: it is the quantity a
// deferred GI pass can compute, and the test scene is lit so that no camera-visible surface emits.
//
// Base colour comes from the G-buffer rather than from the material pool, which is what makes this
// a deferred pass at all -- the first bounce's albedo is already rasterized, textures included,
// while the tracer's own bounces are factor-only.

#include "vkm_bindless.hlsli"
#include "vkm_frame_constants.hlsli"
#include "vkm_gbuffer.hlsli"
#include "vkm_material.hlsli"
#include "vkm_path_tracing.hlsli"
#include "vkm_random.hlsli"

// Mirrors vkm::VkmObjectData. Restated for the same reason path_trace.hlsl restates it.
struct ObjectData
{
    float4x4 worldTransform;
    float4x4 normalTransform;
    uint     vertexPoolSlot;
    uint     indexPoolSlot;
    uint     vertexWordOffset;
    uint     materialIndex;
    uint     indexOffset;
    uint     indexCount;
    uint     vertexStrideWords;
    uint     _pad0;
    float4   boundsCenterRadius;
};

struct FrameData
{
    float4 frustumPlanes[6];
    float4 lightDirection;
    uint   materialPoolSlot;
    uint   debugMode;
    uint2  _pad0;
};

// Mirrors vkm::VkmIndirectConstants (include/vkm/renderer/indirect_pass.h).
struct IndirectConstants
{
    uint  width;
    uint  height;
    uint  sampleIndex;
    uint  maxBounces;
    float environmentR;
    float environmentG;
    float environmentB;
    uint  _pad0;
};

VKM_PUSH_CONSTANTS(IndirectConstants, g_Indirect);
VKM_FRAME_CONSTANTS(g_VkmFrame);

VKM_BINDLESS_VERTEX_PULLING(uint);
VKM_BINDLESS_OBJECT_DATA(ObjectData, g_ObjectData);
VKM_BINDLESS_FRAME_DATA(FrameData, g_FrameData);
VKM_BINDLESS_ACCELERATION_STRUCTURE(g_Scene);
VKM_MATERIAL_LOADER()
VKM_PATH_TRACING_DECLARE()

// Load(), not Sample(): this pass is 1:1 with the G-buffer's pixels, so there is nothing to filter,
// and a compute shader has no derivatives for an implicit-LOD sample anyway.
[[vk::binding(0, 2)]] Texture2D g_Normal    : register(t0, space2);
[[vk::binding(1, 2)]] Texture2D g_BaseColor : register(t1, space2);
[[vk::binding(2, 2)]] Texture2D g_Motion    : register(t2, space2); // w = distance from the camera
// rgb = summed radiance, a = samples in that sum. Same layout the reference accumulates into, so
// the two are directly comparable by vkmComputeImageMse.
[[vk::binding(3, 2)]] RWStructuredBuffer<float4> g_Accumulation : register(u0, space2);

// Distinct from any other pass sampling on the same pixel in the same frame; see vkm_random.hlsli.
static const uint kIndirectPassId = 11u;

[numthreads(8, 8, 1)]
void CSMain(uint3 threadId : SV_DispatchThreadID)
{
    const uint2 pixel = threadId.xy;
    if (pixel.x >= g_Indirect.width || pixel.y >= g_Indirect.height)
    {
        return;
    }
    const uint pixelIndex = pixel.y * g_Indirect.width + pixel.x;

    const float4 motion = g_Motion.Load(int3(pixel, 0));
    const float cameraDistance = motion.w;
    if (cameraDistance <= 0.0)
    {
        // Nothing was rasterized here. Left unaccumulated on purpose rather than written as black:
        // a sample count of zero means "unknown", which vkmComputeImageMse skips, so background
        // pixels cannot flatter or penalise a comparison against the reference.
        return;
    }

    const float2 uv = (float2(pixel) + 0.5) / float2(g_Indirect.width, g_Indirect.height);
    const float3 position = vkmReconstructWorldPosition(uv, cameraDistance,
                                                        g_VkmFrame.inverseViewProjection,
                                                        g_VkmFrame.cameraPositionWorld.xyz);
    // The geometric normal, not the shading one: this is the surface a ray leaves from, and the
    // two differ on normal-mapped geometry in exactly the way that causes self-intersection.
    const float3 geometricNormal = vkmUnpackGeometricNormal(g_Normal.Load(int3(pixel, 0)));
    const float3 albedo = g_BaseColor.Load(int3(pixel, 0)).rgb;

    uint rng = vkmSeed(pixel, g_Indirect.sampleIndex, kIndirectPassId);

    const float3 environment =
        float3(g_Indirect.environmentR, g_Indirect.environmentG, g_Indirect.environmentB);

    // One ray. Cosine-weighted around the geometric normal, so -- exactly as inside vkmTracePath --
    // the BRDF, the cosine and the pdf collapse to the albedo and nothing is divided by a pdf.
    const float3 direction = vkmCosineHemisphere(geometricNormal, vkmRandomFloat2(rng));
    const float3 origin = vkmOffsetRayOrigin(position, geometricNormal, cameraDistance);

    // maxBounces counts scatters from here, matching what the reference does after ITS primary
    // hit -- so a reference run with maxBounces + 1 is the image this converges to.
    const float3 incoming = vkmTracePath(origin, direction, g_Indirect.maxBounces,
                                         environment, g_FrameData[0].materialPoolSlot, rng);
    const float3 radiance = albedo * incoming;

    const float4 previous = (g_Indirect.sampleIndex == 0) ? float4(0.0, 0.0, 0.0, 0.0)
                                                          : g_Accumulation[pixelIndex];
    g_Accumulation[pixelIndex] = float4(previous.rgb + radiance, previous.a + 1.0);
}
