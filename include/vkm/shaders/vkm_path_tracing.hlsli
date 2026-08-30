// Copyright (c) 2026 Snowapril
//
// The path-tracing seam shared by the reference tracer (path_trace.hlsl) and the 1-spp indirect
// pass (gi_indirect.hlsl). Two estimators of the same integral have to agree, so the parts they
// share -- how a hit is identified, how a surface point is recovered from it, how a ray leaves a
// surface, and what a secondary hit contributes -- live here rather than in each.
//
// Three of those exist as named things rather than as inlined arithmetic because retrofitting them
// later is expensive (restir.md section 8, Phase 7):
//
//   VkmSurfaceHit      how a hit point is ENCODED. Carries (instanceId, primitiveIndex,
//                      barycentrics) and reserves a uv, because the LoD paper locates a surface
//                      point by (instanceId, uv) instead -- a reservoir stores this, so its shape
//                      is an ABI once reservoirs exist.
//   vkmVertexMapping   how an encoded hit becomes a world-space surface point. One function, so a
//                      future encoding changes one body and no call site.
//   vkmShadeSecondaryHit  what a secondary hit contributes. Today emission only, with the rest of
//                      the light found by continuing the path; next-event estimation replaces
//                      exactly this body (restir.md section 12).
//
// Usage: declare the bindless resources first, then invoke the macro. It needs `g_ObjectData`,
// `g_FrameData`, `g_Scene`, VKM_BINDLESS_VERTEX_PULLING and `vkmLoadMaterial` to already exist --
// the same requirement (and the same reason) VKM_MATERIAL_SAMPLERS has.

#ifndef VKM_PATH_TRACING_HLSLI
#define VKM_PATH_TRACING_HLSLI

#include "vkm_lights.hlsli"
#include "vkm_random.hlsli"

/*
* @brief How a ray hit is identified.
*
* @details `instanceId` is the object index, which is also the `VkmObjectData` index -- VkmScene
* gives every instance its object's index for exactly this lookup, so a hit recovers its object
* with no side table.
*
* `uv` is **reserved and currently always zero**. Filling it needs a vertex-layout id in
* ObjectData: uv0 sits at a different offset and in a different format in each
* VkmVertexLayoutPreset, and PositionOnly has none at all, so the stride alone does not say how to
* read it. Recorded in TODO.md. The field exists now because a reservoir's payload is an ABI, and
* widening it after Phase 8 stores millions of them is the expensive kind of change.
*/
struct VkmSurfaceHit
{
    uint   instanceId;
    uint   primitiveIndex;
    float2 barycentrics;
    float2 uv;
    float  rayT;
};

// A hit resolved to somewhere in the world. The normal is the GEOMETRIC one, from the triangle
// itself: an interpolated or normal-mapped vector does not agree with the surface a ray can leave
// from, and offsetting along it causes self-intersection.
struct VkmSurfacePoint
{
    float3 position;
    float3 geometricNormal;
    uint   materialIndex;
};

/*
* @brief Where a ray leaving `position` along `normal` should start.
*
* Scale-relative, because a world-space constant is an assumption about scene scale -- the same
* assumption that once put the probe volume's far plane at 100 in a 3721-unit Sponza. The floor
* keeps a hit at t = 0 from starting exactly on the surface.
*
* A future reconnection or shadow ray also has to shorten its far end (`t_max = (1 - eps) * d`) so
* it does not hit the surface it is aiming at; nothing casts one yet.
*/
float3 vkmOffsetRayOrigin(float3 position, float3 normal, float rayT)
{
    return position + normal * max(1.0e-4, rayT * 1.0e-4);
}

#define VKM_PATH_TRACING_DECLARE()                                                                  \
    float3 vkmLoadPoolPosition(uint slot, uint wordBase)                                             \
    {                                                                                                \
        return float3(asfloat(VKM_LOAD_VERTEX(slot, wordBase + 0)),                                  \
                      asfloat(VKM_LOAD_VERTEX(slot, wordBase + 1)),                                  \
                      asfloat(VKM_LOAD_VERTEX(slot, wordBase + 2)));                                 \
    }                                                                                                \
                                                                                                     \
    /* An encoded hit to a world-space surface point. Position is attribute 0 of every            */ \
    /* VkmVertexLayoutPreset, so only the stride differs between layouts -- and that travels in   */ \
    /* ObjectData rather than in a permutation define, because one ray hits whatever is there.    */ \
    VkmSurfacePoint vkmVertexMapping(VkmSurfaceHit hit)                                              \
    {                                                                                                \
        const ObjectData obj = g_ObjectData[hit.instanceId];                                         \
                                                                                                     \
        const uint firstIndex = obj.indexOffset + hit.primitiveIndex * 3;                            \
        const uint i0 = VKM_LOAD_INDEX(obj.indexPoolSlot, firstIndex + 0);                           \
        const uint i1 = VKM_LOAD_INDEX(obj.indexPoolSlot, firstIndex + 1);                           \
        const uint i2 = VKM_LOAD_INDEX(obj.indexPoolSlot, firstIndex + 2);                           \
                                                                                                     \
        const float3 l0 = vkmLoadPoolPosition(obj.vertexPoolSlot,                                    \
                                              obj.vertexWordOffset + i0 * obj.vertexStrideWords);    \
        const float3 l1 = vkmLoadPoolPosition(obj.vertexPoolSlot,                                    \
                                              obj.vertexWordOffset + i1 * obj.vertexStrideWords);    \
        const float3 l2 = vkmLoadPoolPosition(obj.vertexPoolSlot,                                    \
                                              obj.vertexWordOffset + i2 * obj.vertexStrideWords);    \
                                                                                                     \
        const float3 p0 = mul(obj.worldTransform, float4(l0, 1.0)).xyz;                              \
        const float3 p1 = mul(obj.worldTransform, float4(l1, 1.0)).xyz;                              \
        const float3 p2 = mul(obj.worldTransform, float4(l2, 1.0)).xyz;                              \
                                                                                                     \
        VkmSurfacePoint surface;                                                                       \
        /* From the barycentrics rather than origin + t * direction: the latter accumulates the */   \
        /* ray parameter's error across bounces, and this is the surface point by definition.   */   \
        surface.position = p0 * (1.0 - hit.barycentrics.x - hit.barycentrics.y) +                      \
                         p1 * hit.barycentrics.x + p2 * hit.barycentrics.y;                          \
        surface.geometricNormal = normalize(cross(p1 - p0, p2 - p0));                                  \
        surface.materialIndex = obj.materialIndex;                                                     \
        return surface;                                                                                \
    }                                                                                                \
                                                                                                     \
    /* Runs one ray query and reports whether it hit, filling `outHit` when it did.             */   \
    bool vkmTraceClosest(float3 origin, float3 direction, out VkmSurfaceHit outHit)                  \
    {                                                                                                \
        RayDesc rayDesc;                                                                             \
        rayDesc.Origin = origin;                                                                     \
        rayDesc.Direction = direction;                                                               \
        rayDesc.TMin = 0.0;                                                                          \
        rayDesc.TMax = 1.0e30;                                                                       \
                                                                                                     \
        RayQuery<RAY_FLAG_NONE> query;                                                               \
        query.TraceRayInline(g_Scene, RAY_FLAG_NONE, 0xFF, rayDesc);                                 \
        /* Every geometry is built opaque, so traversal commits triangle hits on its own and    */   \
        /* one Proceed() runs it to completion -- no CandidateType handling is needed.          */   \
        query.Proceed();                                                                             \
                                                                                                     \
        outHit = (VkmSurfaceHit)0;                                                                   \
        if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)                                       \
        {                                                                                            \
            return false;                                                                            \
        }                                                                                            \
        outHit.instanceId = query.CommittedInstanceID();                                             \
        outHit.primitiveIndex = query.CommittedPrimitiveIndex();                                     \
        outHit.barycentrics = query.CommittedTriangleBarycentrics();                                 \
        outHit.rayT = query.CommittedRayT();                                                         \
        return true;                                                                                 \
    }                                                                                                \
                                                                                                     \
    /* Whether `targetPosition` is reachable from `surface` -- the shadow segment for a          */  \
    /* reconnection to a sampled light point. Both endpoints are lifted off their surfaces and   */  \
    /* the direction, distance and t_max are all measured from the OFFSET origin: a grazing      */  \
    /* segment ending exactly on a triangle is not cleared by relative t_max shortening alone,   */  \
    /* which is the asymmetry that once cost 13.8% of the mean (gi_reservoir_spatial.hlsl).      */  \
    bool vkmShadowSegmentVisible(VkmSurfacePoint surface, float rayT,                                \
                                 float3 targetPosition, float3 targetNormal)                         \
    {                                                                                                \
        const float3 origin = vkmOffsetRayOrigin(surface.position, surface.geometricNormal, rayT);   \
        const float3 target = vkmOffsetRayOrigin(targetPosition, targetNormal,                       \
                                                 length(targetPosition - origin));                   \
        const float3 toTarget = target - origin;                                                     \
        const float distanceToTarget = length(toTarget);                                             \
        if (distanceToTarget <= 0.0)                                                                 \
        {                                                                                            \
            return false;                                                                            \
        }                                                                                            \
        RayDesc rayDesc;                                                                             \
        rayDesc.Origin = origin;                                                                     \
        rayDesc.Direction = toTarget / distanceToTarget;                                             \
        rayDesc.TMin = 0.0;                                                                          \
        rayDesc.TMax = distanceToTarget * 0.999;                                                     \
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;                                    \
        query.TraceRayInline(g_Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, rayDesc);      \
        query.Proceed();                                                                             \
        return query.CommittedStatus() != COMMITTED_TRIANGLE_HIT;                                    \
    }                                                                                                \
                                                                                                     \
    /* Whether a punctual light at `lightPosition` is reachable from `surface`. Distinct from  */    \
    /* the segment version because a delta light sits on no surface: there is no target normal */    \
    /* to lift the far endpoint off, so the ray stops just short of the light itself. Distance */    \
    /* is measured from the OFFSET origin, for the reason the segment version records.         */    \
    bool vkmShadowPointVisible(VkmSurfacePoint surface, float rayT, float3 lightPosition)            \
    {                                                                                                \
        const float3 origin = vkmOffsetRayOrigin(surface.position, surface.geometricNormal, rayT);   \
        const float3 toLight = lightPosition - origin;                                               \
        const float distanceToLight = length(toLight);                                               \
        if (distanceToLight <= 0.0)                                                                  \
        {                                                                                            \
            return false;                                                                            \
        }                                                                                            \
        RayDesc rayDesc;                                                                             \
        rayDesc.Origin = origin;                                                                     \
        rayDesc.Direction = toLight / distanceToLight;                                               \
        rayDesc.TMin = 0.0;                                                                          \
        rayDesc.TMax = distanceToLight * 0.999;                                                      \
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;                                    \
        query.TraceRayInline(g_Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, rayDesc);      \
        query.Proceed();                                                                             \
        return query.CommittedStatus() != COMMITTED_TRIANGLE_HIT;                                    \
    }                                                                                                \
                                                                                                     \
                                                                                                     \
    /* Whether `direction` escapes the scene from `surface` -- the shadow ray for a directional  */  \
    /* light, which has no far endpoint to shorten towards.                                      */  \
    bool vkmShadowRayVisible(VkmSurfacePoint surface, float rayT, float3 direction)                  \
    {                                                                                                \
        RayDesc rayDesc;                                                                             \
        rayDesc.Origin = vkmOffsetRayOrigin(surface.position, surface.geometricNormal, rayT);        \
        rayDesc.Direction = direction;                                                               \
        rayDesc.TMin = 0.0;                                                                          \
        rayDesc.TMax = 1.0e30;                                                                       \
        RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH> query;                                    \
        query.TraceRayInline(g_Scene, RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH, 0xFF, rayDesc);      \
        query.Proceed();                                                                             \
        return query.CommittedStatus() != COMMITTED_TRIANGLE_HIT;                                    \
    }                                                                                                \
                                                                                                     \
    /* The seam restir.md section 12 named, now carrying what it was reserved for: next-event    */  \
    /* estimation. What a vertex contributes is the light REACHING it from the scene's light     */  \
    /* table -- one deterministic directional sample, one area sample drawn proportionally      */  \
    /* to emitted power, and every model-placed punctual light in closed form --                */  \
    /* times the Lambertian BRDF; its own emission belongs to the path's                        */  \
    /* first vertex only (see the loop), or it would be counted once by NEE from the previous    */  \
    /* vertex and again on arrival.                                                              */  \
    /*                                                                                           */  \
    /* The area estimator is in the AREA measure: L * (albedo/pi) * cos_x * |cos_y| / (d^2 *     */  \
    /* pdfArea). |cos_y|, not a clamp: emission-on-hit is two-sided (the loop flips normals), so */  \
    /* NEE must integrate the same emitter from both sides. On a scene whose table is empty and  */  \
    /* whose sun is black this draws NO randoms and returns 0 -- which is what keeps the         */  \
    /* light-less gates bit-identical.                                                           */  \
    float3 vkmShadeSecondaryHit(VkmSurfacePoint surface, VkmMaterial material, float rayT,           \
                                inout uint rng)                                                      \
    {                                                                                                \
        const uint lightPoolSlot = g_FrameData[0].lightPoolSlot;                                     \
        const uint lightCount = g_FrameData[0].lightCount;                                           \
        const float3 albedoOverPi = material.baseColorFactor.rgb * (1.0 / 3.14159265358979);         \
        float3 nee = float3(0.0, 0.0, 0.0);                                                          \
                                                                                                     \
        const float3 sunRadiance = vkmLoadSunRadiance(lightPoolSlot);                                \
        if (any(sunRadiance > 0.0))                                                                  \
        {                                                                                            \
            const float3 toSun = g_FrameData[0].lightDirection.xyz;                                  \
            const float cosSun = dot(surface.geometricNormal, toSun);                                \
            if (cosSun > 0.0 && vkmShadowRayVisible(surface, rayT, toSun))                           \
            {                                                                                        \
                nee += albedoOverPi * cosSun * sunRadiance;                                          \
            }                                                                                        \
        }                                                                                            \
                                                                                                     \
        if (lightCount > 0)                                                                          \
        {                                                                                            \
            const float pick = vkmRandomFloat(rng);                                                  \
            const float2 uv = vkmRandomFloat2(rng);                                                  \
            const VkmLightSample lightSample =                                                       \
                vkmSampleLightTable(lightPoolSlot, lightCount, pick, uv);                            \
            const float3 toLight = lightSample.position - surface.position;                          \
            const float distanceSquared = dot(toLight, toLight);                                     \
            if (distanceSquared > 1.0e-8)                                                            \
            {                                                                                        \
                const float3 lightDirection = toLight * rsqrt(distanceSquared);                      \
                const float cosSurface = dot(surface.geometricNormal, lightDirection);               \
                const float cosLight = abs(dot(lightSample.normal, -lightDirection));                \
                if (cosSurface > 0.0 && cosLight > 0.0 &&                                            \
                    vkmShadowSegmentVisible(surface, rayT, lightSample.position, lightSample.normal)) \
                {                                                                                    \
                    nee += lightSample.radiance * albedoOverPi * cosSurface * cosLight /             \
                           (distanceSquared * lightSample.pdfArea);                                  \
                }                                                                                    \
            }                                                                                        \
        }                                                                                            \
                                                                                                     \
        /* Model-placed punctual lights. Delta lights, so this is closed form: no pdf, no       */   \
        /* randoms, and the loop is exhaustive rather than a single importance-sampled pick --  */   \
        /* one sample of a delta light already has zero variance, so picking one of N would buy */   \
        /* only a cost saving and pay for it in noise. The cost is one shadow ray per light per */   \
        /* bounce; TODO.md records the cap that will eventually need.                           */   \
        const uint punctualCount = vkmLoadPunctualCount(lightPoolSlot);                              \
        for (uint punctualIndex = 0; punctualIndex < punctualCount; ++punctualIndex)                 \
        {                                                                                            \
            const VkmPunctualLight light =                                                           \
                vkmLoadPunctualLight(lightPoolSlot, lightCount, punctualIndex);                      \
            const VkmPunctualSample lightSample =                                                    \
                vkmSamplePunctualLight(light, surface.position);                                     \
            /* Zero radiance is how a spot reports "outside the cone" and a ranged point light */    \
            /* reports "past the range", so this skips the ray rather than tracing for a zero. */    \
            if (!any(lightSample.radiance > 0.0))                                                    \
            {                                                                                        \
                continue;                                                                            \
            }                                                                                        \
            const float cosSurface = dot(surface.geometricNormal, lightSample.direction);            \
            if (cosSurface <= 0.0)                                                                   \
            {                                                                                        \
                continue;                                                                            \
            }                                                                                        \
            const bool visible =                                                                     \
                light.type == VKM_LIGHT_TYPE_DIRECTIONAL                                             \
                    ? vkmShadowRayVisible(surface, rayT, lightSample.direction)                      \
                    : vkmShadowPointVisible(surface, rayT, light.positionWorld);                     \
            if (visible)                                                                             \
            {                                                                                        \
                nee += lightSample.radiance * albedoOverPi * cosSurface;                             \
            }                                                                                        \
        }                                                                                            \
        return nee;                                                                                  \
    }                                                                                                \
                                                                                                     \
                                                                                                     \
    /*                                                                                            */ \
    /* Radiance arriving back along a ray, by continuing it for at most `maxBounces` scatters.    */ \
    /*                                                                                            */ \
    /* Cosine-weighted sampling of a Lambertian BRDF: the cos(theta) in the rendering equation    */ \
    /* and the pdf cancel exactly, so a bounce's throughput multiplier is the albedo and nothing  */ \
    /* else. No pdf division appears anywhere, which is what makes the furnace test zero-variance */ \
    /* rather than merely convergent.                                                             */ \
    /*                                                                                            */ \
    /* A path still alive after `maxBounces` is dropped, and its energy with it: there is no      */ \
    /* Russian roulette, so that count is a bias knob and not only a cost one (TODO.md).          */ \
    /*                                                                                            */ \
    /* Reports the first hit alongside the radiance, which is what a reservoir needs: the sample  */ \
    /* it carries IS that hit, and the radiance is what leaves it towards the shading point.      */ \
    /* Sharing the loop rather than tracing the first ray twice is the point -- two copies would  */ \
    /* be two chances to disagree about the offset, the two-sidedness or the bounce count.        */ \
    /*                                                                                            */ \
    /* `outHit` is false when the first ray escaped; `outFirst` is then untouched and the caller  */ \
    /* decides what an environment sample means to it.                                           */ \
    float3 vkmTracePathFirstHit(float3 origin, float3 direction, uint maxBounces,                    \
                                float3 environment, uint materialPoolSlot, inout uint rng,           \
                                out bool outHit, out VkmSurfacePoint outFirst)                       \
    {                                                                                                \
        outHit = false;                                                                              \
        outFirst = (VkmSurfacePoint)0;                                                                \
        float3 radiance = float3(0.0, 0.0, 0.0);                                                     \
        float3 throughput = float3(1.0, 1.0, 1.0);                                                   \
                                                                                                     \
        for (uint bounce = 0; bounce < maxBounces; ++bounce)                                         \
        {                                                                                            \
            VkmSurfaceHit hit;                                                                       \
            if (!vkmTraceClosest(origin, direction, hit))                                             \
            {                                                                                        \
                radiance += throughput * environment;                                                 \
                break;                                                                                \
            }                                                                                         \
                                                                                                      \
            VkmSurfacePoint surface = vkmVertexMapping(hit);                                            \
            /* Two-sided, matching the instances, which both backends build with triangle      */     \
            /* culling disabled. A one-sided normal would scatter into the surface.            */     \
            if (dot(surface.geometricNormal, direction) > 0.0)                                          \
            {                                                                                         \
                surface.geometricNormal = -surface.geometricNormal;                                        \
            }                                                                                          \
            /* Reported AFTER the flip, so a reservoir's sample normal faces the shading point   */    \
            /* it will be resampled towards -- which is the orientation a neighbour rejection    */    \
            /* test and a cosine both want.                                                      */    \
            if (bounce == 0)                                                                          \
            {                                                                                          \
                outHit = true;                                                                         \
                outFirst = surface;                                                                    \
            }                                                                                          \
                                                                                                       \
            const VkmMaterial material = vkmLoadMaterial(materialPoolSlot, surface.materialIndex);       \
            /* Emission at the path's own first vertex only: every later vertex's light arrives  */    \
            /* by NEE from its predecessor, so counting it again on arrival would double it.     */    \
            if (bounce == 0)                                                                            \
            {                                                                                           \
                radiance += throughput * material.emissiveFactor;                                       \
            }                                                                                           \
            radiance += throughput * vkmShadeSecondaryHit(surface, material, hit.rayT, rng);            \
            throughput *= material.baseColorFactor.rgb;                                                \
                                                                                                       \
            origin = vkmOffsetRayOrigin(surface.position, surface.geometricNormal, hit.rayT);              \
            direction = vkmCosineHemisphere(surface.geometricNormal, vkmRandomFloat2(rng));              \
        }                                                                                              \
        return radiance;                                                                               \
    }                                                                                                  \
                                                                                                       \
    /* The same estimator without the first-hit report, for callers that only want the radiance. */    \
    float3 vkmTracePath(float3 origin, float3 direction, uint maxBounces,                              \
                        float3 environment, uint materialPoolSlot, inout uint rng)                     \
    {                                                                                                  \
        bool ignoredHit;                                                                               \
        VkmSurfacePoint ignoredSurface;                                                                \
        return vkmTracePathFirstHit(origin, direction, maxBounces, environment, materialPoolSlot,      \
                                    rng, ignoredHit, ignoredSurface);                                  \
    }

#endif // VKM_PATH_TRACING_HLSLI
