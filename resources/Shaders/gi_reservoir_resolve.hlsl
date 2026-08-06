// Copyright (c) 2025 Snowapril
//
// Phase 8.6 (the part 8.3 needs to be observable): shade a pixel from its reservoir.
//
// f_s * cos * L * W, which is the ReSTIR shading equation with the visibility already carried by
// the sample. No final visibility ray yet -- the sample was traced from this very pixel, so it is
// visible by construction; that ray becomes necessary once spatial reuse starts handing a pixel a
// neighbour's sample.
//
// The cosine and the direction are recomputed from the STORED sample position rather than carried
// alongside W. That is not an optimization, it is the invariant: the moment 8.4 hands this pixel a
// neighbour's reservoir, the direction and cosine are this pixel's, not the neighbour's, and a
// resolve that trusted a stored cosine would silently shade the wrong geometry.

#include "vkm_bindless.hlsli"
#include "vkm_frame_constants.hlsli"
#include "vkm_gbuffer.hlsli"
#include "vkm_reservoir.hlsli"

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
    uint  _pad0;
};

VKM_PUSH_CONSTANTS(RestirConstants, g_Restir);
VKM_FRAME_CONSTANTS(g_VkmFrame);

[[vk::binding(0, 2)]] Texture2D g_Normal    : register(t0, space2);
[[vk::binding(1, 2)]] Texture2D g_BaseColor : register(t1, space2);
[[vk::binding(2, 2)]] Texture2D g_Motion    : register(t2, space2);
[[vk::binding(3, 2)]] RWStructuredBuffer<uint> g_Reservoirs : register(u0, space2);
// rgb = summed radiance, a = samples in that sum: the layout VkmPathTracer and VkmIndirectPass
// accumulate into, so vkmComputeImageMse compares all three directly.
[[vk::binding(4, 2)]] RWStructuredBuffer<float4> g_Accumulation : register(u1, space2);

VKM_RESERVOIR_DECLARE()

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

    const float cameraDistance = g_Motion.Load(int3(pixel, 0)).w;
    if (cameraDistance <= 0.0)
    {
        // Left unaccumulated, exactly as the 1-spp pass leaves it: a sample count of zero means
        // "unknown", which is what keeps background out of a comparison.
        return;
    }

    const VkmReservoir reservoir =
        vkmLoadReservoir(vkmReservoirWordBase(g_Restir.inputSlice, pixelIndex, pixelCount));

    float3 radiance = float3(0.0, 0.0, 0.0);
    if (reservoir.sampleCount > 0 && reservoir.weight > 0.0)
    {
        const float2 uv = (float2(pixel) + 0.5) / float2(g_Restir.width, g_Restir.height);
        const float3 position = vkmReconstructWorldPosition(uv, cameraDistance,
                                                            g_VkmFrame.inverseViewProjection,
                                                            g_VkmFrame.cameraPositionWorld.xyz);
        const float3 geometricNormal = vkmUnpackGeometricNormal(g_Normal.Load(int3(pixel, 0)));
        const float3 albedo = g_BaseColor.Load(int3(pixel, 0)).rgb;

        const float3 toSample = reservoir.samplePosition - position;
        const float distanceSquared = dot(toSample, toSample);
        if (distanceSquared > 0.0)
        {
            const float3 direction = toSample * rsqrt(distanceSquared);
            const float cosTheta = saturate(dot(geometricNormal, direction));
            // Lambert: f_s = albedo / pi. The pi cancels against the pi in W, which is what makes
            // this equal albedo * L for a freshly generated reservoir -- the 1-spp estimator.
            const float3 brdf = albedo * (1.0 / 3.14159265358979);
            radiance = brdf * cosTheta * reservoir.radiance * reservoir.weight;
        }
    }

    const float4 previous = (g_Restir.sampleIndex == 0) ? float4(0.0, 0.0, 0.0, 0.0)
                                                        : g_Accumulation[pixelIndex];
    g_Accumulation[pixelIndex] = float4(previous.rgb + radiance, previous.a + 1.0);
}
