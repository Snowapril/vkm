// Copyright (c) 2026 Snowapril
//
// Turns one probe's cube capture into atlas contents: integrates the capture into an octahedral
// map and blends it into the previous frame's values.
//
// Two permutations from one source, because the two atlases integrate different quantities at
// different resolutions but are otherwise the same pass:
//   VKM_PROBE_BLEND_IRRADIANCE  cosine-weighted radiance, per direction
//   VKM_PROBE_BLEND_DISTANCE    mean and mean-squared distance, for the Chebyshev test
//
// Three details are what make this correct rather than approximately correct:
//
//   - Border texels are not copied from the interior afterwards. A border texel is mapped to the
//     interior texel it mirrors, and *that* texel's direction is integrated. Same result as a copy
//     pass, but in one pass, and without reading the target being written.
//   - The capture is sampled by projecting a direction through the same face view-projections the
//     capture was rendered with, rather than through a hand-derived cube-face convention. It costs
//     six matrix multiplies per sample and is correct by construction: there is no second
//     convention that can disagree with the first.
//   - Hysteresis is the *blend hardware*, not a fetch of the previous atlas: this pass outputs
//     alpha = 1 - hysteresis and the PSO blends SrcAlpha/OneMinusSrcAlpha, giving exactly
//     lerp(new, previous, hysteresis) against the atlas itself. That is what lets probes be
//     refreshed a few per frame -- a cell nobody draws over keeps its value, with no second copy
//     of the atlas to swap and nothing to go stale.

#include "vkm_bindless.hlsli"
#include "vkm_fullscreen.hlsli"
#include "vkm_probe_volume.hlsli"

#if !defined(VKM_PROBE_BLEND_IRRADIANCE) && !defined(VKM_PROBE_BLEND_DISTANCE)
    #error "probe_blend.hlsl requires VKM_PROBE_BLEND_IRRADIANCE or VKM_PROBE_BLEND_DISTANCE"
#endif

// Mirrors vkm::VkmProbeBlendConstants (renderer/probe_volume.h). Built for a probe at the origin
// and shared by every probe: this pass evaluates the matrices at `probePosition + direction`, so
// the probe's position cancels outright.
struct ProbeBlendConstants
{
    float4x4 faceViewProjection[6];
    float4   blendParams;       // x = octahedral resolution, y unused (hysteresis is pushed),
                                // z = capture faces per row, w = capture face size in texels
    float4   captureAtlasSize;  // xy = capture atlas extent in texels
};

// Mirrors vkm::VkmProbeBlendPushConstants. Pushed rather than bound because one render pass blends
// several probes, one viewport each, and rebuilding a resource table per probe is not possible --
// per-pass tables are immutable.
struct ProbePushConstants
{
    uint  captureTileBase; // index of this probe's first face tile in the shared capture atlas
    float hysteresis;      // per probe: a probe's first-ever update passes 0, see the updater
};

VKM_PUSH_CONSTANTS(ProbePushConstants, g_Probe);

[[vk::binding(0, 2)]] Texture2D    g_Capture : register(t0, space2);
[[vk::binding(1, 2)]] SamplerState g_Sampler : register(s0, space2);
[[vk::binding(2, 2)]] ConstantBuffer<ProbeBlendConstants> g_Blend : register(b0, space2);

// The push constants have to reach the pixel shader through the vertex shader: Vulkan declares the
// push-constant range for the vertex and compute stages only, so a fragment shader cannot read it.
// Spelled out rather than nesting VkmFullscreenVSOutput, so the signature stays a flat list of
// semantics all the way through dxc and SPIRV-Cross.
struct VSOutput
{
    float4 position : SV_POSITION;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
    [[vk::location(1)]] nointerpolation uint  captureTileBase : TEXCOORD1;
    [[vk::location(2)]] nointerpolation float hysteresis : TEXCOORD2;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    const VkmFullscreenVSOutput fullscreen = vkmFullscreenTriangle(vertexId);

    VSOutput output;
    output.position = fullscreen.position;
    output.uv = fullscreen.uv;
    output.captureTileBase = g_Probe.captureTileBase;
    output.hysteresis = g_Probe.hysteresis;
    return output;
}

/*
* @brief Samples the cube capture along `direction`, returning rgb = radiance, a = distance.
*
* Finds the face by asking each face's own view-projection whether the direction lands inside it,
* which is what keeps this consistent with how the capture was rendered. The matrices are built for
* a probe at the origin, so a direction *is* the probe-relative sample point and no probe position
* is needed here.
*
* `tileBase` is where this probe's six face tiles start in the capture atlas, which is shared by
* every probe the frame is refreshing.
*/
float4 sampleCapture(float3 direction, uint tileBase)
{
    const float facesPerRow = g_Blend.blendParams.z;
    const float faceSize = g_Blend.blendParams.w;
    const float2 atlasSize = g_Blend.captureAtlasSize.xy;

    for (uint face = 0; face < 6; ++face)
    {
        const float4 clip = mul(g_Blend.faceViewProjection[face], float4(direction, 1.0));
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
        const float tile = float(tileBase + face);
        const float2 tileOrigin = float2(fmod(tile, facesPerRow), floor(tile / facesPerRow));
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
        const float4 captured = sampleCapture(sampleDirection, input.captureTileBase);

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

    float3 result = float3(0.0, 0.0, 0.0);
    if (totalWeight > 0.0)
    {
        result = accumulated.rgb / totalWeight;
    }

    // Hysteresis: a probe converges over frames rather than jumping, which is what keeps a
    // round-robin update from flickering as probes are refreshed at different times. The blend
    // state does the interpolation -- alpha here is the weight of the *new* value, so the atlas
    // ends up holding lerp(new, previous, hysteresis). Alpha is not otherwise stored: neither
    // atlas reads it back.
    return float4(result, 1.0 - input.hysteresis);
}
