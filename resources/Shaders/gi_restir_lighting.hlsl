// Copyright (c) 2025 Snowapril
//
// Phase 8.6: shade the screen from the resampled reservoirs into the indirect-radiance texture.
//
// This is the technique-interface end of ReSTIR: it writes the same target probe_lighting does,
// and honours the same contract gi_composite states -- the texture carries INCOMING irradiance,
// no albedo and no 1/pi, both applied once by the composite. The estimator is radiance * cos * W,
// the ReSTIR shading equation with the Lambertian f_s deliberately left for the composite.
//
// A graphics pass rather than compute because the target is a color attachment and the reservoir
// buffer reads fine from a fragment shader; the engine has no storage-texture table type, and
// does not need one for this.
//
// Debug views expose what the image cannot: reservoir confidence (M), sample age and the
// contribution weight W. The caps those fields enforce are otherwise invisible.

#include "vkm_frame_constants.hlsli"
#include "vkm_fullscreen.hlsli"
#include "vkm_gbuffer.hlsli"
#include "vkm_reservoir.hlsli"

// Mirrors vkm::VkmRestirLightingConstants.
struct RestirLightingConstants
{
    uint4  slices; // x = resampled slice, y = fresh slice, z = debug view, w = flags (reserved)
    float4 params; // x = MIS blend toward the fresh sample on smooth surfaces (8.7, 0 = off),
                   // y = reserved, z = 1 / (confidence cap + 1), w = 1 / max sample age
};

VKM_FRAME_CONSTANTS(g_VkmFrame);

[[vk::binding(0, 2)]] Texture2D g_Normal    : register(t0, space2);
[[vk::binding(1, 2)]] Texture2D g_BaseColor : register(t1, space2);
[[vk::binding(2, 2)]] Texture2D g_Motion    : register(t2, space2);
[[vk::binding(3, 2)]] RWStructuredBuffer<uint> g_Reservoirs : register(u0, space2);
[[vk::binding(4, 2)]] ConstantBuffer<RestirLightingConstants> g_Lighting : register(b0, space2);

VKM_RESERVOIR_DECLARE()

VkmFullscreenVSOutput VSMain(uint vertexId : SV_VertexID)
{
    return vkmFullscreenTriangle(vertexId);
}

// The pixel's estimate from one reservoir: incoming radiance times the receiver cosine times W.
float3 shadeFromReservoir(VkmReservoir reservoir, float3 position, float3 normal)
{
    if (reservoir.sampleCount == 0 || reservoir.weight <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }
    const float3 toSample = reservoir.samplePosition - position;
    const float distanceSquared = dot(toSample, toSample);
    if (distanceSquared <= 0.0)
    {
        return float3(0.0, 0.0, 0.0);
    }
    const float cosTheta = saturate(dot(normal, toSample * rsqrt(distanceSquared)));
    return reservoir.radiance * cosTheta * reservoir.weight;
}

float4 PSMain(VkmFullscreenVSOutput input) : SV_TARGET
{
    const uint2 pixel = uint2(input.position.xy);
    const uint width = (uint)g_VkmFrame.viewportSize.x;
    const uint height = (uint)g_VkmFrame.viewportSize.y;
    const uint pixelIndex = pixel.y * width + pixel.x;
    const uint pixelCount = width * height;

    const float cameraDistance = g_Motion.Load(int3(pixel, 0)).w;
    if (cameraDistance <= 0.0)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    const VkmReservoir reservoir =
        vkmLoadReservoir(vkmReservoirWordBase(g_Lighting.slices.x, pixelIndex, pixelCount));

    if (g_Lighting.slices.z != 0)
    {
        // The reservoir's bookkeeping as a grey ramp, normalized by the caps that bound it.
        float value = 0.0;
        if (g_Lighting.slices.z == 1)      { value = float(reservoir.sampleCount) * g_Lighting.params.z; }
        else if (g_Lighting.slices.z == 2) { value = float(reservoir.age) * g_Lighting.params.w; }
        else                               { value = saturate(reservoir.weight * 0.1); }
        return float4(value.xxx, 1.0);
    }

    const float3 position = vkmReconstructWorldPosition(input.uv, cameraDistance,
                                                        g_VkmFrame.inverseViewProjection,
                                                        g_VkmFrame.cameraPositionWorld.xyz);
    const float3 normal = vkmUnpackGeometricNormal(g_Normal.Load(int3(pixel, 0)));

    float3 irradiance = shadeFromReservoir(reservoir, position, normal);

    // 8.7: on smooth surfaces the resampled estimate is the biased one (cached radiance reused
    // across directions), so blend toward the pixel's own fresh sample by 1 - roughness^2.
    if (g_Lighting.params.x > 0.0)
    {
        const float roughness = g_BaseColor.Load(int3(pixel, 0)).a;
        const VkmReservoir fresh =
            vkmLoadReservoir(vkmReservoirWordBase(g_Lighting.slices.y, pixelIndex, pixelCount));
        const float3 freshIrradiance = shadeFromReservoir(fresh, position, normal);
        const float resampledShare = lerp(1.0, roughness * roughness, g_Lighting.params.x);
        irradiance = lerp(freshIrradiance, irradiance, resampledShare);
    }

    return float4(irradiance, 1.0);
}
