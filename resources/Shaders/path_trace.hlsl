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
// The estimator itself -- the path loop, the hit encoding, the vertex mapping and the
// secondary-hit seam -- lives in vkm_path_tracing.hlsli, shared with the 1-spp indirect pass this
// is the reference for. What is left here is the camera: primary rays and accumulation.
//
// Triangles only, and factor-only materials: see vkm_path_tracing.hlsli.

#include "vkm_bindless.hlsli"
#include "vkm_frame_constants.hlsli"
#include "vkm_material.hlsli"
#include "vkm_path_tracing.hlsli"
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
    uint   lightPoolSlot;
    uint   lightCount;
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
    uint  jitterPrimaryRay; // 0 samples the pixel centre, for comparison against a deferred pass
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
VKM_LIGHT_LOADER()
// The path loop, the hit encoding and the vertex mapping are shared with gi_indirect.hlsl -- two
// estimators of the same integral must not each have their own copy of what they have in common.
VKM_PATH_TRACING_DECLARE()

// rgb = summed radiance, a = how many samples are in that sum. One buffer rather than a storage
// texture because VkmTableResourceType has no storage-texture kind (see pipeline_state.h), and a
// buffer is also what a readback test wants.
[[vk::binding(0, 2)]] RWStructuredBuffer<float4> g_Accumulation : register(u0, space2);

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
    // test stays exact regardless. The rng is advanced either way, so turning the jitter off does
    // not also change which paths are sampled.
    const float2 jitter = vkmRandomFloat2(rng);
    primaryRay(pixel, g_PathTrace.jitterPrimaryRay != 0 ? jitter : float2(0.5, 0.5),
               origin, direction);

    const float3 radiance = vkmTracePath(origin, direction, g_PathTrace.maxBounces,
                                        environment, materialPoolSlot, rng);

    const uint pixelIndex = pixel.y * g_PathTrace.width + pixel.x;
    const float4 previous = (g_PathTrace.sampleIndex == 0) ? float4(0.0, 0.0, 0.0, 0.0)
                                                           : g_Accumulation[pixelIndex];
    g_Accumulation[pixelIndex] = float4(previous.rgb + radiance, previous.a + 1.0);
}
