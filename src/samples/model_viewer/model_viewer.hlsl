// Copyright (c) 2026 Snowapril
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
// VKM_LOAD_VERTEX a plain word load. The only per-permutation code is fetchNormal() and
// fetchTangent(); the PSO JSON "options" supply exactly one VKM_VERTEX_LAYOUT_* define per build.

#include "vkm_bindless.hlsli"
#include "vkm_material.hlsli"
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
    uint   debugMode;        // one of VKM_DEBUG_MODE_*, chosen by the sample
    uint2  _pad0;
};

// Debug visualisations debugMode selects. Mirrors DebugMode in the sample's main.cpp; the scene
// only carries the value, so these names live here rather than in the engine.
#define VKM_DEBUG_MODE_LIT            0
#define VKM_DEBUG_MODE_BASE_COLOR     1
#define VKM_DEBUG_MODE_MATERIAL_INDEX 2
#define VKM_DEBUG_MODE_NORMAL         3
#define VKM_DEBUG_MODE_TANGENT_NORMAL 4
#define VKM_DEBUG_MODE_STREAMING_MIP  5

// The pools are untyped word arrays, so the "vertex type" is a single u32.
VKM_BINDLESS_VERTEX_PULLING(uint);
VKM_BINDLESS_OBJECT_DATA(ObjectData, g_VkmObjectData);
VKM_BINDLESS_FRAME_DATA(FrameData, g_VkmSceneFrame);
VKM_MATERIAL_DECLARE();
VKM_FRAME_CONSTANTS(g_VkmFrame);

struct VSOutput
{
    float4 position : SV_POSITION;
    [[vk::location(0)]] float3 normal : NORMAL0;
    [[vk::location(1)]] float2 uv : TEXCOORD1;
    // World-space tangent, w = bitangent sign. Zero when the layout or the asset has no tangent.
    [[vk::location(2)]] float4 tangent : TANGENT0;
    // Push constants are vertex-stage only, so the debug views carry what they need as
    // interpolants; the material index is an integer and must not be interpolated.
    [[vk::location(3)]] nointerpolation uint materialIndex : TEXCOORD0;
};

float3 loadFloat3(uint slot, uint wordBase)
{
    return float3(asfloat(VKM_LOAD_VERTEX(slot, wordBase + 0)),
                  asfloat(VKM_LOAD_VERTEX(slot, wordBase + 1)),
                  asfloat(VKM_LOAD_VERTEX(slot, wordBase + 2)));
}

float4 loadFloat4(uint slot, uint wordBase)
{
    return float4(loadFloat3(slot, wordBase), asfloat(VKM_LOAD_VERTEX(slot, wordBase + 3)));
}

// Unpacks a snorm8x4 word, the inverse of meshopt_quantizeSnorm(v, 8).
float4 unpackSnorm8x4(uint packed)
{
    const int4 signedBytes = int4(packed << 24, packed << 16, packed << 8, packed) >> 24;
    return max(float4(signedBytes) / 127.0, -1.0);
}

// One object's vertex `index` in its pool, in that pool's layout.
float3 fetchNormal(ObjectData obj, uint wordBase)
{
#if defined(VKM_VERTEX_LAYOUT_STANDARD_PBR)
    return loadFloat3(obj.vertexPoolSlot, wordBase + 4); // normal at byte 16 == word 4
#elif defined(VKM_VERTEX_LAYOUT_COMPACT)
    return unpackSnorm8x4(VKM_LOAD_VERTEX(obj.vertexPoolSlot, wordBase + 3)).xyz; // normal at byte 12 == word 3
#else
    // PositionOnly carries no shading data; a constant normal keeps the geometry visible.
    return float3(0.0, 0.0, 1.0);
#endif
}

/*
* One object's vertex tangent, xyz plus the bitangent sign in w.
*
* Zero whenever there is no tangent to fetch: PositionOnly has no such attribute, and the glTF
* importer leaves tangents zeroed for assets that ship none. The tangent debug view flags that
* rather than drawing a frame built from nothing.
*/
float4 fetchTangent(ObjectData obj, uint wordBase)
{
#if defined(VKM_VERTEX_LAYOUT_STANDARD_PBR)
    return loadFloat4(obj.vertexPoolSlot, wordBase + 12); // tangent at byte 48 == word 12
#elif defined(VKM_VERTEX_LAYOUT_COMPACT)
    return unpackSnorm8x4(VKM_LOAD_VERTEX(obj.vertexPoolSlot, wordBase + 5)); // tangent at byte 20 == word 5
#else
    return float4(0.0, 0.0, 0.0, 0.0);
#endif
}

float2 fetchUV(uint slot, uint wordBase)
{
    // See gbuffer.hlsl's fetchUV -- same layouts, same offsets.
#if defined(VKM_VERTEX_LAYOUT_STANDARD_PBR)
    return float2(asfloat(VKM_LOAD_VERTEX(slot, wordBase + 8)),
                  asfloat(VKM_LOAD_VERTEX(slot, wordBase + 9)));
#elif defined(VKM_VERTEX_LAYOUT_COMPACT)
    const uint packed = VKM_LOAD_VERTEX(slot, wordBase + 4);
    return float2(f16tof32(packed & 0xFFFFu), f16tof32(packed >> 16));
#else
    return float2(0.0, 0.0);
#endif
}

VSOutput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    const ObjectData obj = g_VkmObjectData[instanceId];

    const uint index = VKM_LOAD_INDEX(obj.indexPoolSlot, obj.indexOffset + vertexId);
    const uint wordBase = obj.vertexWordOffset + index * VERTEX_STRIDE_WORDS;

    const float3 position = loadFloat3(obj.vertexPoolSlot, wordBase);
    const float3 normal = fetchNormal(obj, wordBase);
    const float4 tangent = fetchTangent(obj, wordBase);

    VSOutput output;
    // Two mat4 x vec4 products rather than folding VP*M per draw: the camera is shared by every
    // draw, so it stays in set 1 and only the model matrix comes from the object record.
    output.position = mul(g_VkmFrame.viewProjection, mul(obj.worldTransform, float4(position, 1.0)));
    // Shading is world-space now that there are no per-draw push constants to carry an
    // object-space light in; normalTransform is the inverse-transpose of worldTransform.
    output.normal = mul((float3x3)obj.normalTransform, normal);
    // A tangent is a direction along the surface, so it rides the world matrix itself rather than
    // its inverse-transpose; the handedness sign is a scalar and passes through untouched.
    output.tangent = float4(mul((float3x3)obj.worldTransform, tangent.xyz), tangent.w);
    // The base colour is resolved in the pixel stage now that it can be textured: a texture
    // sample needs the interpolated UV, and interpolating an already-sampled colour would
    // flatten every material to one value per vertex.
    output.uv = fetchUV(obj.vertexPoolSlot, wordBase);
    output.materialIndex = obj.materialIndex;
    return output;
}

// Marks data a debug view needs but the asset does not carry.
static const float3 kMissingDataColor = float3(1.0, 0.0, 1.0);

// A distinct, reasonably bright colour per index, from a cheap integer hash. Only adjacent
// indices need to be told apart, so a hash beats maintaining a palette.
float3 debugIndexColor(uint index)
{
    const uint hash = index * 2654435761u;
    const float3 channels = float3(uint3(hash, hash >> 8, hash >> 16) & 255u) / 255.0;
    return 0.25 + 0.75 * channels;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    const float3 normal = normalize(input.normal);
    const uint debugMode = g_VkmSceneFrame[0].debugMode;

    const VkmMaterial material = vkmLoadMaterial(g_VkmSceneFrame[0].materialPoolSlot, input.materialIndex);
    const float4 baseColor = vkmSampleBaseColor(material, input.uv);

    if (debugMode == VKM_DEBUG_MODE_BASE_COLOR)
    {
        return float4(baseColor.rgb, 1.0);
    }
    if (debugMode == VKM_DEBUG_MODE_MATERIAL_INDEX)
    {
        return float4(debugIndexColor(input.materialIndex), 1.0);
    }
    if (debugMode == VKM_DEBUG_MODE_STREAMING_MIP)
    {
        // The base-colour texture's resident level, which is the one the record carries.
        const float2 streamingMip =
            vkmLoadMaterialStreamingMip(g_VkmSceneFrame[0].materialPoolSlot, input.materialIndex);
        return float4(vkmStreamingMipHeatColor(streamingMip), 1.0);
    }
    if (debugMode == VKM_DEBUG_MODE_NORMAL)
    {
        return float4(normal * 0.5 + 0.5, 1.0);
    }
    if (debugMode == VKM_DEBUG_MODE_TANGENT_NORMAL)
    {
        // Materials carry no normal map yet, so what goes through the basis is a flat tangent-space
        // normal: the result equals the world normal exactly when the frame is well formed, which
        // is what makes a missing or broken one stand out against VKM_DEBUG_MODE_NORMAL.
        if (dot(input.tangent.xyz, input.tangent.xyz) < 1e-8)
        {
            return float4(kMissingDataColor, 1.0);
        }
        // Gram-Schmidt: interpolation leaves the tangent neither unit length nor perpendicular.
        const float3 tangent = normalize(input.tangent.xyz - normal * dot(normal, input.tangent.xyz));
        const float3 bitangent = cross(normal, tangent) * input.tangent.w;
        const float3x3 tbn = float3x3(tangent, bitangent, normal);
        return float4(normalize(mul(float3(0.0, 0.0, 1.0), tbn)) * 0.5 + 0.5, 1.0);
    }

    const float3 lightDirection = normalize(g_VkmSceneFrame[0].lightDirection.xyz);

    // Half-Lambert wrap so backfacing-but-visible geometry stays readable instead of black.
    const float diffuse = saturate(dot(normal, lightDirection)) * 0.8 + 0.2;
    return float4(baseColor.rgb * diffuse, baseColor.a);
}
