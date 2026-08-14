// Copyright (c) 2025 Snowapril
//
// Fills the G-buffer both GI tiers read (see renderer/gbuffer.h for the channel layout, and
// vkm_gbuffer.hlsli for the packing this shader and its readers share).
//
// Structurally the same bindless, GPU-driven vertex-pulling draw as scene_object drawing: no
// vertex input attributes, no push constants, SV_InstanceID IS the ObjectData index, the camera
// comes from set 1, and everything else hangs off the ObjectData record. The vertex-layout
// permutations come from the PSO JSON's "options", exactly as the model_viewer shader's do.
//
// What is new here is motion vectors. Each vertex is transformed twice -- once by this frame's
// jitter-free viewProjection and once by the previous frame's -- and the fragment stage divides
// both. That
// captures CAMERA motion only: VkmObjectData carries one world transform, so an object that moved
// between frames reports the motion of a static object at its new position. That is correct for
// the static geometry the engine can currently express, and wrong the moment objects animate, so
// a per-object previous transform is the prerequisite for dynamic motion vectors.

#include "vkm_bindless.hlsli"
#include "vkm_frame_constants.hlsli"
#include "vkm_gbuffer.hlsli"
#include "vkm_material.hlsli"

// Deliberately NOT scene_common.hlsli, even though it defines these same records: that header also
// declares the two read-write singletons, which are compute-only on purpose (WebGPU forbids
// writable storage in the vertex stage, and a buffer used as writable storage inside a render
// pass's usage scope may not also be fetched as an indirect argument there -- see
// vkm_bindless.hlsli). A render pipeline's shader must never declare them, which is why the
// drawing shaders restate the records they need.

// Mirrors vkm::VkmObjectData (include/vkm/renderer/scene/scene.h). The padding is part of the
// layout, not decoration.
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
    uint     vertexStrideWords; // read only by the ray-tracing kernels; a draw knows its own layout
    uint     _pad0;
    float4   boundsCenterRadius;
};

// Mirrors vkm::VkmFrameData. Carries no camera: that is set 1's job.
struct FrameData
{
    float4 frustumPlanes[6];
    float4 lightDirection;
    uint   materialPoolSlot;
    uint   debugMode;
    uint2  _pad0;
};

// The pools are untyped word arrays, so the "vertex type" is a single u32.
VKM_BINDLESS_VERTEX_PULLING(uint);
VKM_BINDLESS_OBJECT_DATA(ObjectData, g_ObjectData);
VKM_BINDLESS_FRAME_DATA(FrameData, g_FrameData);
VKM_MATERIAL_DECLARE();

#if defined(VKM_VERTEX_LAYOUT_STANDARD_PBR)
    // position f32x3 @0, normal f32x3 @16, uv0 f32x2 @32, tangent f32x4 @48; stride 64 = 16 words
    #define VERTEX_STRIDE_WORDS 16
#elif defined(VKM_VERTEX_LAYOUT_COMPACT)
    // position f32x3 @0, normal snorm8x4 @12, uv0 f16x2 @16, tangent snorm8x4 @20; stride 32 = 8 words
    #define VERTEX_STRIDE_WORDS 8
#elif defined(VKM_VERTEX_LAYOUT_POSITION_ONLY)
    // position f32x3 @0; stride 16 = 4 words
    #define VERTEX_STRIDE_WORDS 4
#else
    #error "gbuffer.hlsl requires a VKM_VERTEX_LAYOUT_* permutation define (see the PSO JSON options)"
#endif

VKM_FRAME_CONSTANTS(g_VkmFrame);

struct VSOutput
{
    float4 position : SV_POSITION;
    [[vk::location(0)]] float3 worldNormal : NORMAL0;
    [[vk::location(1)]] float3 worldPosition : TEXCOORD0;
    // Clip positions carried un-divided so the fragment stage can divide them itself: dividing in
    // the vertex stage and interpolating the result is wrong under perspective.
    [[vk::location(2)]] float4 currentClip : TEXCOORD1;
    [[vk::location(3)]] float4 previousClip : TEXCOORD2;
    [[vk::location(4)]] nointerpolation uint materialIndex : TEXCOORD3;
    [[vk::location(5)]] float2 uv : TEXCOORD4;
};

struct PSOutput
{
    float4 normal : SV_TARGET0;             // xy = shading normal, zw = geometric normal
    float4 baseColorRoughness : SV_TARGET1; // rgb = base colour, a = roughness
    float4 motionMetallic : SV_TARGET2;     // xy = motion (UV), z = metallic, w = camera distance
};

float3 loadFloat3(uint slot, uint wordBase)
{
    return float3(asfloat(VKM_LOAD_VERTEX(slot, wordBase + 0)),
                  asfloat(VKM_LOAD_VERTEX(slot, wordBase + 1)),
                  asfloat(VKM_LOAD_VERTEX(slot, wordBase + 2)));
}

// Unpacks a snorm8x4 word, the inverse of meshopt_quantizeSnorm(v, 8).
float4 unpackSnorm8x4(uint packed)
{
    const int4 signedBytes = int4(packed << 24, packed << 16, packed << 8, packed) >> 24;
    return max(float4(signedBytes) / 127.0, -1.0);
}

float3 fetchNormal(ObjectData obj, uint wordBase)
{
#if defined(VKM_VERTEX_LAYOUT_STANDARD_PBR)
    return loadFloat3(obj.vertexPoolSlot, wordBase + 4); // normal at byte 16 == word 4
#elif defined(VKM_VERTEX_LAYOUT_COMPACT)
    return unpackSnorm8x4(VKM_LOAD_VERTEX(obj.vertexPoolSlot, wordBase + 3)).xyz; // normal at byte 12 == word 3
#else
    // PositionOnly carries no shading normal; the geometric normal derived in the fragment stage
    // is the only one available, and the two are written identically below.
    return float3(0.0, 0.0, 0.0);
#endif
}

// uv0 lives at byte 32 (word 8) in StandardPBR and byte 16 (word 4, packed f16x2) in Compact.
// PositionOnly carries none, so its materials are factor-only -- correct rather than a fallback,
// since there is no coordinate to sample at.
float2 fetchUV(ObjectData obj, uint wordBase)
{
#if defined(VKM_VERTEX_LAYOUT_STANDARD_PBR)
    return float2(asfloat(VKM_LOAD_VERTEX(obj.vertexPoolSlot, wordBase + 8)),
                  asfloat(VKM_LOAD_VERTEX(obj.vertexPoolSlot, wordBase + 9)));
#elif defined(VKM_VERTEX_LAYOUT_COMPACT)
    const uint packed = VKM_LOAD_VERTEX(obj.vertexPoolSlot, wordBase + 4);
    return float2(f16tof32(packed & 0xFFFFu), f16tof32(packed >> 16));
#else
    return float2(0.0, 0.0);
#endif
}

VSOutput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    const ObjectData obj = g_ObjectData[instanceId];

    const uint index = VKM_LOAD_INDEX(obj.indexPoolSlot, obj.indexOffset + vertexId);
    const uint wordBase = obj.vertexWordOffset + index * VERTEX_STRIDE_WORDS;

    const float3 position = loadFloat3(obj.vertexPoolSlot, wordBase);
    const float3 normal = fetchNormal(obj, wordBase);

    const float4 worldPosition = mul(obj.worldTransform, float4(position, 1.0));

    VSOutput output;
    output.position = mul(g_VkmFrame.viewProjection, worldPosition);
    // Motion vectors use the jitter-free pair: viewProjection carries the temporal upscaler's
    // sub-pixel jitter, which must not appear as apparent motion.
    output.currentClip = mul(g_VkmFrame.viewProjectionNoJitter, worldPosition);
    // Camera-only motion: the same world position through last frame's camera. An object that
    // moved needs its own previous transform, which VkmObjectData does not carry.
    output.previousClip = mul(g_VkmFrame.prevViewProjection, worldPosition);
    output.worldPosition = worldPosition.xyz;
    output.worldNormal = mul((float3x3)obj.normalTransform, normal);
    output.materialIndex = obj.materialIndex;
    output.uv = fetchUV(obj, wordBase);
    return output;
}

PSOutput PSMain(VSOutput input)
{
    // Derived from the world-position derivatives across the triangle, so it is the true face
    // normal regardless of what the vertex normals say. This is the one a secondary ray offsets
    // along; using an interpolated or normal-mapped vector there causes self-intersection.
    //
    // **Oriented towards the camera**, which the derivative cross product alone does not give:
    // its sign follows the screen-space derivative order, and on this engine's conventions it
    // came out pointing INTO the surface. A rasterized pixel is by definition a surface facing
    // the eye, so this is well defined -- and it is what the channel is for. Nothing had consumed
    // it before Phase 7's indirect pass (SSGI marches in screen space, deferred lighting uses the
    // shading normal, gi_composite only visualizes it), so the first consumer to offset a ray
    // along it is what found this: with a small offset the ray re-hit its own wall and the image
    // came out 14% dark, and with a large one the origin ended up outside the room entirely.
    const float3 towardsCamera = g_VkmFrame.cameraPositionWorld.xyz - input.worldPosition;
    float3 geometricNormal =
        normalize(cross(ddx(input.worldPosition), ddy(input.worldPosition)));
    if (dot(geometricNormal, towardsCamera) < 0.0)
    {
        geometricNormal = -geometricNormal;
    }

    // PositionOnly geometry has no vertex normal, so the two normals coincide there.
    const float3 shadingNormal = (dot(input.worldNormal, input.worldNormal) > 0.0)
                                     ? normalize(input.worldNormal)
                                     : geometricNormal;

    const VkmMaterial material = vkmLoadMaterial(g_FrameData[0].materialPoolSlot, input.materialIndex);
    const float4 baseColor = vkmSampleBaseColor(material, input.uv);
    const float2 metallicRoughness = vkmSampleMetallicRoughness(material, input.uv);

    PSOutput output;
    output.normal = vkmPackGBufferNormals(shadingNormal, geometricNormal);
    output.baseColorRoughness = float4(baseColor.rgb, metallicRoughness.y);
    // Distance from the camera rather than hardware depth, so consumers reconstruct world
    // positions from this instead of sampling the depth attachment. That matters beyond taste:
    // WebGPU validates a depth-format view against a depth sample type, so binding the depth
    // attachment as an ordinary sampled texture is invalid there.
    output.motionMetallic = float4(vkmComputeMotionVector(input.currentClip, input.previousClip),
                                   metallicRoughness.x,
                                   distance(input.worldPosition, g_VkmFrame.cameraPositionWorld.xyz));
    return output;
}
