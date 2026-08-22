// Copyright (c) 2026 Snowapril
//
// Phase 8.5: temporal resampling. Each pixel merges last frame's reservoir into this frame's
// fresh one, which is where ReSTIR's large quality win comes from: confidence grows every frame,
// so a pixel effectively chooses among many candidates while tracing one new ray.
//
// This pass casts no rays. The history sample was generated at (up to reprojection) this same
// surface, disocclusions are rejected from the previous G-buffer below, and the resampled result
// still passes through the spatial pass's visibility rays and the resolve. Its target function is
// therefore p_hat = luminance * cos, rayless but the SAME in the merge weights and in the
// bias-correction denominator -- the symmetry 8.4 established is what makes the stage unbiased
// for its own p_hat. The spatial pass's p_hat carries V; each stage is internally consistent,
// and that per-stage choice is deliberate (see restir.md section 8.5).
//
// The reconnection Jacobian is 1 here: geometry is static and motion vectors are camera-only, so
// the shift maps a surface point to itself. The day object motion exists, the reprojected surface
// is a different world-space point and this needs the same Jacobian the spatial pass applies.
//
// Which history pixel to trust is decided from the PREVIOUS G-buffer alone -- position, normal,
// camera distance -- never from the history reservoir's contents (course Tip 4.1, the same hard
// rule the spatial pass makes structural).

#include "vkm_bindless.hlsli"
#include "vkm_frame_constants.hlsli"
#include "vkm_gbuffer.hlsli"
#include "vkm_random.hlsli"
#include "vkm_reservoir.hlsli"

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
    float normalThreshold;
    float depthThreshold;
    uint  historySlice;
    uint  confidenceCap;
    uint  maxSampleAge;
};

VKM_PUSH_CONSTANTS(RestirConstants, g_Restir);
VKM_FRAME_CONSTANTS(g_VkmFrame);

[[vk::binding(0, 2)]] Texture2D g_Normal     : register(t0, space2);
[[vk::binding(1, 2)]] Texture2D g_Motion     : register(t1, space2); // w = distance from the camera
[[vk::binding(2, 2)]] Texture2D g_PrevNormal : register(t2, space2);
[[vk::binding(3, 2)]] Texture2D g_PrevMotion : register(t3, space2);
[[vk::binding(4, 2)]] RWStructuredBuffer<uint> g_Reservoirs : register(u0, space2);

VKM_RESERVOIR_DECLARE()

static const uint kTemporalPassId = 29u;

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

    const float4 motion = g_Motion.Load(int3(pixel, 0));
    const float cameraDistance = motion.w;
    if (cameraDistance <= 0.0)
    {
        vkmStoreReservoir(outputBase, vkmEmptyReservoir());
        return;
    }

    // The fresh 1-spp reservoir generation just wrote. Its Jacobian is 1 by construction and its
    // sample is visible by construction -- it was traced from this pixel this frame.
    const VkmReservoir fresh =
        vkmLoadReservoir(vkmReservoirWordBase(g_Restir.inputSlice, pixelIndex, pixelCount));

    const float2 uv = (float2(pixel) + 0.5) / float2(g_Restir.width, g_Restir.height);
    const float3 position = vkmReconstructWorldPosition(uv, cameraDistance,
                                                        g_VkmFrame.inverseViewProjection,
                                                        g_VkmFrame.cameraPositionWorld.xyz);
    const float3 normal = vkmUnpackGeometricNormal(g_Normal.Load(int3(pixel, 0)));

    /*
    * Reproject: the motion vector is where this pixel WAS, in UV units (vkm_gbuffer.hlsli), so
    * subtracting it from the current UV lands on last frame's tap. A static camera makes it zero
    * and the tap is this very pixel.
    */
    const float2 prevUv = uv - motion.xy;
    const int2 prevPixel = int2(floor(prevUv * float2(g_Restir.width, g_Restir.height)));

    VkmReservoir history = vkmEmptyReservoir();
    float3 prevNormal = normal;
    bool historyValid = prevPixel.x >= 0 && prevPixel.y >= 0 &&
                        prevPixel.x < (int)g_Restir.width && prevPixel.y < (int)g_Restir.height;
    if (historyValid)
    {
        // Validated against the previous G-buffer only. The previous camera distance is radial
        // from the PREVIOUS camera position, so the current surface is measured against that
        // origin -- with a static camera the two distances are identical.
        const float prevCameraDistance = g_PrevMotion.Load(int3(prevPixel, 0)).w;
        prevNormal = vkmUnpackGeometricNormal(g_PrevNormal.Load(int3(prevPixel, 0)));
        const float prevDistanceOfSurface =
            length(position - g_VkmFrame.prevCameraPositionWorld.xyz);
        historyValid = prevCameraDistance > 0.0 &&
                       dot(prevNormal, normal) >= g_Restir.normalThreshold &&
                       abs(prevCameraDistance - prevDistanceOfSurface) <=
                           g_Restir.depthThreshold * prevCameraDistance;
    }
    if (historyValid)
    {
        history = vkmLoadReservoir(
            vkmReservoirWordBase(g_Restir.historySlice, uint(prevPixel.y) * g_Restir.width + uint(prevPixel.x),
                                 pixelCount));
        // Age is the sample's, not the reservoir's: it counts frames since the carried sample was
        // generated, because its cached radiance goes stale even where the surface is the same.
        // A zero weight does NOT invalidate: a null sample is a legitimate outcome, and its
        // confidence still belongs in the denominator below (restir.md section 9) -- dropping it
        // shrinks Z and brightens, the same asymmetry 8.4 was.
        history.age += 1;
        if (history.sampleCount == 0 || history.age > g_Restir.maxSampleAge)
        {
            historyValid = false;
        }
        history.sampleCount = min(history.sampleCount, g_Restir.confidenceCap);
    }

    if (!historyValid)
    {
        // Disocclusion, out-of-bounds, or a stale sample: the fresh reservoir stands alone. This
        // is also every pixel of the first frame, whose "previous" G-buffer holds nothing.
        vkmStoreReservoir(outputBase, fresh);
        return;
    }

    uint rng = vkmSeed(pixel, g_Restir.sampleIndex, kTemporalPassId);

    /*
    * Two-participant merge under p_hat = luminance * cos at this pixel, then the bias-correction
    * denominator over the same two participants (Bitterli et al. 2020, algorithm 6). The history
    * participant's own p_hat is approximated with last frame's normal at the reprojected tap and
    * this frame's position -- last frame's true p_hat needs last frame's scene, which nobody
    * keeps. Sign rule (restir.md section 9): over-estimating it darkens, under-estimating
    * brightens.
    */
    const float freshTargetPdf =
        vkmReservoirTargetPdfAt(fresh.radiance, position, normal, fresh.samplePosition);
    const float historyTargetPdf =
        vkmReservoirTargetPdfAt(history.radiance, position, normal, history.samplePosition);

    float weightSum = 0.0;
    VkmReservoir chosen = fresh;
    float chosenTargetPdf = freshTargetPdf;
    uint mergedSampleCount = 0;

    if (fresh.sampleCount > 0)
    {
        weightSum = freshTargetPdf * fresh.weight * float(fresh.sampleCount);
        mergedSampleCount = fresh.sampleCount;
    }

    // The history participates whatever its weight: a zero contribution cannot be chosen, but
    // its confidence still counts -- in M here and in Z below, symmetrically.
    const float historyWeight = max(historyTargetPdf * history.weight, 0.0) * float(history.sampleCount);
    weightSum += historyWeight;
    mergedSampleCount += history.sampleCount;
    if (historyWeight > 0.0 && vkmRandomFloat(rng) * weightSum < historyWeight)
    {
        chosen = history;
        chosenTargetPdf = historyTargetPdf;
    }

    float normalization = 0.0;
    if (chosen.sampleCount > 0 && chosenTargetPdf > 0.0)
    {
        if (vkmReservoirTargetPdfAt(chosen.radiance, position, normal, chosen.samplePosition) > 0.0)
        {
            normalization += float(fresh.sampleCount);
        }
        if (vkmReservoirTargetPdfAt(chosen.radiance, position, prevNormal, chosen.samplePosition) > 0.0)
        {
            normalization += float(history.sampleCount);
        }
    }

    VkmReservoir result = chosen;
    result.sampleCount = mergedSampleCount;
    result.age = chosen.age;
    result.flags = chosen.flags;
    result.weight = (normalization > 0.0 && chosenTargetPdf > 0.0)
                        ? weightSum / (normalization * chosenTargetPdf)
                        : 0.0;
    vkmStoreReservoir(outputBase, result);
}
