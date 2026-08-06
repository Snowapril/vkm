// Copyright (c) 2025 Snowapril
//
// Phase 6's reference path tracer: brute-force, accumulating, no resampling and no denoiser. It
// exists to be *right*, not fast -- every later phase is measured against what this converges to,
// so a Jacobian bug in ReSTIR is only distinguishable from noise if this is trustworthy.
//
// Deliberately unbiased in the parts that matter and deliberately simple everywhere else:
//
//   - **Cosine-weighted hemisphere sampling of a Lambertian BRDF.** The cos(theta) in the
//     rendering equation and the pdf cancel exactly, so a bounce's throughput multiplier is the
//     albedo and nothing else -- no division by a pdf that could be wrong. That is also what makes
//     the white furnace test zero-variance rather than merely convergent: in a uniform environment
//     every path returns exactly albedo^bounces * L_env, so one sample per pixel already gives the
//     analytic answer.
//   - **No next-event estimation.** Light is found by hitting emissive geometry, which is the
//     whole of the area-light representation here. Slow to converge on a small bright light, and
//     that is acceptable for a reference; NEE belongs to Phase 7's shadeSecondaryHit() seam.
//   - **Fixed bounce count, no Russian roulette.** A path that has not escaped after maxBounces is
//     dropped, which loses energy. RR would make it unbiased at the cost of variance; the fixed
//     count is honest and visible, and the furnace fixture escapes in one bounce.
//
// Triangles only, and factor-only materials: see the notes at the fetches below.

#include "vkm_bindless.hlsli"
#include "vkm_frame_constants.hlsli"
#include "vkm_material.hlsli"
#include "vkm_random.hlsli"

// Mirrors vkm::VkmObjectData (include/vkm/renderer/scene/scene.h), restated here for the same
// reason gbuffer.hlsl restates it: scene_common.hlsli also declares the two read-write singletons,
// which this pass has no use for.
struct ObjectData
{
    float4x4 worldTransform;
    float4x4 normalTransform;
    uint     vertexPoolSlot;
    uint     indexPoolSlot;
    uint     vertexWordOffset;
    uint     materialIndex;
    uint     indexOffset;
    uint     indexCount;
    uint     vertexStrideWords;
    uint     _pad0;
    float4   boundsCenterRadius;
};

// Mirrors vkm::VkmFrameData. Only materialPoolSlot is read here.
struct FrameData
{
    float4 frustumPlanes[6];
    float4 lightDirection;
    uint   materialPoolSlot;
    uint   debugMode;
    uint2  _pad0;
};

// Mirrors vkm::VkmPathTraceConstants (include/vkm/renderer/path_tracer.h). Scalars only, so the
// HLSL and the C++ side agree without either having to reason about vector alignment.
struct PathTraceConstants
{
    uint  width;
    uint  height;
    uint  sampleIndex;  // 0 resets the accumulator; anything else adds to it
    uint  maxBounces;
    float environmentR; // uniform environment radiance, the only light besides emissive geometry
    float environmentG;
    float environmentB;
    uint  _pad0;
};

VKM_PUSH_CONSTANTS(PathTraceConstants, g_PathTrace);
VKM_FRAME_CONSTANTS(g_VkmFrame);

// The pools are untyped word arrays, so the "vertex type" is a single u32.
VKM_BINDLESS_VERTEX_PULLING(uint);
VKM_BINDLESS_OBJECT_DATA(ObjectData, g_ObjectData);
VKM_BINDLESS_FRAME_DATA(FrameData, g_FrameData);
VKM_BINDLESS_ACCELERATION_STRUCTURE(g_Scene);
// The loader only -- NOT VKM_MATERIAL_DECLARE(). That macro's samplers call Sample(), which needs
// screen-space derivatives a compute shader does not have, and it would declare a texture array
// this pass never indexes. Materials are therefore factor-only here; see TODO.md.
VKM_MATERIAL_LOADER()

// rgb = summed radiance, a = how many samples are in that sum. One buffer rather than a storage
// texture because VkmTableResourceType has no storage-texture kind (see pipeline_state.h), and a
// buffer is also what a readback test wants.
[[vk::binding(0, 2)]] RWStructuredBuffer<float4> g_Accumulation : register(u0, space2);

float3 loadPosition(uint slot, uint wordBase)
{
    return float3(asfloat(VKM_LOAD_VERTEX(slot, wordBase + 0)),
                  asfloat(VKM_LOAD_VERTEX(slot, wordBase + 1)),
                  asfloat(VKM_LOAD_VERTEX(slot, wordBase + 2)));
}

// The three world-space corners of one triangle of `obj`.
//
// Position is attribute 0 of every VkmVertexLayoutPreset, so only the stride differs between
// layouts -- and that travels in ObjectData rather than in a permutation define, because one ray
// hits whatever is there and cannot know the layout at compile time.
void loadTriangle(ObjectData obj, uint primitiveIndex, out float3 p0, out float3 p1, out float3 p2)
{
    const uint firstIndex = obj.indexOffset + primitiveIndex * 3;
    const uint i0 = VKM_LOAD_INDEX(obj.indexPoolSlot, firstIndex + 0);
    const uint i1 = VKM_LOAD_INDEX(obj.indexPoolSlot, firstIndex + 1);
    const uint i2 = VKM_LOAD_INDEX(obj.indexPoolSlot, firstIndex + 2);

    const float3 local0 = loadPosition(obj.vertexPoolSlot, obj.vertexWordOffset + i0 * obj.vertexStrideWords);
    const float3 local1 = loadPosition(obj.vertexPoolSlot, obj.vertexWordOffset + i1 * obj.vertexStrideWords);
    const float3 local2 = loadPosition(obj.vertexPoolSlot, obj.vertexWordOffset + i2 * obj.vertexStrideWords);

    p0 = mul(obj.worldTransform, float4(local0, 1.0)).xyz;
    p1 = mul(obj.worldTransform, float4(local1, 1.0)).xyz;
    p2 = mul(obj.worldTransform, float4(local2, 1.0)).xyz;
}

// Reconstructs the primary ray for a pixel from set 1's camera, by unprojecting the near and far
// points of its NDC column. Derived from inverseViewProjection rather than from a hand-built basis
// so it cannot disagree with what the rasterizer draws for the same camera.
void primaryRay(uint2 pixel, float2 jitter, out float3 origin, out float3 direction)
{
    const float2 uv = (float2(pixel) + jitter) /
                      float2(g_PathTrace.width, g_PathTrace.height);
    // Engine clip space is +Y up with [0,1] depth on every backend, so y flips and near is z = 0.
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);

    const float4 nearPoint = mul(g_VkmFrame.inverseViewProjection, float4(ndc, 0.0, 1.0));
    const float4 farPoint = mul(g_VkmFrame.inverseViewProjection, float4(ndc, 1.0, 1.0));

    origin = nearPoint.xyz / nearPoint.w;
    direction = normalize(farPoint.xyz / farPoint.w - origin);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 threadId : SV_DispatchThreadID)
{
    const uint2 pixel = threadId.xy;
    if (pixel.x >= g_PathTrace.width || pixel.y >= g_PathTrace.height)
    {
        return;
    }

    const float3 environment =
        float3(g_PathTrace.environmentR, g_PathTrace.environmentG, g_PathTrace.environmentB);
    const uint materialPoolSlot = g_FrameData[0].materialPoolSlot;

    uint rng = vkmSeed(pixel, g_PathTrace.sampleIndex, 0x9E37u);

    float3 origin;
    float3 direction;
    // Jittered inside the pixel, so accumulating samples anti-aliases rather than resampling one
    // point. On the furnace fixture every point of a face gives the same answer, which is why that
    // test stays exact regardless.
    primaryRay(pixel, vkmRandomFloat2(rng), origin, direction);

    float3 radiance = float3(0.0, 0.0, 0.0);
    float3 throughput = float3(1.0, 1.0, 1.0);

    for (uint bounce = 0; bounce < g_PathTrace.maxBounces; ++bounce)
    {
        RayDesc rayDesc;
        rayDesc.Origin = origin;
        rayDesc.Direction = direction;
        rayDesc.TMin = 0.0;
        rayDesc.TMax = 1.0e30;

        RayQuery<RAY_FLAG_NONE> query;
        query.TraceRayInline(g_Scene, RAY_FLAG_NONE, 0xFF, rayDesc);
        query.Proceed();

        if (query.CommittedStatus() != COMMITTED_TRIANGLE_HIT)
        {
            radiance += throughput * environment;
            break;
        }

        // The instance id is the object index, which is also the ObjectData index -- VkmScene
        // gives every instance its object's index for exactly this lookup.
        const ObjectData obj = g_ObjectData[query.CommittedInstanceID()];

        float3 p0, p1, p2;
        loadTriangle(obj, query.CommittedPrimitiveIndex(), p0, p1, p2);

        const float2 bary = query.CommittedTriangleBarycentrics();
        // From the barycentrics rather than origin + t * direction: the latter accumulates the
        // ray parameter's error across bounces, and this is the surface point by definition.
        const float3 hitPosition = p0 * (1.0 - bary.x - bary.y) + p1 * bary.x + p2 * bary.y;

        // The GEOMETRIC normal, from the triangle itself. A reference tracer must not use an
        // interpolated or normal-mapped vector here: those do not agree with the surface a ray
        // can leave from, and the disagreement shows up as energy that is neither conserved nor
        // attributable.
        float3 normal = normalize(cross(p1 - p0, p2 - p0));
        // Two-sided, matching the instances, which both backends build with triangle culling
        // disabled. A one-sided normal would make a back-face hit scatter into the surface.
        if (dot(normal, direction) > 0.0)
        {
            normal = -normal;
        }

        const VkmMaterial material = vkmLoadMaterial(materialPoolSlot, obj.materialIndex);

        // Emissive geometry IS the area light. Added on arrival rather than sampled, since there
        // is no next-event estimation here.
        radiance += throughput * material.emissiveFactor;

        // Lambert with cosine-weighted sampling: BRDF * cos / pdf == albedo, exactly. No pdf
        // division appears anywhere, which is the point.
        throughput *= material.baseColorFactor.rgb;

        // Scale-relative, because a world-space constant is an assumption about scene scale --
        // the same assumption that put the probe volume's far plane at 100 in a 3721-unit Sponza.
        const float offset = max(1.0e-4, query.CommittedRayT() * 1.0e-4);
        origin = hitPosition + normal * offset;
        direction = vkmCosineHemisphere(normal, vkmRandomFloat2(rng));
    }
    // A path still alive here is dropped, and its energy with it. See the header note.

    const uint pixelIndex = pixel.y * g_PathTrace.width + pixel.x;
    const float4 previous = (g_PathTrace.sampleIndex == 0) ? float4(0.0, 0.0, 0.0, 0.0)
                                                           : g_Accumulation[pixelIndex];
    g_Accumulation[pixelIndex] = float4(previous.rgb + radiance, previous.a + 1.0);
}
