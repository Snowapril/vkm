// Copyright (c) 2025 Snowapril
//
// Shader-side mirror of the probe volume's atlas layout and its lookup (see renderer/probe_volume.h
// for the storage, which is the single source of truth for formats, resolutions and the border).
//
// Included by both the pass that fills the atlases and every pass that reads them, so the
// addressing exists in one place. A lookup carrying its own copy of the octahedral or cell maths
// would drift the moment the layout changed, and the symptom is lighting that is subtly wrong
// everywhere rather than an error anywhere.

#ifndef VKM_PROBE_VOLUME_HLSLI
#define VKM_PROBE_VOLUME_HLSLI

// Mirrors vkm::VkmProbeVolumeConstants (renderer/probe_volume.h), byte for byte.
struct VkmProbeVolumeConstants
{
    float4 originAndSpacingX;   // xyz = grid origin, w = spacing.x
    float4 spacingYZAndCounts;  // x = spacing.y, y = spacing.z, zw unused
    uint4  probeCounts;         // xyz = probes per axis, w unused
    float4 atlasParams;         // x = irradiance resolution, y = distance resolution,
                                // z = normal bias, w = hysteresis
};

float3 vkmProbeGridOrigin(VkmProbeVolumeConstants c)  { return c.originAndSpacingX.xyz; }
float3 vkmProbeGridSpacing(VkmProbeVolumeConstants c)
{
    return float3(c.originAndSpacingX.w, c.spacingYZAndCounts.x, c.spacingYZAndCounts.y);
}

// --- Octahedral mapping -------------------------------------------------------------------
// Same construction as vkm_gbuffer.hlsli's normal encoding, but expressed as a [0,1] UV because a
// probe's map is a texture rather than two stored channels.

float2 vkmProbeDirectionToOctUv(float3 direction)
{
    const float3 n = direction / (abs(direction.x) + abs(direction.y) + abs(direction.z));
    float2 oct = n.xy;
    if (n.z < 0.0)
    {
        oct = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return oct * 0.5 + 0.5;
}

float3 vkmProbeOctUvToDirection(float2 uv)
{
    const float2 oct = uv * 2.0 - 1.0;
    float3 n = float3(oct.x, oct.y, 1.0 - abs(oct.x) - abs(oct.y));
    if (n.z < 0.0)
    {
        n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return normalize(n);
}

// --- Atlas addressing ---------------------------------------------------------------------

uint vkmProbeIndex(uint3 coord, uint3 probeCounts)
{
    return coord.x + coord.y * probeCounts.x + coord.z * probeCounts.x * probeCounts.y;
}

/*
* @brief Atlas UV for a direction at one probe.
*
* The returned UV addresses the probe's *interior*, never its border: the octahedral UV is scaled
* into the interior and offset past the border texel. That is the whole point of the border -- a
* bilinear tap at an interior edge then blends with the replicated opposite edge instead of with a
* neighbouring probe's texels.
*/
float2 vkmProbeAtlasUv(uint3 coord, uint3 probeCounts, float probeResolution, float2 octUv)
{
    const float cellSize = probeResolution + 2.0; // one border texel each side
    const float2 cellCoord = float2(coord.y * probeCounts.x + coord.x, coord.z);
    const float2 atlasSize = float2(probeCounts.x * probeCounts.y, probeCounts.z) * cellSize;

    const float2 interiorTexel = octUv * probeResolution;
    const float2 texel = cellCoord * cellSize + 1.0 + interiorTexel;
    return texel / atlasSize;
}

/*
* @brief Chebyshev visibility: how likely the probe is to see a point `distanceToProbe` away.
*
* This is what stops irradiance leaking through walls. `moments` holds the mean and mean-squared
* distance the probe recorded in that direction; a surface further away than the mean is probably
* behind whatever the probe actually saw, and the variance turns that into a soft weight rather
* than a binary test. Returns 1 for surfaces at or in front of the mean.
*/
float vkmProbeChebyshevWeight(float2 moments, float distanceToProbe)
{
    const float mean = moments.x;
    if (distanceToProbe <= mean)
    {
        return 1.0;
    }
    const float variance = abs(mean * mean - moments.y);
    const float delta = distanceToProbe - mean;
    // Biased towards rejecting: an over-eager weight leaks light, which reads as a bug, while an
    // over-cautious one only darkens a corner.
    const float weight = variance / (variance + delta * delta);
    return max(weight * weight * weight, 0.0);
}

#endif // VKM_PROBE_VOLUME_HLSLI
