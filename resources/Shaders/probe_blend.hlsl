// Copyright (c) 2025 Snowapril
//
// Turns one probe's cube capture into atlas contents: integrates the capture into an octahedral
// map and blends it into the previous frame's values.
//
// Two permutations from one source, because the two atlases integrate different quantities at
// different resolutions but are otherwise the same pass:
//   VKM_PROBE_BLEND_IRRADIANCE  cosine-weighted radiance, per direction
//   VKM_PROBE_BLEND_DISTANCE    mean and mean-squared distance, for the Chebyshev test
//
// Two details are what make this correct rather than approximately correct:
//
//   - Border texels are not copied from the interior afterwards. A border texel is mapped to the
//     interior texel it mirrors, and *that* texel's direction is integrated. Same result as a copy
//     pass, but in one pass, and without reading the target being written.
//   - The capture is sampled by projecting a direction through the same face view-projections the
//     capture was rendered with, rather than through a hand-derived cube-face convention. It costs
//     six matrix multiplies per sample and is correct by construction: there is no second
//     convention that can disagree with the first.

#include "vkm_fullscreen.hlsli"
#include "vkm_probe_volume.hlsli"

#if !defined(VKM_PROBE_BLEND_IRRADIANCE) && !defined(VKM_PROBE_BLEND_DISTANCE)
    #error "probe_blend.hlsl requires VKM_PROBE_BLEND_IRRADIANCE or VKM_PROBE_BLEND_DISTANCE"
#endif

// Mirrors vkm::VkmProbeBlendConstants (renderer/probe_volume.h).
struct ProbeBlendConstants
{
    float4x4 faceViewProjection[6];
    float4   probePositionWorld;  // xyz = probe being blended
    float4   blendParams;         // x = octahedral resolution, y = hysteresis,
                                  // z = capture faces per row, w = capture face size in texels
};

[[vk::binding(0, 2)]] Texture2D    g_Capture  : register(t0, space2);
[[vk::binding(1, 2)]] Texture2D    g_Previous : register(t1, space2);
[[vk::binding(2, 2)]] SamplerState g_Sampler  : register(s0, space2);
[[vk::binding(3, 2)]] ConstantBuffer<ProbeBlendConstants> g_Blend : register(b0, space2);

typedef VkmFullscreenVSOutput VSOutput;

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    return vkmFullscreenTriangle(vertexId);
}

/*
* @brief Samples the cube capture along `direction`, returning rgb = radiance, a = distance.
*
* Finds the face by asking each face's own view-projection whether the direction lands inside it,
* which is what keeps this consistent with how the capture was rendered.
*/
float4 sampleCapture(float3 direction)
{
    const float facesPerRow = g_Blend.blendParams.z;
    const float faceSize = g_Blend.blendParams.w;
    const float2 atlasSize = float2(facesPerRow * faceSize, ceil(6.0 / facesPerRow) * faceSize);

    const float3 samplePoint = g_Blend.probePositionWorld.xyz + direction;
    for (uint face = 0; face < 6; ++face)
    {
        const float4 clip = mul(g_Blend.faceViewProjection[face], float4(samplePoint, 1.0));
        if (clip.w <= 0.0)
        {
            continue;
        }
        const float2 ndc = clip.xy / clip.w;
        // A small inset keeps a direction exactly on a face boundary from sampling the neighbouring
        // tile through bilinear filtering.
        if (any(abs(ndc) > 0.995))
        {
            continue;
        }

        // Clip space is +Y up, tile UV is +Y down.
        const float2 faceUv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
        const float2 tileOrigin = float2(fmod(float(face), facesPerRow), floor(float(face) / facesPerRow));
        const float2 atlasUv = (tileOrigin * faceSize + faceUv * faceSize) / atlasSize;
        return g_Capture.SampleLevel(g_Sampler, atlasUv, 0);
    }
    // Every face rejected it, which should not happen for a normalized direction; contributing
    // nothing is safer than guessing a face.
    return float4(0.0, 0.0, 0.0, 0.0);
}

/*
* @brief The interior texel a given cell texel represents.
*
* Interior texels map to themselves. A border texel maps to the interior texel it mirrors under the
* octahedral fold: side borders mirror along the opposite axis, corners map to the diagonally
* opposite interior corner. Returning the mirrored *coordinate* rather than copying its value lets
* the integration below produce the same answer in a single pass.
*/
int2 resolveInteriorTexel(int2 cellTexel, int resolution)
{
    const int last = resolution - 1;
    int2 interior = cellTexel - int2(1, 1); // interior coordinates, may be -1 or `resolution`

    const bool leftOrRight = (interior.x < 0) || (interior.x > last);
    const bool topOrBottom = (interior.y < 0) || (interior.y > last);

    if (leftOrRight && topOrBottom)
    {
        // Corner: the diagonally opposite interior corner.
        interior.x = (interior.x < 0) ? 0 : last;
        interior.y = (interior.y < 0) ? 0 : last;
        return int2(last - interior.x, last - interior.y);
    }
    if (leftOrRight)
    {
        interior.x = (interior.x < 0) ? 0 : last;
        interior.y = last - clamp(interior.y, 0, last);
        return interior;
    }
    if (topOrBottom)
    {
        interior.y = (interior.y < 0) ? 0 : last;
        interior.x = last - clamp(interior.x, 0, last);
        return interior;
    }
    return interior;
}

// A fixed low-discrepancy sphere. Enough directions to integrate a low-frequency diffuse response
// without the cost scaling with the capture's resolution.
static const uint kSampleCount = 64;

float3 fibonacciDirection(uint index)
{
    const float goldenAngle = 2.39996323;
    const float z = 1.0 - (2.0 * float(index) + 1.0) / float(kSampleCount);
    const float radius = sqrt(saturate(1.0 - z * z));
    const float theta = goldenAngle * float(index);
    return float3(radius * cos(theta), radius * sin(theta), z);
}

float4 PSMain(VSOutput input) : SV_TARGET0
{
    const float resolution = g_Blend.blendParams.x;
    const float hysteresis = g_Blend.blendParams.y;
    const float cellSize = resolution + 2.0;

    // The viewport covers exactly one probe's cell, so the UV spans the cell including its border.
    const int2 cellTexel = int2(floor(input.uv * cellSize));
    const int2 interiorTexel = resolveInteriorTexel(cellTexel, int(resolution));

    // Texel centre, so a direction is the middle of what the texel represents rather than a corner.
    const float2 octUv = (float2(interiorTexel) + 0.5) / resolution;
    const float3 texelDirection = vkmProbeOctUvToDirection(octUv);

    float4 accumulated = float4(0.0, 0.0, 0.0, 0.0);
    float totalWeight = 0.0;
    for (uint i = 0; i < kSampleCount; ++i)
    {
        const float3 sampleDirection = fibonacciDirection(i);
        const float cosTerm = dot(texelDirection, sampleDirection);
        if (cosTerm <= 0.0)
        {
            continue; // the far hemisphere contributes nothing to this direction
        }
        const float4 captured = sampleCapture(sampleDirection);

#if defined(VKM_PROBE_BLEND_IRRADIANCE)
        accumulated.rgb += captured.rgb * cosTerm;
#else
        // Mean and mean-squared distance, weighted the same way, so the Chebyshev test and the
        // irradiance it gates agree about which directions matter.
        accumulated.r += captured.a * cosTerm;
        accumulated.g += captured.a * captured.a * cosTerm;
#endif
        totalWeight += cosTerm;
    }

    float4 result = float4(0.0, 0.0, 0.0, 1.0);
    if (totalWeight > 0.0)
    {
        result = accumulated / totalWeight;
        result.a = 1.0;
    }

    // Hysteresis: a probe converges over frames rather than jumping, which is what keeps a
    // round-robin update from flickering as probes are refreshed at different times.
    const float4 previous = g_Previous.SampleLevel(g_Sampler, input.uv, 0);
    return float4(lerp(result.rgb, previous.rgb, hysteresis), 1.0);
}
