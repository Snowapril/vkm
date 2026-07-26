// Copyright (c) 2025 Snowapril
//
// Bindless, GPU-driven vertex-pulling glTF shader. The pipeline declares no vertex input
// attributes and pushes no constants at all:
//
//   - SV_InstanceID IS the ObjectData index. The scene passes it as an ordinary draw's
//     firstInstance, and the culling pass writes it into the indirect arguments' firstInstance,
//     so the direct and GPU-driven draw paths share this one shader unchanged.
//   - The camera comes from descriptor set 1, which the engine rewrites once per frame
//     (vkm_frame_constants.hlsli).
//   - Everything else hangs off the ObjectData record SV_InstanceID selects (pool slots, pool
//     offsets, transforms, material index) or off the scene's own FrameData singleton, which
//     carries only what set 1 does not: the light and the material pool's bindless slot.
//
// Geometry pools are untyped u32 word arrays on every backend, which is what lets one pool hold
// vertices of any stride (see VkmVertexLayout): VKM_BINDLESS_VERTEX_PULLING(uint) makes
// VKM_LOAD_VERTEX a plain word load. The only per-permutation code is fetchNormal(); the PSO JSON
// "options" supply exactly one VKM_VERTEX_LAYOUT_* define per build.

#include "vkm_bindless.hlsli"
#include "vkm_frame_constants.hlsli"

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
    #error "model_viewer.hlsl requires a VKM_VERTEX_LAYOUT_* permutation define (see renderpass.json options)"
#endif

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
    uint2    _pad0;
    float4   boundsCenterRadius;
};

// Mirrors vkm::VkmFrameData. Deliberately carries no camera: that is set 1's job.
struct FrameData
{
    float4 frustumPlanes[6]; // world space, normalized, xyz = normal, w = distance
    float4 lightDirection;   // xyz: normalized world-space direction towards the light
    uint   materialPoolSlot;
    uint3  _pad0;
};

// The pools are untyped word arrays, so the "vertex type" is a single u32.
VKM_BINDLESS_VERTEX_PULLING(uint);
VKM_BINDLESS_OBJECT_DATA(ObjectData, g_VkmObjectData);
VKM_BINDLESS_FRAME_DATA(FrameData, g_VkmSceneFrame);
VKM_FRAME_CONSTANTS(g_VkmFrame);

struct VSOutput
{
    float4 position : SV_POSITION;
    [[vk::location(0)]] float3 normal : NORMAL0;
    [[vk::location(1)]] float4 baseColor : COLOR0;
};

float3 loadFloat3(uint slot, uint wordBase)
{
    return float3(asfloat(VKM_LOAD_VERTEX(slot, wordBase + 0)),
                  asfloat(VKM_LOAD_VERTEX(slot, wordBase + 1)),
                  asfloat(VKM_LOAD_VERTEX(slot, wordBase + 2)));
}

// Unpacks the low three components of a snorm8x4 word, the inverse of meshopt_quantizeSnorm(v, 8).
float3 unpackSnorm8x3(uint packed)
{
    const int3 signedBytes = int3(packed << 24, packed << 16, packed << 8) >> 24;
    return max(float3(signedBytes) / 127.0, -1.0);
}

// One object's vertex `index` in its pool, in that pool's layout.
float3 fetchNormal(ObjectData obj, uint wordBase)
{
#if defined(VKM_VERTEX_LAYOUT_STANDARD_PBR)
    return loadFloat3(obj.vertexPoolSlot, wordBase + 4); // normal at byte 16 == word 4
#elif defined(VKM_VERTEX_LAYOUT_COMPACT)
    return unpackSnorm8x3(VKM_LOAD_VERTEX(obj.vertexPoolSlot, wordBase + 3)); // normal at byte 12 == word 3
#else
    // PositionOnly carries no shading data; a constant normal keeps the geometry visible.
    return float3(0.0, 0.0, 1.0);
#endif
}

float4 fetchBaseColor(uint materialPoolSlot, uint materialIndex)
{
    // VkmMaterialData is 48 bytes = 12 words, with baseColorFactor first.
    const uint wordBase = materialIndex * 12;
    return float4(asfloat(VKM_LOAD_VERTEX(materialPoolSlot, wordBase + 0)),
                  asfloat(VKM_LOAD_VERTEX(materialPoolSlot, wordBase + 1)),
                  asfloat(VKM_LOAD_VERTEX(materialPoolSlot, wordBase + 2)),
                  asfloat(VKM_LOAD_VERTEX(materialPoolSlot, wordBase + 3)));
}

VSOutput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    const ObjectData obj = g_VkmObjectData[instanceId];

    const uint index = VKM_LOAD_INDEX(obj.indexPoolSlot, obj.indexOffset + vertexId);
    const uint wordBase = obj.vertexWordOffset + index * VERTEX_STRIDE_WORDS;

    const float3 position = loadFloat3(obj.vertexPoolSlot, wordBase);
    const float3 normal = fetchNormal(obj, wordBase);

    VSOutput output;
    // Two mat4 x vec4 products rather than folding VP*M per draw: the camera is shared by every
    // draw, so it stays in set 1 and only the model matrix comes from the object record.
    output.position = mul(g_VkmFrame.viewProjection, mul(obj.worldTransform, float4(position, 1.0)));
    // Shading is world-space now that there are no per-draw push constants to carry an
    // object-space light in; normalTransform is the inverse-transpose of worldTransform.
    output.normal = mul((float3x3)obj.normalTransform, normal);
    output.baseColor = fetchBaseColor(g_VkmSceneFrame[0].materialPoolSlot, obj.materialIndex);
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    const float3 normal = normalize(input.normal);
    const float3 lightDirection = normalize(g_VkmSceneFrame[0].lightDirection.xyz);

    // Half-Lambert wrap so backfacing-but-visible geometry stays readable instead of black.
    const float diffuse = saturate(dot(normal, lightDirection)) * 0.8 + 0.2;
    return float4(input.baseColor.rgb * diffuse, input.baseColor.a);
}
