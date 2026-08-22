// Copyright (c) 2026 Snowapril
//
// Fills one tile of the shadow atlas: the scene, drawn from a light, storing how far the nearest
// surface is from that light.
//
// Every tile of every light shares ONE render pass, exactly as probe_capture shares one pass
// across a frame's probes and their six cube faces. The caller sets a viewport per tile and
// pushes which tile is being drawn; the vertex shader picks that tile's view-projection out of
// set 2. That is not a stylistic echo -- it is the only shape available, because the engine has
// no layered rendering: no attachment descriptor carries a slice or face index, Vulkan pins
// layerCount to 1, and there is no SV_RenderTargetArrayIndex or geometry stage. A cascade per
// array slice or a cube face per pass cannot be expressed.
//
// The output is LINEAR distance from the light, in world units, not post-projection depth:
//
//   - a point light's six faces then share one comparison, since distance does not depend on
//     which face a query lands in;
//   - the lookup's bias is in world units, which is what lets one bias constant work on a
//     centimetre model and on a 3721-unit Sponza alike;
//   - and it sidesteps sampling a depth texture, which WebGPU cannot do here (its bind-group
//     layout hardcodes a filterable float sample type) and which gbuffer.h has already recorded
//     the engine's position against.
//
// A colour target rather than depth-only also costs nothing extra on Metal, which disables
// rasterization outright for a pipeline with no fragment shader -- so a fragment shader had to
// exist regardless, and the alpha-mask discard below needs one anyway.

#include "vkm_bindless.hlsli"
#include "vkm_material.hlsli"
#include "vkm_shadow.hlsli"

// Mirrors vkm::VkmObjectData.
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

// Mirrors vkm::VkmFrameData.
struct FrameData
{
    float4 frustumPlanes[6];
    float4 lightDirection;
    uint   materialPoolSlot;
    uint   debugMode;
    uint2  _pad0;
};

// Mirrors vkm::VkmShadowTilePushConstants.
struct TilePushConstants
{
    uint tileIndex;
    // Which cull view's FrameData this pass published. Every other scene shader reads
    // g_FrameData[0] because the fields it wants are view-invariant AND the camera always
    // publishes view 0 first -- an assumption that holds in a full frame and silently fails
    // anywhere else, which is exactly how this pass first rendered nothing at all: with only its
    // own view published, materialPoolSlot read garbage and the alpha test discarded every
    // fragment. Indexing by the pass's own view costs one push word and removes the coupling.
    uint frameDataIndex;
};

VKM_PUSH_CONSTANTS(TilePushConstants, g_Tile);
VKM_BINDLESS_VERTEX_PULLING(uint);
VKM_BINDLESS_OBJECT_DATA(ObjectData, g_ObjectData);
VKM_BINDLESS_FRAME_DATA(FrameData, g_FrameData);
VKM_MATERIAL_DECLARE();

[[vk::binding(0, 2)]] ConstantBuffer<VkmShadowAtlasConstants> g_Atlas : register(b0, space2);

// Words per vertex, per layout preset: 64 B, 32 B and 16 B (see VkmVertexLayoutPreset). These
// must match probe_capture.hlsl and gbuffer.hlsl exactly -- a wrong stride reads a vertex from
// the wrong address, which renders nothing at all rather than something visibly wrong.
#if defined(VKM_VERTEX_LAYOUT_STANDARD_PBR)
    #define VERTEX_STRIDE_WORDS 16
#elif defined(VKM_VERTEX_LAYOUT_COMPACT)
    #define VERTEX_STRIDE_WORDS 8
#elif defined(VKM_VERTEX_LAYOUT_POSITION_ONLY)
    #define VERTEX_STRIDE_WORDS 4
#else
    #error "shadow_depth.hlsl requires a VKM_VERTEX_LAYOUT_* permutation define"
#endif

struct VSOutput
{
    float4 position : SV_POSITION;
    // The light-relative POSITION, not the distance. Position is linear across a triangle and
    // distance is not, so interpolating a per-vertex distance is wrong wherever the triangle is
    // large compared to its distance from the light. The fixture that gates this pass makes the
    // error concrete: its three vertices are all equidistant from the light, so the interpolated
    // value is constant across the whole triangle while the true distance dips by 1.5% under the
    // light. The length is therefore taken per fragment, exactly as probe_capture.hlsl does.
    [[vk::location(0)]] float3 lightRelativePosition : TEXCOORD0;
    [[vk::location(1)]] float2 uv : TEXCOORD1;
    [[vk::location(2)]] nointerpolation uint materialIndex : TEXCOORD2;
    // Which tile this fragment belongs to, so the fragment stage can pick the same light the
    // vertex stage did without re-reading a push constant Vulkan does not give it.
    [[vk::location(3)]] nointerpolation uint tileIndex : TEXCOORD3;
};

float3 loadFloat3(uint slot, uint wordBase)
{
    return float3(asfloat(VKM_LOAD_VERTEX(slot, wordBase + 0)),
                  asfloat(VKM_LOAD_VERTEX(slot, wordBase + 1)),
                  asfloat(VKM_LOAD_VERTEX(slot, wordBase + 2)));
}

// See gbuffer.hlsl's fetchUV -- same layouts, same offsets.
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
    const float4 worldPosition = mul(obj.worldTransform, float4(position, 1.0));

    const uint tile = g_Tile.tileIndex;

    VSOutput output;
    output.position = mul(g_Atlas.tileViewProjection[tile], worldPosition);
    output.lightRelativePosition = worldPosition.xyz - g_Atlas.tileLightPosition[tile].xyz;
    output.uv = fetchUV(obj, wordBase);
    output.materialIndex = obj.materialIndex;
    output.tileIndex = tile;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET0
{
    // The same alpha-mask test the G-buffer and the probe capture apply. Without it a leaf card
    // casts the shadow of its whole quad, which is the most visible way a masked material goes
    // wrong -- and unlike the colour passes, here it would be a hard black rectangle on the floor.
    const VkmMaterial material = vkmLoadMaterial(g_FrameData[g_Tile.frameDataIndex].materialPoolSlot, input.materialIndex);
    if (vkmSampleBaseColor(material, input.uv).a < material.alphaCutoff)
    {
        discard;
    }

    // A positional light measures radially; a directional one measures along its axis, so the
    // stored value matches what a lookup can reconstruct without knowing a light position.
    const float4 lightPosition = g_Atlas.tileLightPosition[input.tileIndex];
    const float lightDistance =
        lightPosition.w > 0.5
            ? length(input.lightRelativePosition)
            : dot(input.lightRelativePosition, normalize(g_Atlas.tileLightDirection[input.tileIndex].xyz));

    // Only .r is read. The other channels keep the target a plain RGBA16F colour attachment,
    // which is the format every backend already renders to here.
    return float4(lightDistance, 0.0, 0.0, 1.0);
}
