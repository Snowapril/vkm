// Copyright (c) 2026 Snowapril
//
// Phase 8.3: one traced ray per pixel, written into a fresh reservoir.
//
// The estimator is exactly Phase 7's -- same G-buffer surface, same cosine-weighted direction, same
// path continuation -- and that is deliberate. With one candidate, RIS reduces to the estimator it
// resamples: w = p_hat/p_source, w_sum = w, and W = w_sum / (M * p_hat) = 1/p_source. Resolving
// such a reservoir therefore has to reproduce gi_indirect.hlsl sample for sample, and the *only*
// thing that can make the two differ is the reservoir round trip. That is what makes this
// sub-step's gate sharp: the difference between them measures the packing and nothing else.
//
// It even shares gi_indirect's random stream on purpose (see kIndirectPassId below): the two never
// run in the same frame -- one replaces the other -- so the usual rule about distinct pass ids does
// not apply, and sharing lets the comparison be sample-for-sample rather than merely statistical.

#include "vkm_bindless.hlsli"
#include "vkm_frame_constants.hlsli"
#include "vkm_gbuffer.hlsli"
#include "vkm_material.hlsli"
#include "vkm_path_tracing.hlsli"
#include "vkm_random.hlsli"
#include "vkm_reservoir.hlsli"

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
    uint   lightPoolSlot;
    uint   lightCount;
};

// Mirrors vkm::VkmRestirConstants (include/vkm/renderer/restir.h).
struct RestirConstants
{
    uint  width;
    uint  height;
    uint  sampleIndex;
    uint  maxBounces;
    float environmentR;
    float environmentG;
    float environmentB;
    uint  outputSlice;
    uint  inputSlice;
    uint  neighbourCount;
    float neighbourRadius;
    float normalThreshold;
    float depthThreshold;
    uint  historySlice;
    uint  confidenceCap;
    uint  maxSampleAge;
};

VKM_PUSH_CONSTANTS(RestirConstants, g_Restir);
VKM_FRAME_CONSTANTS(g_VkmFrame);

VKM_BINDLESS_VERTEX_PULLING(uint);
VKM_BINDLESS_OBJECT_DATA(ObjectData, g_ObjectData);
VKM_BINDLESS_FRAME_DATA(FrameData, g_FrameData);
VKM_BINDLESS_ACCELERATION_STRUCTURE(g_Scene);
VKM_MATERIAL_LOADER()
VKM_LIGHT_LOADER()
VKM_PATH_TRACING_DECLARE()

[[vk::binding(0, 2)]] Texture2D g_Normal    : register(t0, space2);
[[vk::binding(1, 2)]] Texture2D g_Motion    : register(t1, space2); // w = distance from the camera
[[vk::binding(2, 2)]] RWStructuredBuffer<uint> g_Reservoirs : register(u0, space2);

VKM_RESERVOIR_DECLARE()

// Deliberately gi_indirect.hlsl's id, not a fresh one -- see the header note.
static const uint kIndirectPassId = 11u;

// Where an escaped ray's sample is placed. Far enough that the direction to it from the shading
// point is the direction that was traced to within a rounding error, and near enough that fp32
// still resolves the difference cleanly. Such a sample carries VKM_RESERVOIR_FLAG_ENVIRONMENT,
// because 8.4 must not give an infinitely distant sample a 1/d^2 Jacobian.
static const float kEnvironmentSampleDistance = 1.0e5;

[numthreads(8, 8, 1)]
void CSMain(uint3 threadId : SV_DispatchThreadID)
{
    const uint2 pixel = threadId.xy;
    if (pixel.x >= g_Restir.width || pixel.y >= g_Restir.height)
    {
        return;
    }
    const uint pixelIndex = pixel.y * g_Restir.width + pixel.x;
    const uint pixelCount = g_Restir.width * g_Restir.height;
    const uint wordBase = vkmReservoirWordBase(g_Restir.outputSlice, pixelIndex, pixelCount);

    const float4 motion = g_Motion.Load(int3(pixel, 0));
    const float cameraDistance = motion.w;
    if (cameraDistance <= 0.0)
    {
        // Nothing was rasterized here. An empty reservoir rather than a stale one: M = 0 is what
        // every later pass reads as "no sample", and leaving last frame's bytes would make a
        // disoccluded pixel resample a surface that is no longer there.
        vkmStoreReservoir(wordBase, vkmEmptyReservoir());
        return;
    }

    const float2 uv = (float2(pixel) + 0.5) / float2(g_Restir.width, g_Restir.height);
    const float3 position = vkmReconstructWorldPosition(uv, cameraDistance,
                                                        g_VkmFrame.inverseViewProjection,
                                                        g_VkmFrame.cameraPositionWorld.xyz);
    const float3 geometricNormal = vkmUnpackGeometricNormal(g_Normal.Load(int3(pixel, 0)));

    uint rng = vkmSeed(pixel, g_Restir.sampleIndex, kIndirectPassId);

    const float3 environment =
        float3(g_Restir.environmentR, g_Restir.environmentG, g_Restir.environmentB);

    const float3 direction = vkmCosineHemisphere(geometricNormal, vkmRandomFloat2(rng));
    const float3 origin = vkmOffsetRayOrigin(position, geometricNormal, cameraDistance);

    bool hit;
    VkmSurfacePoint first;
    const float3 incoming = vkmTracePathFirstHit(origin, direction, g_Restir.maxBounces,
                                                 environment, g_FrameData[0].materialPoolSlot,
                                                 rng, hit, first);

    VkmReservoir reservoir = vkmEmptyReservoir();
    reservoir.radiance = incoming;
    reservoir.sampleCount = 1;
    reservoir.age = 0;
    if (hit)
    {
        reservoir.samplePosition = first.position;
        reservoir.sampleNormal = first.geometricNormal;
    }
    else
    {
        reservoir.samplePosition = origin + direction * kEnvironmentSampleDistance;
        reservoir.sampleNormal = -direction;
        reservoir.flags = VKM_RESERVOIR_FLAG_ENVIRONMENT;
    }

    /*
    * W = 1/p_source, which is what RIS collapses to for a single candidate. The source pdf is the
    * cosine-weighted one this ray was drawn from, cos(theta)/pi about the geometric normal, so
    * W = pi / cos(theta).
    *
    * The cosine is recovered from the sampled direction rather than assumed: vkmCosineHemisphere
    * normalizes its result, so the two agree, and taking it from the direction is what the resolve
    * pass will also do -- from the stored position. A reservoir that stored W against one cosine
    * and were resolved against another would be quietly biased.
    *
    * A grazing sample makes W large and its f_s * cos small by exactly the same factor, so the
    * product is stable; cosine sampling makes such a sample rare in the first place.
    */
    const float cosTheta = max(dot(geometricNormal, direction), 1.0e-4);
    reservoir.weight = 3.14159265358979 / cosTheta;

    vkmStoreReservoir(wordBase, reservoir);
}
