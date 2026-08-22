// Copyright (c) 2026 Snowapril
//
// Phase 8.4: spatial resampling. Each pixel merges k neighbours' reservoirs into its own.
//
// Spatial before temporal, because it is the one that can be validated: nothing in the scene moves
// between the samples being combined, so a shifted mean is a broken Jacobian or a broken
// bias-correction denominator and cannot be blamed on reprojection.
//
// **The hard rule (course Tip 4.1) is structural here, not a comment:** which neighbours are
// reused is decided from the G-buffer alone -- position, normal, camera distance. Nothing about a
// neighbour's stored sample or weight may enter that decision, because choosing on the samples
// conditions the probability space the estimator is defined over and biases the result. The
// Jacobian rejection below is not an exception to this: a shift with a sample at or behind its own
// horizon is undefined rather than merely unpromising, and the bias-correction denominator rejects
// exactly the same configurations.
//
// Unbiased, deliberately. The magnitude clamp RTXDI applies to the Jacobian (reject outside
// [0.1, 10]) is NOT applied: it bounds variance at the cost of a mean this sub-step's gate exists
// to check. That trade belongs to 8.7 and to a profiler, not to the step that proves the maths.
//
// The target function here is p_hat = luminance * cos * V, and both loops evaluate the SAME one:
// the merge loop rejects a candidate the centre cannot see, and the denominator Z rejects a
// participant that cannot see the chosen sample. Bitterli's algorithm 6 is unbiased only when the
// numerator's p_hat and Z's agree -- visibility in Z alone leaves every genuinely occluded
// participant out of the divisor while its weight stays in the sum, which measured 13.8% bright
// on the Cornell gate. With the two symmetric the mean lands within 0.3% of the un-resampled
// estimator (TestIndirectPassShared).

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

// Mirrors vkm::VkmRestirConstants.
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
    float normalThreshold;   // cos of the largest accepted angle between two surfaces
    float depthThreshold;    // relative camera-distance difference still considered the same surface
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

[[vk::binding(0, 2)]] Texture2D g_Normal   : register(t0, space2);
[[vk::binding(1, 2)]] Texture2D g_Motion   : register(t1, space2);
// Phase 8.2: precomputed low-discrepancy disk offsets, indexed with a mask. A buffer rather than
// per-pixel disk sampling because the pattern is then stable frame to frame and identical across
// pixels up to a rotation, which is what keeps spatial reuse from adding its own noise.
// RW rather than read-only because VkmTableResourceType has no read-only storage-buffer kind
// (pipeline_state.h); nothing writes it after upload.
[[vk::binding(2, 2)]] RWStructuredBuffer<float2> g_NeighbourOffsets : register(u0, space2);
[[vk::binding(3, 2)]] RWStructuredBuffer<uint> g_Reservoirs : register(u1, space2);

VKM_RESERVOIR_DECLARE()

static const uint kSpatialPassId = 23u;
// Mirrors vkm::kVkmNeighbourOffsetCount; the mask is what makes the index wrap for free.
static const uint kNeighbourOffsetMask = 255u;
// The most neighbours either loop will visit.
#define VKM_SPATIAL_MAX_NEIGHBOURS 8

struct SurfacePoint
{
    float3 position;
    float3 normal;
    float  cameraDistance;
    bool   valid;
};

SurfacePoint loadSurface(uint2 pixel)
{
    SurfacePoint surface;
    surface.position = float3(0.0, 0.0, 0.0);
    surface.normal = float3(0.0, 0.0, 1.0);
    surface.cameraDistance = 0.0;
    surface.valid = false;

    if (pixel.x >= g_Restir.width || pixel.y >= g_Restir.height)
    {
        return surface;
    }
    const float distanceFromCamera = g_Motion.Load(int3(pixel, 0)).w;
    if (distanceFromCamera <= 0.0)
    {
        return surface;
    }
    const float2 uv = (float2(pixel) + 0.5) / float2(g_Restir.width, g_Restir.height);
    surface.position = vkmReconstructWorldPosition(uv, distanceFromCamera,
                                                   g_VkmFrame.inverseViewProjection,
                                                   g_VkmFrame.cameraPositionWorld.xyz);
    surface.normal = vkmUnpackGeometricNormal(g_Normal.Load(int3(pixel, 0)));
    surface.cameraDistance = distanceFromCamera;
    surface.valid = true;
    return surface;
}

// Whether `sample` can be seen from `surface`. The far end is shortened so the ray cannot hit the
// very surface it is aiming at, which is the other half of the offset that keeps it off its own.
bool sampleIsVisible(SurfacePoint surface, float3 samplePosition, float3 sampleNormal)
{
    /*
    * Direction and distance are both measured from the OFFSET origin the ray actually leaves,
    * not from the surface point. Taking them from the surface and shooting from the offset is a
    * quarter-of-a-pixel error in direction and, worse, leaves t_max longer than the distance that
    * remains -- so the ray reaches the very surface it is aiming at and reports its own target as
    * an occluder. That drops pixels out of the bias-correction denominator, which divides by too
    * little and BRIGHTENS the image: 13.8% here, and invisible in anything but a mean.
    */
    const float3 origin = vkmOffsetRayOrigin(surface.position, surface.normal, surface.cameraDistance);
    /*
    * BOTH ends are lifted off their surfaces, and the far one matters as much as the near one. A
    * segment that ends exactly ON a triangle meets it at whatever angle the geometry dictates, and
    * at a grazing one no *relative* shortening of t_max clears it: the perpendicular gap goes to
    * zero with the cosine while the shortening stays proportional to length. The ray then reports
    * its own target as an occluder, which drops that pixel out of the bias-correction denominator
    * -- dividing by too little and brightening the image by 13.8% here, with nothing to see but a
    * mean that moved.
    */
    const float3 target = vkmOffsetRayOrigin(samplePosition, sampleNormal,
                                             length(samplePosition - origin));
    const float3 toSample = target - origin;
    const float distanceToSample = length(toSample);
    if (distanceToSample <= 0.0)
    {
        return false;
    }

    RayDesc rayDesc;
    rayDesc.Origin = origin;
    rayDesc.Direction = toSample / distanceToSample;
    rayDesc.TMin = 0.0;
    // t_max = (1 - eps) * d: anything closer than the sample occludes it, the sample itself does
    // not. Without this every visibility ray reports its own target as an occluder.
    rayDesc.TMax = distanceToSample * 0.999;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;
    query.TraceRayInline(g_Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, rayDesc);
    query.Proceed();
    return query.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}

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
    const uint outputBase = vkmReservoirWordBase(g_Restir.outputSlice, pixelIndex, pixelCount);

    const SurfacePoint centre = loadSurface(pixel);
    if (!centre.valid)
    {
        vkmStoreReservoir(outputBase, vkmEmptyReservoir());
        return;
    }

    uint rng = vkmSeed(pixel, g_Restir.sampleIndex, kSpatialPassId);

    // The canonical sample: this pixel's own reservoir, whose Jacobian is 1 by construction.
    const VkmReservoir canonical =
        vkmLoadReservoir(vkmReservoirWordBase(g_Restir.inputSlice, pixelIndex, pixelCount));

    VkmReservoir chosen = canonical;
    float chosenTargetPdf = vkmReservoirTargetPdfAt(canonical.radiance, centre.position,
                                                    centre.normal, canonical.samplePosition);
    float weightSum = 0.0;
    uint mergedSampleCount = 0;

    if (canonical.sampleCount > 0)
    {
        weightSum = chosenTargetPdf * canonical.weight * float(canonical.sampleCount);
        mergedSampleCount = canonical.sampleCount;
    }

    const uint neighbourCount = min(g_Restir.neighbourCount, (uint)VKM_SPATIAL_MAX_NEIGHBOURS);
    const uint offsetBase = vkmRandomUint(rng);
    for (uint neighbour = 0; neighbour < neighbourCount; ++neighbour)
    {
        const float2 offset =
            g_NeighbourOffsets[(offsetBase + neighbour) & kNeighbourOffsetMask] * g_Restir.neighbourRadius;
        const int2 candidate = int2(pixel) + int2(round(offset));
        if (candidate.x < 0 || candidate.y < 0 ||
            candidate.x >= (int)g_Restir.width || candidate.y >= (int)g_Restir.height ||
            (candidate.x == (int)pixel.x && candidate.y == (int)pixel.y))
        {
            continue;
        }

        const SurfacePoint neighbourSurface = loadSurface(uint2(candidate));
        // G-buffer only. Nothing below reads the neighbour's reservoir before this point, and that
        // ordering is the hard rule made structural.
        if (!neighbourSurface.valid ||
            dot(neighbourSurface.normal, centre.normal) < g_Restir.normalThreshold ||
            abs(neighbourSurface.cameraDistance - centre.cameraDistance) >
                g_Restir.depthThreshold * centre.cameraDistance)
        {
            continue;
        }

        const uint neighbourIndex = uint(candidate.y) * g_Restir.width + uint(candidate.x);
        const VkmReservoir neighbourReservoir =
            vkmLoadReservoir(vkmReservoirWordBase(g_Restir.inputSlice, neighbourIndex, pixelCount));
        if (neighbourReservoir.sampleCount == 0 || neighbourReservoir.weight <= 0.0)
        {
            continue;
        }

        const float jacobian = vkmReservoirJacobian(neighbourReservoir.samplePosition,
                                                    neighbourReservoir.sampleNormal,
                                                    neighbourSurface.position, centre.position);
        if (!(jacobian > 0.0))
        {
            continue;
        }

        // p_hat includes visibility, here and in Z below. The two loops must evaluate the same
        // target function: a candidate occluded from the centre contributes zero target density,
        // and letting it into the numerator while Z's ray excludes its supporters divides by too
        // little and brightens the result.
        if (!sampleIsVisible(centre, neighbourReservoir.samplePosition, neighbourReservoir.sampleNormal))
        {
            continue;
        }

        const float targetPdf = vkmReservoirTargetPdfAt(neighbourReservoir.radiance, centre.position,
                                                        centre.normal, neighbourReservoir.samplePosition);
        // The sample's contribution weight expressed in THIS pixel's measure is W * jacobian; the
        // target pdf is this pixel's own, with no Jacobian in it. Folding the Jacobian into p_hat
        // instead also works, but only if the denominator below folds it in too -- keeping them
        // apart is what makes that impossible to get half-right.
        const float weight = targetPdf * neighbourReservoir.weight * jacobian *
                             float(neighbourReservoir.sampleCount);
        if (!(weight > 0.0))
        {
            continue;
        }

        weightSum += weight;
        mergedSampleCount += neighbourReservoir.sampleCount;
        if (vkmRandomFloat(rng) * weightSum < weight)
        {
            chosen = neighbourReservoir;
            chosenTargetPdf = targetPdf;
        }
    }

    /*
    * The bias-correction denominator (Bitterli et al. 2020, algorithm 6). Z counts the samples of
    * every pixel that COULD have produced the chosen sample -- which is not the same as every
    * pixel that was merged. A pixel from whose surface the chosen sample is below the horizon or
    * occluded had no chance of generating it, and counting it would divide by too much and darken
    * the result.
    *
    * This is the second loop the plan calls for, and the visibility ray it casts per accepted
    * neighbour is the expensive half of being unbiased rather than merely plausible.
    */
    float normalization = 0.0;
    if (chosen.sampleCount > 0 && chosenTargetPdf > 0.0)
    {
        if (vkmReservoirTargetPdfAt(chosen.radiance, centre.position, centre.normal,
                                    chosen.samplePosition) > 0.0 &&
            sampleIsVisible(centre, chosen.samplePosition, chosen.sampleNormal))
        {
            normalization += float(canonical.sampleCount);
        }
        /*
        * The same neighbours, re-derived rather than remembered. `offsetBase` and the thresholds
        * are unchanged, so this walks exactly the set the first loop merged -- and rejecting on
        * different grounds in the two loops would be a bias, not an optimization.
        *
        * Recomputed rather than carried in an on-stack array on purpose: an indexable local array
        * in a compute shader is scratch memory, and reading a surface back out of one produced a
        * neighbour whose visibility ray rejected every time -- Z short by a fifth, the image 13.8%
        * bright, and nothing to see but a mean that moved.
        */
        for (uint accepted = 0; accepted < neighbourCount; ++accepted)
        {
            const float2 offset =
                g_NeighbourOffsets[(offsetBase + accepted) & kNeighbourOffsetMask] * g_Restir.neighbourRadius;
            const int2 candidate = int2(pixel) + int2(round(offset));
            if (candidate.x < 0 || candidate.y < 0 ||
                candidate.x >= (int)g_Restir.width || candidate.y >= (int)g_Restir.height ||
                (candidate.x == (int)pixel.x && candidate.y == (int)pixel.y))
            {
                continue;
            }
            const SurfacePoint neighbourSurface = loadSurface(uint2(candidate));
            if (!neighbourSurface.valid ||
                dot(neighbourSurface.normal, centre.normal) < g_Restir.normalThreshold ||
                abs(neighbourSurface.cameraDistance - centre.cameraDistance) >
                    g_Restir.depthThreshold * centre.cameraDistance)
            {
                continue;
            }

            const uint neighbourIndex = uint(candidate.y) * g_Restir.width + uint(candidate.x);
            const VkmReservoir neighbourReservoir =
                vkmLoadReservoir(vkmReservoirWordBase(g_Restir.inputSlice, neighbourIndex, pixelCount));
            if (neighbourReservoir.sampleCount == 0)
            {
                continue;
            }
            if (vkmReservoirTargetPdfAt(chosen.radiance, neighbourSurface.position,
                                        neighbourSurface.normal, chosen.samplePosition) <= 0.0)
            {
                continue;
            }
            if (!sampleIsVisible(neighbourSurface, chosen.samplePosition, chosen.sampleNormal))
            {
                continue;
            }
            normalization += float(neighbourReservoir.sampleCount);
        }
    }

    VkmReservoir result = chosen;
    result.sampleCount = mergedSampleCount;
    result.age = chosen.age;
    result.weight = (normalization > 0.0 && chosenTargetPdf > 0.0)
                        ? weightSum / (normalization * chosenTargetPdf)
                        : 0.0;
    vkmStoreReservoir(outputBase, result);
}
