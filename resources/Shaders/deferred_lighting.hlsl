// Copyright (c) 2026 Snowapril
//
// Fullscreen deferred lighting: samples the G-buffer and shades it with every punctual light,
// plus every emissive triangle as an area light -- exact diffuse, approximate specular.
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
#include "vkm_area_lights.hlsli"
#include "vkm_punctual_lights.hlsli"
#include "vkm_shadow.hlsli"

VKM_FRAME_CONSTANTS(g_VkmFrame);

// Per-pass lighting parameters. Carried in set 2 rather than read from the scene's FrameData
// singleton so this pass needs no scene at all -- which is also why the light list is a
// fixed-size array: WGSL has no runtime-sized arrays.
//
// The directional light is an ordinary entry of type Directional, not a special case. Mirrors
// vkm::VkmDeferredLightConstants (renderer/deferred_lighting.h).
struct LightConstants
{
    // x = valid entries in `lights`, y = shadow tiles per atlas row, z = tile size in texels
    uint4 lightCount;
    // x = valid entries in `areaLights`; yzw unused.
    uint4 areaLightCount;
    VkmPunctualLight lights[VKM_MAX_PUNCTUAL_LIGHTS];
    VkmAreaLight areaLights[VKM_MAX_AREA_LIGHTS];
};


[[vk::binding(0, 2)]] Texture2D            g_Normal             : register(t0, space2);
[[vk::binding(1, 2)]] Texture2D            g_BaseColorRoughness : register(t1, space2);
[[vk::binding(2, 2)]] Texture2D            g_MotionMetallic     : register(t2, space2);
[[vk::binding(3, 2)]] SamplerState         g_Sampler            : register(s0, space2);
[[vk::binding(4, 2)]] ConstantBuffer<LightConstants> g_Light    : register(b0, space2);
// Appended past the constants rather than beside the other G-buffer channels, so existing
// binding tables only grow by one entry instead of renumbering.
[[vk::binding(5, 2)]] Texture2D            g_Emissive           : register(t3, space2);
// Appended past the existing bindings for the same reason binding 5 was: an existing table only
// grows an entry instead of renumbering every one it already had.
[[vk::binding(6, 2)]] Texture2D            g_ShadowAtlas        : register(t4, space2);
[[vk::binding(7, 2)]] ConstantBuffer<VkmShadowAtlasConstants> g_ShadowAtlasConstants : register(b1, space2);

VKM_SHADOW_LOADER(g_ShadowAtlas, g_Sampler, g_ShadowAtlasConstants);

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
    const float4 motionMetallic = g_MotionMetallic.SampleLevel(g_Sampler, input.uv, 0);
    const float cameraDistance = motionMetallic.w;
    // The G-buffer clears to zero, so a zero distance means no geometry covered this pixel.
    // Shading it would light the background as if it were a surface.
    if (cameraDistance <= 0.0)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    const float4 packedNormals = g_Normal.SampleLevel(g_Sampler, input.uv, 0);
    const float3 shadingNormal = vkmUnpackShadingNormal(packedNormals);
    // The shadow query is offset along the GEOMETRIC normal, not the shading one: a normal map
    // must not move where the surface actually is, or a bumpy wall self-shadows in its dents.
    const float3 geometricNormal = vkmUnpackGeometricNormal(packedNormals);

    const float4 baseColorRoughness = g_BaseColorRoughness.SampleLevel(g_Sampler, input.uv, 0);
    const float3 baseColor = baseColorRoughness.rgb;
    // Clamped away from zero: a perfectly smooth microfacet lobe is a delta that this analytic
    // form cannot represent, and it produces fireflies rather than a highlight.
    const float roughness = clamp(baseColorRoughness.a, 0.045, 1.0);
    const float metallic = saturate(motionMetallic.b);

    const float3 worldPosition = vkmReconstructWorldPosition(
        input.uv, cameraDistance, g_VkmFrame.inverseViewProjection, g_VkmFrame.cameraPositionWorld.xyz);

    const float3 viewDirection = normalize(g_VkmFrame.cameraPositionWorld.xyz - worldPosition);
    const float nDotV = abs(dot(shadingNormal, viewDirection)) + 1e-5;

    // Metals have no diffuse response and tint their specular with the base colour; dielectrics
    // reflect an achromatic ~4% at normal incidence.
    const float3 diffuseColor = baseColor * (1.0 - metallic);
    const float3 f0 = lerp(float3(0.04, 0.04, 0.04), baseColor, metallic);

    float3 shaded = float3(0.0, 0.0, 0.0);
    for (uint lightIndex = 0; lightIndex < g_Light.lightCount.x; ++lightIndex)
    {
        const VkmPunctualSample lightSample =
            vkmSamplePunctualLight(g_Light.lights[lightIndex], worldPosition);

        const float nDotL = saturate(dot(shadingNormal, lightSample.direction));
        // Both cheap rejections in one test: a surface facing away, and a light whose distance
        // or cone attenuation already took it to zero.
        if (nDotL <= 0.0 || all(lightSample.radiance <= 0.0))
        {
            continue;
        }

        const float shadow = vkmShadowFactor(g_Light.lights[lightIndex], worldPosition,
                                             geometricNormal, nDotL,
                                             g_Light.lightCount.y, g_Light.lightCount.z);
        if (shadow <= 0.0)
        {
            continue;
        }

        const float3 halfVector = normalize(viewDirection + lightSample.direction);
        const float nDotH = saturate(dot(shadingNormal, halfVector));
        const float vDotH = saturate(dot(viewDirection, halfVector));

        const float3 fresnel = fresnelSchlick(vDotH, f0);
        const float3 specular = distributionGGX(nDotH, roughness) *
                                visibilitySmithGGX(nDotV, nDotL, roughness) * fresnel;
        // Energy that was not reflected specularly is what remains for the diffuse lobe.
        const float3 diffuse = (1.0 - fresnel) * diffuseColor / 3.14159265;

        shaded += (diffuse + specular) * lightSample.radiance * nDotL * shadow;
    }

    /*
    * Emissive triangles. The traced tier reaches these through NEE against the light table; the
    * raster tier cannot read that table at all, so the same polygons arrive here in the uniform
    * buffer and are shaded without sampling anything.
    *
    * The two lobes are not equally trustworthy, and the difference is deliberate:
    *
    *   diffuse   exact. The polygon's projected solid angle in closed form, which is the identity
    *             case of linearly transformed cosines.
    *   specular  approximate. Karis's representative point -- the emitter stands in as the single
    *             point its mirror direction reaches, with the lobe widened by the angle it
    *             subtends. LTC would warp the polygon by a fitted M^-1 instead; those tables are
    *             data this tree does not have.
    *
    * The form factor carries the integral of the incoming cosine over the emitter, so it takes
    * the place a punctual light's nDotL would have: radiance * BRDF * formFactor is the same
    * shape as radiance * BRDF * nDotL, with the polygon's extent folded into the one term.
    *
    * No shadow term on either: a polygon integral measures how much hemisphere the emitter covers,
    * not what stands between. An occluded area light still lights this surface here, which is
    * exactly the discrepancy against the traced tier that TODO.md records.
    */
    const float3 reflectionDir = reflect(-viewDirection, shadingNormal);
    const float alpha = roughness * roughness;
    for (uint areaIndex = 0; areaIndex < g_Light.areaLightCount.x; ++areaIndex)
    {
        const VkmAreaLight areaLight = g_Light.areaLights[areaIndex];
        const float formFactor =
            vkmAreaLightFormFactor(areaLight, worldPosition, shadingNormal);
        if (formFactor <= 0.0)
        {
            continue;
        }
        shaded += areaLight.radiance.rgb * diffuseColor * (1.0 / 3.14159265) * formFactor;

        const float3 representative =
            vkmAreaLightRepresentativePoint(areaLight, worldPosition, reflectionDir);
        const float3 toRepresentative = representative - worldPosition;
        const float representativeDistance = length(toRepresentative);
        if (representativeDistance <= 1e-6)
        {
            continue;
        }
        const float3 areaLightDir = toRepresentative / representativeDistance;
        const float areaNDotL = saturate(dot(shadingNormal, areaLightDir));
        if (areaNDotL <= 0.0)
        {
            continue;
        }

        const float2 widening =
            vkmAreaLightSpecularWidening(areaLight, representativeDistance, alpha);
        // The widened alpha is fed back as a roughness because the GGX helpers here square what
        // they are given; passing alpha directly would square it twice.
        const float widenedRoughness = sqrt(widening.x);

        const float3 areaHalf = normalize(viewDirection + areaLightDir);
        const float areaNDotH = saturate(dot(shadingNormal, areaHalf));
        const float areaVDotH = saturate(dot(viewDirection, areaHalf));
        const float3 areaFresnel = fresnelSchlick(areaVDotH, f0);
        const float3 areaSpecular = distributionGGX(areaNDotH, widenedRoughness) *
                                    visibilitySmithGGX(nDotV, areaNDotL, widenedRoughness) *
                                    areaFresnel * widening.y;
        shaded += areaLight.radiance.rgb * areaSpecular * formFactor;
    }

    // Emission is what the surface adds on its own, on top of what the lights reflect off it --
    // the term that makes a camera-visible emitter glow instead of rendering black.
    const float3 emissive = g_Emissive.SampleLevel(g_Sampler, input.uv, 0).rgb;
    return float4(shaded + emissive, 1.0);
}
