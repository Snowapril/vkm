// Copyright (c) 2025 Snowapril
//
// The low-spec GI tier's output pass: reads the G-buffer, samples the probe volume, and writes the
// indirect diffuse radiance the composite consumes.
//
// Structured like the deferred lighting pass -- fullscreen triangle, inputs through descriptor
// set 2 so it runs on every backend and needs no VkmScene -- and it shares the probe addressing
// with whatever fills the atlases through vkm_probe_volume.hlsli.
//
// Two corrections are what make a probe grid usable rather than a source of artifacts:
//
//   - The lookup position is pushed along the surface normal before locating the cell. Without
//     that bias a surface sitting exactly on a probe plane self-shadows against its own probes.
//   - Each of the eight surrounding probes is weighted by a Chebyshev visibility test against the
//     distance atlas, so a probe on the far side of a wall contributes nothing. Trilinear weights
//     alone leak light through every wall thinner than the probe spacing.

#include "vkm_fullscreen.hlsli"
#include "vkm_frame_constants.hlsli"
#include "vkm_gbuffer.hlsli"
#include "vkm_probe_volume.hlsli"

VKM_FRAME_CONSTANTS(g_VkmFrame);

[[vk::binding(0, 2)]] Texture2D    g_GBufferNormal : register(t0, space2);
// Camera distance rides in .w; the depth attachment is never bound (see vkm_gbuffer.hlsli).
[[vk::binding(1, 2)]] Texture2D    g_GBufferMotion : register(t1, space2);
[[vk::binding(2, 2)]] Texture2D    g_Irradiance    : register(t2, space2);
[[vk::binding(3, 2)]] Texture2D    g_Distance      : register(t3, space2);
[[vk::binding(4, 2)]] SamplerState g_Sampler       : register(s0, space2);
[[vk::binding(5, 2)]] ConstantBuffer<VkmProbeVolumeConstants> g_Volume : register(b0, space2);

typedef VkmFullscreenVSOutput VSOutput;

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    return vkmFullscreenTriangle(vertexId);
}

/*
* @brief Trilinearly interpolated, visibility-weighted irradiance at a surface.
*
* Walks the eight probes surrounding the (biased) position, weighting each by its trilinear share,
* a cosine term rejecting probes behind the surface, and the Chebyshev visibility test.
*/
float3 sampleProbeVolume(float3 worldPosition, float3 normal)
{
    const float3 origin = vkmProbeGridOrigin(g_Volume);
    const float3 spacing = vkmProbeGridSpacing(g_Volume);
    const uint3 counts = g_Volume.probeCounts.xyz;
    const float normalBias = g_Volume.atlasParams.z;

    // Biased along the normal so a surface does not locate itself against probes it is coplanar
    // with, which otherwise reads as self-shadowing.
    const float3 biasedPosition = worldPosition + normal * normalBias;

    const float3 gridCoord = (biasedPosition - origin) / spacing;
    const float3 baseCoordF = floor(gridCoord);
    const float3 alpha = saturate(gridCoord - baseCoordF);
    const int3 baseCoord = int3(baseCoordF);

    float3 accumulated = float3(0.0, 0.0, 0.0);
    float totalWeight = 0.0;

    for (uint corner = 0; corner < 8; ++corner)
    {
        const int3 offset = int3(corner & 1, (corner >> 1) & 1, (corner >> 2) & 1);
        const int3 probeCoordI = baseCoord + offset;
        // Outside the grid entirely: contributes nothing rather than clamping, which would smear
        // the boundary probes outwards across the whole scene.
        if (any(probeCoordI < int3(0, 0, 0)) || any(probeCoordI >= int3(counts)))
        {
            continue;
        }
        const uint3 probeCoord = uint3(probeCoordI);

        const float3 trilinear = lerp(1.0 - alpha, alpha, float3(offset));
        float weight = trilinear.x * trilinear.y * trilinear.z;

        const float3 probePosition = origin + float3(probeCoord) * spacing;
        /*
        * Measured from the BIASED point, not the surface. This is what the bias is for: a probe
        * stores the distance to the nearest surface in each direction, and for thin two-sided
        * geometry -- a curtain, a leaf -- that surface IS this one, so a query from the surface
        * itself sits exactly at the stored depth and the Chebyshev test reads it as occluded by
        * itself. Stepping off along the normal first puts the query comfortably in front of what
        * the probe recorded. Biasing only the cell lookup (as this did) fixes which probes are
        * consulted and none of what they answer.
        */
        const float3 toProbe = probePosition - biasedPosition;
        const float distanceToProbe = length(toProbe);
        const float3 directionToProbe = distanceToProbe > 1e-6 ? toProbe / distanceToProbe : normal;

        // A probe behind the surface has nothing to say about it. Smoothed rather than clipped so
        // the term does not pop as the surface turns.
        const float cosTerm = (dot(normal, directionToProbe) + 1.0) * 0.5;
        weight *= cosTerm * cosTerm + 0.05;

        const float2 distanceUv =
            vkmProbeAtlasUv(probeCoord, counts, g_Volume.atlasParams.y,
                            vkmProbeDirectionToOctUv(-directionToProbe));
        const float2 moments = g_Distance.SampleLevel(g_Sampler, distanceUv, 0).rg;
        weight *= vkmProbeChebyshevWeight(moments, distanceToProbe);

        if (weight <= 0.0)
        {
            continue;
        }

        const float2 irradianceUv =
            vkmProbeAtlasUv(probeCoord, counts, g_Volume.atlasParams.x,
                            vkmProbeDirectionToOctUv(normal));
        accumulated += g_Irradiance.SampleLevel(g_Sampler, irradianceUv, 0).rgb * weight;
        totalWeight += weight;
    }

    // Every surrounding probe was rejected (outside the grid, or all occluded). Returning black is
    // the honest answer; normalizing by a near-zero weight would amplify whatever survived.
    if (totalWeight <= 1e-6)
    {
        return float3(0.0, 0.0, 0.0);
    }
    return accumulated / totalWeight;
}

float4 PSMain(VSOutput input) : SV_TARGET0
{
    const float cameraDistance = g_GBufferMotion.SampleLevel(g_Sampler, input.uv, 0).w;
    if (cameraDistance <= 0.0)
    {
        return float4(0.0, 0.0, 0.0, 1.0); // never covered by geometry
    }

    const float3 shadingNormal = vkmUnpackShadingNormal(g_GBufferNormal.SampleLevel(g_Sampler, input.uv, 0));
    const float3 worldPosition = vkmReconstructWorldPosition(
        input.uv, cameraDistance, g_VkmFrame.inverseViewProjection, g_VkmFrame.cameraPositionWorld.xyz);

    return float4(sampleProbeVolume(worldPosition, shadingNormal), 1.0);
}
