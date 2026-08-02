// Copyright (c) 2025 Snowapril
//
// The low-spec tier's contact term: short-range indirect light, ray-marched in screen space and
// added on top of the probe volume's result.
//
// It exists because of what the probe grid cannot represent. Probes sit metres apart, so everything
// finer than that spacing -- the darkening where two walls meet, the bounce off a floor onto the
// object standing on it -- is averaged away before it is ever stored. This pass recovers exactly
// that near-field detail from what is already on screen, and nothing else: rays are short by
// design, and anything they miss is the probe volume's job.
//
// **Strictly additive, and strictly this tier.** Screen-space tracing is a poor ReSTIR citizen: the
// support of its target function is view-dependent and changes as the camera moves, which is the
// one assumption cross-domain MIS relies on. Combining the two through reservoirs produces
// brightening and darkening that shifts with camera motion, so this stays a separate additive term
// that the high tier will not inherit (restir.md section 5).
//
// Its honest limitations, none of which are bugs to fix here:
//   - offscreen and occluded geometry contribute nothing, because it can only sample what was
//     rasterized;
//   - a ray that leaves the screen is treated as "no contact" rather than as sky, which darkens
//     rather than brightens -- the conservative direction;
//   - thin geometry can be marched through, since a depth buffer records only the nearest surface.

#include "vkm_fullscreen.hlsli"
#include "vkm_frame_constants.hlsli"
#include "vkm_gbuffer.hlsli"
#include "vkm_random.hlsli"

VKM_FRAME_CONSTANTS(g_VkmFrame);

// Distinct from any other pass sampling on the same pixel in the same frame; see vkm_random.hlsli.
static const uint kSsgiPassId = 7u;

// Mirrors vkm::VkmSsgiConstants (renderer/gi_composite.h).
struct SsgiConstants
{
    // x = ray count, y = world-space ray length, z = steps per ray, w = intensity
    float4 params;
};

[[vk::binding(0, 2)]] Texture2D    g_Normal   : register(t0, space2);
// Motion in xy, metallic in z, camera distance in w -- the channel this pass marches against.
[[vk::binding(1, 2)]] Texture2D    g_Motion   : register(t1, space2);
// Direct lighting: what a hit surface is emitting towards us, and therefore what bounces.
[[vk::binding(2, 2)]] Texture2D    g_Direct   : register(t2, space2);
[[vk::binding(3, 2)]] SamplerState g_Sampler  : register(s0, space2);
[[vk::binding(4, 2)]] ConstantBuffer<SsgiConstants> g_Ssgi : register(b0, space2);

typedef VkmFullscreenVSOutput VSOutput;

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    return vkmFullscreenTriangle(vertexId);
}

// World position of whatever the G-buffer recorded at `uv`, or a miss.
bool sampleWorldPosition(float2 uv, out float3 worldPosition)
{
    worldPosition = float3(0.0, 0.0, 0.0);
    const float distanceFromCamera = g_Motion.SampleLevel(g_Sampler, uv, 0).w;
    if (distanceFromCamera <= 0.0)
    {
        return false; // never covered by geometry
    }
    worldPosition = vkmReconstructWorldPosition(uv, distanceFromCamera, g_VkmFrame.inverseViewProjection,
                                                g_VkmFrame.cameraPositionWorld.xyz);
    return true;
}

float4 PSMain(VSOutput input) : SV_TARGET0
{
    const float4 motionMetallic = g_Motion.SampleLevel(g_Sampler, input.uv, 0);
    if (motionMetallic.w <= 0.0)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    const float3 normal = vkmUnpackShadingNormal(g_Normal.SampleLevel(g_Sampler, input.uv, 0));
    const float3 origin = vkmReconstructWorldPosition(input.uv, motionMetallic.w,
                                                      g_VkmFrame.inverseViewProjection,
                                                      g_VkmFrame.cameraPositionWorld.xyz);

    const uint rayCount = uint(g_Ssgi.params.x);
    const float rayLength = g_Ssgi.params.y;
    const uint stepCount = uint(g_Ssgi.params.z);

    const uint2 pixel = uint2(input.uv * g_VkmFrame.viewportSize.xy);
    uint rngState = vkmSeed(pixel, g_VkmFrame.frameIndex.x, kSsgiPassId);

    float3 accumulated = float3(0.0, 0.0, 0.0);
    for (uint ray = 0; ray < rayCount; ++ray)
    {
        // Cosine-weighted, so the cos(theta) the irradiance integral wants cancels and the hits
        // can simply be averaged.
        const float3 direction = vkmCosineHemisphere(normal, vkmRandomFloat2(rngState));

        // The first step is jittered so the fixed step count does not band: without it every pixel
        // tests the same distances and the result reads as concentric rings.
        const float jitter = vkmRandomFloat(rngState);
        for (uint step = 0; step < stepCount; ++step)
        {
            const float t = rayLength * (float(step) + jitter) / float(stepCount);
            const float3 samplePoint = origin + direction * t;

            const float4 clip = mul(g_VkmFrame.viewProjection, float4(samplePoint, 1.0));
            if (clip.w <= 0.0)
            {
                break;
            }
            const float2 ndc = clip.xy / clip.w;
            if (any(abs(ndc) > 1.0))
            {
                break; // left the screen: no contact rather than sky, which only ever darkens
            }
            const float2 sampleUv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);

            float3 hitPosition;
            if (!sampleWorldPosition(sampleUv, hitPosition))
            {
                continue;
            }

            // A hit is the ray having passed *behind* the recorded surface. The thickness bound
            // stops a ray that passed far behind something from counting: the depth buffer holds
            // one surface, and without the bound every distant background pixel occludes.
            const float rayDepth = distance(g_VkmFrame.cameraPositionWorld.xyz, samplePoint);
            const float surfaceDepth = distance(g_VkmFrame.cameraPositionWorld.xyz, hitPosition);
            const float thickness = max(rayLength * 0.5, 0.05);
            if (rayDepth > surfaceDepth && rayDepth - surfaceDepth < thickness)
            {
                // Only surfaces facing back towards us bounce light this way; one facing away is
                // lit on its far side and has nothing to give.
                const float3 toOrigin = normalize(origin - hitPosition);
                const float3 hitNormal = vkmUnpackShadingNormal(g_Normal.SampleLevel(g_Sampler, sampleUv, 0));
                if (dot(hitNormal, toOrigin) > 0.0)
                {
                    accumulated += g_Direct.SampleLevel(g_Sampler, sampleUv, 0).rgb;
                }
                break; // the first surface along the ray is the one that occludes the rest
            }
        }
    }

    const float3 irradiance = rayCount > 0 ? accumulated / float(rayCount) : float3(0.0, 0.0, 0.0);
    // Alpha is the blend weight: this pass is added onto the probe result, never replaces it.
    return float4(irradiance * g_Ssgi.params.w, 1.0);
}
