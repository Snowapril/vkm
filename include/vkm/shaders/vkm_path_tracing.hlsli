// Copyright (c) 2025 Snowapril
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
    /* The seam restir.md section 12 named: what a secondary hit contributes on arrival, as      */  \
    /* opposed to what continuing the path finds. Emission only today -- the light is found by   */  \
    /* hitting it, which is unbiased and slow. Next-event estimation replaces this body and      */  \
    /* nothing else, which is the whole point of it being one.                                   */  \
    float3 vkmShadeSecondaryHit(VkmSurfacePoint surface, VkmMaterial material, float3 outgoing)        \
    {                                                                                                \
        return material.emissiveFactor;                                                              \
    }                                                                                                \
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
            radiance += throughput * vkmShadeSecondaryHit(surface, material, -direction);                \
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
