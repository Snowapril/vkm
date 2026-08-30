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
// The specular half is NOT exact, and the difference is worth stating plainly. There is no closed
// form for a GGX lobe over a polygon; LTC gets one by warping the polygon through a fitted M^-1,
// and those fitted tables are data this tree does not have. What is here instead is Karis's
// representative point: replace the emitter by the single point on it the mirror direction reaches,
// evaluate ordinary punctual GGX there, and widen the lobe by the angle the emitter subtends. So
// the diffuse term is the truth and the specular term is an approximation whose error is measured
// rather than assumed -- see the area-light gates.
//
// Occlusion is not part of either. A polygon integral says how much of the hemisphere the emitter
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

    float3 polygon[3];
    const float3 corners[3] = { light.p0.xyz, light.p1.xyz, light.p2.xyz };
    for (uint corner = 0; corner < 3; ++corner)
    {
        const float3 relative = corners[corner] - worldPosition;
        polygon[corner] = float3(dot(relative, tangent), dot(relative, bitangent), dot(relative, normal));
    }

    /*
    * Clip against z >= 0, one Sutherland-Hodgman pass over the three edges: keep a vertex that is
    * above, and emit the crossing point wherever an edge changes side.
    *
    * The output holds FOUR vertices, not three. A triangle with exactly one corner below the plane
    * loses that corner and gains two crossings, so the result is a quad -- capping the count at
    * three silently drops one of its vertices, which is a wrong form factor rather than a visible
    * failure. The horizon-clip gate exists because that is invisible in any scene whose emitters
    * float clear of their receivers.
    */
    float3 clipped[4];
    uint clippedCount = 0;
    for (uint edge = 0; edge < 3; ++edge)
    {
        const float3 current = polygon[edge];
        const float3 next = polygon[(edge + 1) % 3];
        const bool currentAbove = current.z >= 0.0;
        const bool nextAbove = next.z >= 0.0;
        if (currentAbove && clippedCount < 4)
        {
            clipped[clippedCount] = current;
            ++clippedCount;
        }
        if (currentAbove != nextAbove && clippedCount < 4)
        {
            const float t = current.z / (current.z - next.z);
            clipped[clippedCount] = lerp(current, next, t);
            ++clippedCount;
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


/*
* @brief Closest point to `target` inside a triangle. Named `target` because `point` is an
* HLSL reserved word -- a geometry-stage input modifier, the same trap `triangle` is.
* @details Ericson's barycentric region test: check the three vertex Voronoi regions, then the
* three edge regions, and fall through to the face interior. Branchy rather than clever because
* the alternative -- clamping barycentrics computed for the plane projection -- is simply wrong
* outside the triangle, and the outside is the case this exists for.
*/
float3 vkmClosestPointOnTriangle(float3 p0, float3 p1, float3 p2, float3 target)
{
    const float3 ab = p1 - p0;
    const float3 ac = p2 - p0;
    const float3 ap = target - p0;
    const float d1 = dot(ab, ap);
    const float d2 = dot(ac, ap);
    if (d1 <= 0.0 && d2 <= 0.0) { return p0; }

    const float3 bp = target - p1;
    const float d3 = dot(ab, bp);
    const float d4 = dot(ac, bp);
    if (d3 >= 0.0 && d4 <= d3) { return p1; }

    const float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0 && d1 >= 0.0 && d3 <= 0.0)
    {
        return p0 + ab * (d1 / max(d1 - d3, 1e-8));
    }

    const float3 cp = target - p2;
    const float d5 = dot(ab, cp);
    const float d6 = dot(ac, cp);
    if (d6 >= 0.0 && d5 <= d6) { return p2; }

    const float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0 && d2 >= 0.0 && d6 <= 0.0)
    {
        return p0 + ac * (d2 / max(d2 - d6, 1e-8));
    }

    const float va = d3 * d6 - d5 * d4;
    if (va <= 0.0 && (d4 - d3) >= 0.0 && (d5 - d6) >= 0.0)
    {
        return p1 + (p2 - p1) * ((d4 - d3) / max((d4 - d3) + (d5 - d6), 1e-8));
    }

    const float denom = 1.0 / max(va + vb + vc, 1e-8);
    return p0 + ab * (vb * denom) + ac * (vc * denom);
}

/*
* @brief The point on the emitter that stands in for the whole of it, for the specular lobe.
* @details Karis's representative point (Real Shading in Unreal Engine 4). The specular response
* of a polygon has no closed form the way the diffuse one does, so the polygon is replaced by the
* single point on it that the mirror direction reflects towards -- the point that dominates the
* lobe -- and the ordinary punctual GGX is evaluated there.
*
* Where the mirror ray misses the polygon, the plane hit is clamped back into it, so a surface
* whose reflection points past the emitter still sees the nearest part of it rather than nothing.
* A ray that runs parallel to the plane, or points away from it, falls back to the closest point
* to the shading position, which is what the grazing case wants.
*
* This is an approximation, unlike vkmAreaLightFormFactor. It is exact only for a mirror; it
* drifts as roughness rises and the true lobe stops being dominated by one direction, which is
* what vkmAreaLightSpecularWidening compensates for.
*/
float3 vkmAreaLightRepresentativePoint(VkmAreaLight light, float3 worldPosition, float3 reflectionDir)
{
    const float3 p0 = light.p0.xyz;
    const float3 edge1 = light.p1.xyz - p0;
    const float3 edge2 = light.p2.xyz - p0;
    const float3 planeNormal = cross(edge1, edge2);

    const float denominator = dot(reflectionDir, planeNormal);
    const float numerator = dot(p0 - worldPosition, planeNormal);
    // Both signs are allowed: the emitter is two-sided, so a reflection reaching its back face is
    // still reaching it. Only a ray running parallel to the plane has no hit at all.
    float3 candidate = worldPosition;
    if (abs(denominator) > 1e-8)
    {
        const float t = numerator / denominator;
        candidate = t > 0.0 ? worldPosition + reflectionDir * t : worldPosition;
    }
    return vkmClosestPointOnTriangle(p0, light.p1.xyz, light.p2.xyz, candidate);
}

/*
* @brief Roughness widened to stand in for the emitter's angular size, and its energy rescale.
* @details A punctual light evaluated at the representative point concentrates all of the
* emitter's energy into one direction, which on a smooth surface is a pinpoint highlight where
* the truth is a broad one the size of the emitter. Karis's normalization widens the GGX lobe by
* the angle the light subtends and rescales so the total energy is unchanged:
*
*     alpha' = saturate(alpha + r / (2 d)),  energy = (alpha / alpha')^2
*
* `r` is the radius of a disc of the triangle's area -- an emitter has no radius, so the
* equivalent disc is the honest stand-in.
* @param light The emitter.
* @param distanceToLight Distance from the shading point to the representative point.
* @param alpha Roughness squared, as the GGX terms here use it.
* @return x = widened alpha, y = the energy rescale.
*/
float2 vkmAreaLightSpecularWidening(VkmAreaLight light, float distanceToLight, float alpha)
{
    const float3 edge1 = light.p1.xyz - light.p0.xyz;
    const float3 edge2 = light.p2.xyz - light.p0.xyz;
    const float area = 0.5 * length(cross(edge1, edge2));
    const float radius = sqrt(area / 3.14159265);
    const float widened = saturate(alpha + radius / max(2.0 * distanceToLight, 1e-6));
    const float ratio = alpha / max(widened, 1e-6);
    return float2(widened, ratio * ratio);
}

#endif // VKM_AREA_LIGHTS_HLSLI
