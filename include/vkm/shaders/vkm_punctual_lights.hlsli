// Copyright (c) 2026 Snowapril
//
// Shader-side mirror of the engine's punctual lights, plus glTF's attenuation forms.
//
// Lives beside vkm_lights.hlsli rather than inside it because the two answer different questions:
// vkm_lights.hlsli samples an emissive-triangle table by power for the traced tier's NEE, while
// this header evaluates a small fixed list of delta lights analytically. A punctual light has no
// area, so there is nothing to importance-sample -- the contribution is closed form.
//
// Consumed through descriptor set 2, never the bindless arrays: deferred_lighting.hlsl runs on
// WebGPU and must not depend on VkmScene.

#ifndef VKM_PUNCTUAL_LIGHTS_HLSLI
#define VKM_PUNCTUAL_LIGHTS_HLSLI

#define VKM_LIGHT_TYPE_POINT       0
#define VKM_LIGHT_TYPE_SPOT        1
#define VKM_LIGHT_TYPE_DIRECTIONAL 2

// The deferred pass shades at most this many lights. A fixed cap because the list rides a uniform
// buffer, which needs a compile-time size; raising it costs 64 bytes per entry.
#define VKM_MAX_PUNCTUAL_LIGHTS 16

// Mirrors vkm::VkmPunctualLight (renderer/scene/light_table.h), byte for byte.
struct VkmPunctualLight
{
    float3 positionWorld;
    float  range;           // 0 = unlimited
    float3 directionWorld;  // the direction the light points, glTF's local -Z
    float  cosOuter;        // -1 on a non-spot: every direction is inside the cone
    float3 radiance;        // colour * intensity
    float  cosInner;
    uint   type;
    int    shadowTile;      // first atlas tile, or -1 when the light casts no shadow
    uint   shadowTileCount; // 6 for a point light's faces, one per cascade for a directional
    float  pad;
};

/*
* @brief What a shading point needs to know about one punctual light: where it is and how
* bright. Named apart from vkm_lights.hlsli's VkmLightSample because a traced shader includes
* both -- that one is a sampled point on an emissive triangle, this one a delta light resolved.
* @details `direction` points FROM the surface TOWARDS the light, which is the convention every
* BRDF term here already uses. `distance` is 1e30 for a directional light so a caller can use it
* as a shadow ray's TMax without branching.
*/
struct VkmPunctualSample
{
    float3 direction;
    float  distance;
    float3 radiance;   // already attenuated by distance and cone
};

/*
* @brief glTF's range-windowed inverse-square falloff.
* @details The spec's exact form. The window is what makes a bounded light bounded: without it a
* light with a range still lit the whole scene faintly, and with a naive hard cutoff it would edge.
* Returns 1/d^2 unwindowed when range is 0, which is what glTF says an absent range means.
*/
float vkmPunctualDistanceAttenuation(float distanceToLight, float range)
{
    const float inverseSquare = 1.0 / max(distanceToLight * distanceToLight, 1e-8);
    if (range <= 0.0)
    {
        return inverseSquare;
    }
    const float ratio = distanceToLight / range;
    const float ratio4 = ratio * ratio * ratio * ratio;
    const float window = saturate(1.0 - ratio4);
    return window * window * inverseSquare;
}

/*
* @brief glTF's smooth spot cone falloff, 1 inside the inner angle and 0 outside the outer one.
* @param cosAngle Cosine of the angle between the light's aim and the direction to the surface.
*/
float vkmPunctualSpotAttenuation(float cosAngle, float cosInner, float cosOuter)
{
    // Equal angles are a hard-edged cone rather than a division by zero.
    const float denominator = cosInner - cosOuter;
    if (denominator <= 1e-6)
    {
        return cosAngle > cosOuter ? 1.0 : 0.0;
    }
    return saturate((cosAngle - cosOuter) / denominator);
}

/*
* @brief Resolves one light at a shading point: direction, distance and attenuated radiance.
* @details Returns radiance 0 for a surface outside a spot's cone or past a light's range, so a
* caller can shade unconditionally and let the zero fall out.
*/
VkmPunctualSample vkmSamplePunctualLight(VkmPunctualLight light, float3 worldPosition)
{
    VkmPunctualSample result;

    if (light.type == VKM_LIGHT_TYPE_DIRECTIONAL)
    {
        result.direction = -normalize(light.directionWorld);
        result.distance = 1.0e30;
        result.radiance = light.radiance;
        return result;
    }

    const float3 toLight = light.positionWorld - worldPosition;
    const float distanceSquared = dot(toLight, toLight);
    const float distanceToLight = sqrt(max(distanceSquared, 1e-16));

    result.direction = toLight / distanceToLight;
    result.distance = distanceToLight;
    result.radiance = light.radiance * vkmPunctualDistanceAttenuation(distanceToLight, light.range);

    if (light.type == VKM_LIGHT_TYPE_SPOT)
    {
        // The cone opens along the light's aim, and `direction` points back at the light, hence
        // the negation.
        const float cosAngle = dot(normalize(light.directionWorld), -result.direction);
        result.radiance *= vkmPunctualSpotAttenuation(cosAngle, light.cosInner, light.cosOuter);
    }
    return result;
}

#endif // VKM_PUNCTUAL_LIGHTS_HLSLI
