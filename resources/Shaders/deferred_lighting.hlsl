// Copyright (c) 2025 Snowapril
//
// Fullscreen deferred lighting: samples the G-buffer and shades it with one directional light.
//
// This is the first pass in the engine that reads the G-buffer, and it is deliberately built the
// way the GI passes will be:
//
//   - Its inputs arrive through descriptor set 2 (per-pass resources), not the bindless arrays.
//     That is what lets it run on WebGPU at all -- the bindless texture path needs runtime-sized
//     arrays, which WGSL does not have -- and it keeps the pass independent of VkmScene, which a
//     fullscreen pass has no business depending on.
//   - The G-buffer is unpacked through vkm_gbuffer.hlsli rather than by restating the octahedral
//     maths here, so writer and reader cannot drift.
//
// The fullscreen triangle comes from vkm_fullscreen.hlsli, which is why the PSO declares no input
// layout.

#include "vkm_frame_constants.hlsli"
#include "vkm_fullscreen.hlsli"
#include "vkm_gbuffer.hlsli"

VKM_FRAME_CONSTANTS(g_VkmFrame);

// Per-pass lighting parameters. Carried in set 2 rather than read from the scene's FrameData
// singleton so this pass needs no scene at all.
struct LightConstants
{
    float4 directionToLight; // xyz = normalized direction TOWARDS the light, w unused
    float4 radiance;         // rgb = light colour * intensity, w unused
};

[[vk::binding(0, 2)]] Texture2D            g_Normal             : register(t0, space2);
[[vk::binding(1, 2)]] Texture2D            g_BaseColorRoughness : register(t1, space2);
[[vk::binding(2, 2)]] Texture2D            g_MotionMetallic     : register(t2, space2);
[[vk::binding(3, 2)]] Texture2D            g_Depth              : register(t3, space2);
[[vk::binding(4, 2)]] SamplerState         g_Sampler            : register(s0, space2);
[[vk::binding(5, 2)]] ConstantBuffer<LightConstants> g_Light    : register(b0, space2);

typedef VkmFullscreenVSOutput VSOutput;

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    return vkmFullscreenTriangle(vertexId);
}

// Trowbridge-Reitz (GGX) normal distribution.
float distributionGGX(float nDotH, float roughness)
{
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float denom = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / max(3.14159265 * denom * denom, 1e-7);
}

// Smith height-correlated visibility term, already divided by the 4*nDotL*nDotV of the
// microfacet denominator -- so the specular term below multiplies D * Vis * F directly.
float visibilitySmithGGX(float nDotV, float nDotL, float roughness)
{
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float lambdaV = nDotL * sqrt(nDotV * nDotV * (1.0 - a2) + a2);
    const float lambdaL = nDotV * sqrt(nDotL * nDotL * (1.0 - a2) + a2);
    return 0.5 / max(lambdaV + lambdaL, 1e-7);
}

float3 fresnelSchlick(float vDotH, float3 f0)
{
    return f0 + (1.0 - f0) * pow(saturate(1.0 - vDotH), 5.0);
}

float4 PSMain(VSOutput input) : SV_TARGET0
{
    const float depth = g_Depth.Sample(g_Sampler, input.uv).r;
    // The G-buffer clears depth to 1.0 (far), so anything still at the far plane was never
    // covered by geometry. Shading it would light the background as if it were a surface.
    if (depth >= 1.0)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    const float4 packedNormals = g_Normal.Sample(g_Sampler, input.uv);
    const float3 shadingNormal = vkmUnpackShadingNormal(packedNormals);

    const float4 baseColorRoughness = g_BaseColorRoughness.Sample(g_Sampler, input.uv);
    const float3 baseColor = baseColorRoughness.rgb;
    // Clamped away from zero: a perfectly smooth microfacet lobe is a delta that this analytic
    // form cannot represent, and it produces fireflies rather than a highlight.
    const float roughness = clamp(baseColorRoughness.a, 0.045, 1.0);
    const float metallic = saturate(g_MotionMetallic.Sample(g_Sampler, input.uv).b);

    // Reconstruct the world position from depth: UV to NDC, then through the inverse
    // view-projection. The engine's clip space is +Y up with a [0,1] depth range.
    const float2 ndc = float2(input.uv.x * 2.0 - 1.0, 1.0 - input.uv.y * 2.0);
    const float4 clipPosition = float4(ndc, depth, 1.0);
    const float4 worldPositionH = mul(g_VkmFrame.inverseViewProjection, clipPosition);
    const float3 worldPosition = worldPositionH.xyz / worldPositionH.w;

    const float3 viewDirection = normalize(g_VkmFrame.cameraPositionWorld.xyz - worldPosition);
    const float3 lightDirection = normalize(g_Light.directionToLight.xyz);
    const float3 halfVector = normalize(viewDirection + lightDirection);

    const float nDotL = saturate(dot(shadingNormal, lightDirection));
    const float nDotV = abs(dot(shadingNormal, viewDirection)) + 1e-5;
    const float nDotH = saturate(dot(shadingNormal, halfVector));
    const float vDotH = saturate(dot(viewDirection, halfVector));

    // Metals have no diffuse response and tint their specular with the base colour; dielectrics
    // reflect an achromatic ~4% at normal incidence.
    const float3 diffuseColor = baseColor * (1.0 - metallic);
    const float3 f0 = lerp(float3(0.04, 0.04, 0.04), baseColor, metallic);

    const float3 fresnel = fresnelSchlick(vDotH, f0);
    const float3 specular = distributionGGX(nDotH, roughness) *
                            visibilitySmithGGX(nDotV, nDotL, roughness) * fresnel;
    // Energy that was not reflected specularly is what remains for the diffuse lobe.
    const float3 diffuse = (1.0 - fresnel) * diffuseColor / 3.14159265;

    const float3 radiance = g_Light.radiance.rgb * nDotL;
    return float4((diffuse + specular) * radiance, 1.0);
}
