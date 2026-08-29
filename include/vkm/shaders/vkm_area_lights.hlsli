// Copyright (c) 2026 Snowapril
//
// Analytic area lights for the raster tier: the diffuse response of a Lambertian surface to an
// emissive polygon, in closed form.
//
// This is the identity case of linearly transformed cosines (Heitz et al. 2016). LTC's whole
// construction is "warp the polygon by M^-1, then integrate a clamped cosine over it"; for the
// diffuse lobe M is the identity, so the warp disappears and what remains is the integral itself.
// That is why this header carries no lookup table: the tables encode M^-1 for GGX, and the
// specular lobe is the only thing that needs them.
//
// The integral is exact, not an approximation, which is what makes it gateable against a closed
// form rather than against another estimator: a Lambertian point under an unoccluded polygon
// receives radiance * (albedo/pi) * the polygon's PROJECTED SOLID ANGLE, and that is what
// vkmAreaLightFormFactor returns.
//
// Occlusion is not part of it. A polygon integral says how much of the hemisphere the emitter
// covers, not what stands in the way -- shadowing an area light needs a separate visibility term
// (the traced tier's shadow rays, or a soft-shadow technique the raster tier does not have).
// TODO.md records that.

#ifndef VKM_AREA_LIGHTS_HLSLI
#define VKM_AREA_LIGHTS_HLSLI

// Matches vkm::kVkmMaxAreaLights in renderer/deferred_lighting.h.
#define VKM_MAX_AREA_LIGHTS 32

// Mirrors vkm::VkmAreaLight (renderer/scene/light_table.h), byte for byte. Corners padded to
// float4 because a float3 array in a uniform buffer carries a stride rule the C++ type hides.
struct VkmAreaLight
{
    float4 p0;
    float4 p1;
    float4 p2;
    float4 radiance;
};

/*
* @brief The integral of one clipped edge, as an angle times the edge plane's tilt.
* @details Lambert's formula: the projected solid angle of a spherical polygon is half the sum
* over its edges of the subtended angle times the z-component of the edge plane's normal. Both
* factors matter -- the angle alone would measure solid angle, not PROJECTED solid angle, and the
* cosine weighting is the whole difference between "how big the emitter looks" and "how much light
* it delivers".
*/
float vkmAreaLightEdge(float3 a, float3 b)
{
    const float cosTheta = clamp(dot(a, b), -1.0, 1.0);
    // The cross product's own length is sin(theta), so normalizing it would divide by a quantity
    // that vanishes exactly when the angle does. Dividing the z-component by the length once,
    // guarded, is the same value and degrades to zero rather than to a NaN.
    const float3 cross_ab = cross(a, b);
    const float sinTheta = length(cross_ab);
    if (sinTheta < 1e-7)
    {
        return 0.0;
    }
    return acos(cosTheta) * (cross_ab.z / sinTheta);
}

/*
* @brief Projected solid angle of an emissive triangle at a shading point.
* @details The triangle is moved into the shading frame (surface at the origin, normal on +z) and
* clipped to the upper hemisphere before the edge sum, because an edge that dips below the horizon
* contributes to Lambert's formula with the wrong sign -- an unclipped sum on a polygon that
* straddles the surface plane can even go negative. Clipping a triangle against one plane yields
* three or four vertices, which is why the loop below runs over a four-entry buffer.
*
* Two-sided, matching the traced tier: vkmShadeSecondaryHit takes abs() on the light-side cosine
* because the path loop flips normals, so an emitter lights both of its faces. Taking the
* magnitude here keeps the two tiers integrating the same emitter.
*
* @param light The emitter.
* @param worldPosition Shading point.
* @param normal Shading normal, normalized.
* @return Projected solid angle in [0, pi]; multiply by radiance * albedo/pi for outgoing radiance.
*/
float vkmAreaLightFormFactor(VkmAreaLight light, float3 worldPosition, float3 normal)
{
    // An orthonormal shading frame. The tangent is any vector off the normal; the form factor is
    // rotationally symmetric about it, so which one is arbitrary.
    const float3 up = abs(normal.z) < 0.999 ? float3(0.0, 0.0, 1.0) : float3(1.0, 0.0, 0.0);
    const float3 tangent = normalize(cross(up, normal));
    const float3 bitangent = cross(normal, tangent);

    float3 polygon[4];
    const float3 corners[3] = { light.p0.xyz, light.p1.xyz, light.p2.xyz };
    for (uint corner = 0; corner < 3; ++corner)
    {
        const float3 relative = corners[corner] - worldPosition;
        polygon[corner] = float3(dot(relative, tangent), dot(relative, bitangent), dot(relative, normal));
    }
    polygon[3] = polygon[2];

    // Clip against z >= 0, walking the three edges and emitting the crossing points. Written as a
    // straight-line Sutherland-Hodgman step rather than a loop with a dynamic output index, which
    // dxc unrolls badly and WGSL cannot express with a runtime-indexed local array.
    float3 clipped[4];
    uint clippedCount = 0;
    for (uint edge = 0; edge < 3; ++edge)
    {
        const float3 current = polygon[edge];
        const float3 next = polygon[(edge + 1) % 3];
        const bool currentAbove = current.z >= 0.0;
        const bool nextAbove = next.z >= 0.0;
        if (currentAbove)
        {
            clipped[clippedCount] = current;
            clippedCount = min(clippedCount + 1, 3u);
        }
        if (currentAbove != nextAbove)
        {
            const float t = current.z / (current.z - next.z);
            clipped[clippedCount] = lerp(current, next, t);
            clippedCount = min(clippedCount + 1, 3u);
        }
    }
    if (clippedCount < 3)
    {
        return 0.0; // entirely below the horizon
    }

    float sum = 0.0;
    for (uint i = 0; i < clippedCount; ++i)
    {
        const float3 a = normalize(clipped[i]);
        const float3 b = normalize(clipped[(i + 1) % clippedCount]);
        sum += vkmAreaLightEdge(a, b);
    }
    return abs(sum) * 0.5;
}

#endif // VKM_AREA_LIGHTS_HLSLI
