// Copyright (c) 2025 Snowapril
//
// Phase 5's gate: a compute shader casts rays at the scene's top-level acceleration structure,
// reached through the bindless set rather than any per-pass binding, and writes hit/miss plus the
// hit distance for a CPU reference to check.
//
// The rays are generated from the thread id rather than read from a buffer, so the only input this
// pass has is the acceleration structure itself -- which is the point. Thread i shoots along -Z
// from z = +2 at (x, y) = kInstanceOrigin + kRayOrigins[i], so a hit is at exactly t = 3 and the
// analytic answer is "is (x, y) inside the unit right triangle".
//
// kInstanceOrigin is where the test places the traced object, and it is load-bearing rather than
// decorative: the triangle's own vertices are at the origin in object space, so a build that
// ignored or mangled the instance transform leaves every one of these rays missing.
//
// Triangles only: see VKM_BINDLESS_ACCELERATION_STRUCTURE in vkm_bindless.hlsli for why a
// procedural-primitive query would compile and be silently wrong on Metal.

#include "vkm_bindless.hlsli"

VKM_BINDLESS_ACCELERATION_STRUCTURE(g_Scene);

// One float2 per ray. Chosen around the fixture triangle (0,0,0)-(1,0,0)-(0,1,0): three well
// inside, one just outside the hypotenuse, one outside the bounding box, and one on the far side
// of the origin.
static const float2 kRayOrigins[6] = {
    float2(0.10f, 0.10f),  // inside
    float2(0.25f, 0.25f),  // inside
    float2(0.05f, 0.90f),  // inside, near the y edge
    float2(0.60f, 0.60f),  // outside: past the hypotenuse x + y = 1
    float2(2.00f, 2.00f),  // outside the bounding box entirely
    float2(-0.10f, 0.10f), // outside: negative x
};

// Where TestRayQueryShared.hpp places the traced object. Its z of -1 is what makes a correct hit
// land at t = 3 rather than at the ray's own start distance, so a plane at the wrong depth is a
// wrong number here rather than an equally plausible one.
static const float2 kInstanceOrigin = float2(10.0f, 20.0f);

// Two words per ray: [hit ? 1 : 0, asuint(t)]. A single float would make "missed" and "hit at
// t = 0" indistinguishable, which is exactly the confusion a gate must not have.
[[vk::binding(0, 2)]] RWStructuredBuffer<uint> g_Result : register(u0, space2);

[numthreads(6, 1, 1)]
void CSMain(uint3 threadId : SV_DispatchThreadID)
{
    const uint ray = threadId.x;

    RayDesc rayDesc;
    rayDesc.Origin = float3(kInstanceOrigin + kRayOrigins[ray], 2.0f);
    rayDesc.Direction = float3(0.0f, 0.0f, -1.0f);
    rayDesc.TMin = 0.0f;
    rayDesc.TMax = 100.0f;

    // RAY_FLAG_NONE, not RAY_FLAG_CULL_BACK_FACING_TRIANGLES: both backends build their instances
    // with triangle culling disabled, so the winding the importer happened to produce must not
    // decide the result.
    RayQuery<RAY_FLAG_NONE> query;
    query.TraceRayInline(g_Scene, RAY_FLAG_NONE, 0xFF, rayDesc);
    // Every geometry is built opaque, so traversal commits triangle hits on its own and this loop
    // needs no CandidateType handling at all.
    query.Proceed();

    const bool hit = query.CommittedStatus() == COMMITTED_TRIANGLE_HIT;
    g_Result[ray * 2 + 0] = hit ? 1u : 0u;
    g_Result[ray * 2 + 1] = asuint(hit ? query.CommittedRayT() : 0.0f);
}
