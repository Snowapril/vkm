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
// Diffuse only. There is no closed form for a GGX lobe over a polygon; LTC gets one by warping the
// polygon through a fitted M^-1, and those tables are data this tree does not have. Karis's
// representative point stood in for them briefly and is gone again: it measured 38.7% error against
// exact integration outside its small-source regime, and the branchy closest-point-on-triangle it
// needs crashed lavapipe's shader compiler. TODO.md carries both findings.
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
* @brief Where a segment crossing the horizon meets it.
* @details Only called for a pair straddling z = 0, so the denominator cannot vanish: one endpoint
* is at or above the plane and the other strictly below it.
*/
float3 vkmAreaLightHorizonCross(float3 above, float3 below)
{
    return lerp(above, below, above.z / (above.z - below.z));
}

/*
* @brief Projected solid angle of an emissive triangle at a shading point.
* @details The triangle is moved into the shading frame (surface at the origin, normal on +z) and
* clipped to the upper hemisphere before the edge sum, because an edge that dips below the horizon
* contributes to Lambert's formula with the wrong sign -- an unclipped sum on a polygon that
* straddles the surface plane can even go negative.
*
* The clip is a switch over the eight ways three vertices can fall either side of the plane,
* rather than a loop appending to an array. That is not a style preference: a loop writes at a
* runtime index, which forces the polygon into indexable memory, and that shape crashed lavapipe's
* shader compiler outright (four Vulkan CI jobs, SIGSEGV, while Metal and MoltenVK ran it fine).
* Every index here is a literal.
*
* Clipping a triangle against one plane yields three or four vertices -- a corner below the plane
* is replaced by two crossings. The four-vertex cases are why `d` exists; the three-vertex cases
* set it equal to `a`, which makes the last edge a zero-length one that contributes nothing, so
* the sum below needs no branch.
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

    const float3 r0 = light.p0.xyz - worldPosition;
    const float3 r1 = light.p1.xyz - worldPosition;
    const float3 r2 = light.p2.xyz - worldPosition;
    const float3 p0 = float3(dot(r0, tangent), dot(r0, bitangent), dot(r0, normal));
    const float3 p1 = float3(dot(r1, tangent), dot(r1, bitangent), dot(r1, normal));
    const float3 p2 = float3(dot(r2, tangent), dot(r2, bitangent), dot(r2, normal));

    const uint config = (p0.z >= 0.0 ? 1u : 0u) | (p1.z >= 0.0 ? 2u : 0u) | (p2.z >= 0.0 ? 4u : 0u);
    if (config == 0u)
    {
        return 0.0; // entirely below the horizon
    }

    float3 a = p0;
    float3 b = p1;
    float3 c = p2;
    float3 d = p0;
    if (config == 1u)        // p0 only
    {
        a = p0; b = vkmAreaLightHorizonCross(p0, p1); c = vkmAreaLightHorizonCross(p0, p2); d = a;
    }
    else if (config == 2u)   // p1 only
    {
        a = vkmAreaLightHorizonCross(p1, p0); b = p1; c = vkmAreaLightHorizonCross(p1, p2); d = a;
    }
    else if (config == 3u)   // p0, p1
    {
        a = p0; b = p1; c = vkmAreaLightHorizonCross(p1, p2); d = vkmAreaLightHorizonCross(p0, p2);
    }
    else if (config == 4u)   // p2 only
    {
        a = vkmAreaLightHorizonCross(p2, p0); b = vkmAreaLightHorizonCross(p2, p1); c = p2; d = a;
    }
    else if (config == 5u)   // p0, p2
    {
        a = p0; b = vkmAreaLightHorizonCross(p0, p1); c = vkmAreaLightHorizonCross(p2, p1); d = p2;
    }
    else if (config == 6u)   // p1, p2
    {
        a = vkmAreaLightHorizonCross(p1, p0); b = p1; c = p2; d = vkmAreaLightHorizonCross(p2, p0);
    }
    // config == 7: the whole triangle is above, and a/b/c already hold it with d == a.

    const float3 na = normalize(a);
    const float3 nb = normalize(b);
    const float3 nc = normalize(c);
    const float3 nd = normalize(d);
    // Four edges always. Where the clip produced three vertices d == a, so the last two terms are
    // edge(c, a) and edge(a, a) -- and a zero-length edge subtends no angle, so it adds nothing.
    const float sum = vkmAreaLightEdge(na, nb) + vkmAreaLightEdge(nb, nc) +
                      vkmAreaLightEdge(nc, nd) + vkmAreaLightEdge(nd, na);
    return abs(sum) * 0.5;
}

#endif // VKM_AREA_LIGHTS_HLSLI
