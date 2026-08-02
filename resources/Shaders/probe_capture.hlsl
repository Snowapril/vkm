// Copyright (c) 2025 Snowapril
//
// Renders the scene from one probe, into the six cube faces of a single capture target.
//
// All six faces share one render pass -- and so do all of a frame's probes. The caller sets a
// viewport per (probe, face) tile and pushes which probe and which face is being drawn, so the
// vertex shader can pick that face's view-projection out of set 2. Without viewport control this
// would be six render passes per probe, on a grid of thousands -- which is why
// setViewportAndScissor exists.
//
// The output is radiance, not a G-buffer. A probe stores what it *saw*, so shading happens here,
// forward, with the same directional light the scene carries. That makes the probes' contribution
// one bounce of indirect light for the main view.
//
// Distance rides in alpha rather than a second target. The conversion pass needs it to build the
// mean and mean-squared moments the Chebyshev test uses, and a probe capture is small enough that
// half-float distance precision is not the limiting factor.

#include "vkm_bindless.hlsli"
#include "vkm_material.hlsli"

// Deliberately not scene_common.hlsli: that header also declares the compute-only read-write
// singletons, which a render pipeline must never declare (see vkm_bindless.hlsli).

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
    uint2    _pad0;
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

// Mirrors vkm::VkmProbeCaptureConstants (renderer/probe_volume.h). Built for a probe at the
// origin, and shared by every probe: this shader works in probe-relative space, so the probe's
// position cancels out of the matrices (see the struct's comment for the algebra).
struct ProbeCaptureConstants
{
    float4x4 faceViewProjection[6];
};

// Mirrors vkm::VkmProbeCapturePushConstants. Everything genuinely per-probe fits in 16 bytes,
// which is why a whole frame's probes share one constant buffer and one resource table.
struct FacePushConstants
{
    float3 probePositionWorld;
    uint   faceIndex;
};

VKM_PUSH_CONSTANTS(FacePushConstants, g_Face);
VKM_BINDLESS_VERTEX_PULLING(uint);
VKM_BINDLESS_OBJECT_DATA(ObjectData, g_ObjectData);
VKM_BINDLESS_FRAME_DATA(FrameData, g_FrameData);
VKM_MATERIAL_DECLARE();

[[vk::binding(0, 2)]] ConstantBuffer<ProbeCaptureConstants> g_Capture : register(b0, space2);

#if defined(VKM_VERTEX_LAYOUT_STANDARD_PBR)
    #define VERTEX_STRIDE_WORDS 16
#elif defined(VKM_VERTEX_LAYOUT_COMPACT)
    #define VERTEX_STRIDE_WORDS 8
#elif defined(VKM_VERTEX_LAYOUT_POSITION_ONLY)
    #define VERTEX_STRIDE_WORDS 4
#else
    #error "probe_capture.hlsl requires a VKM_VERTEX_LAYOUT_* permutation define"
#endif

struct VSOutput
{
    float4 position : SV_POSITION;
    // Relative to the probe, not the world: the pixel shader needs the distance to the probe and
    // a geometric normal, and both are invariant under the translation (a normal comes from
    // ddx/ddy, which a constant offset does not change). It also cannot read the push constant
    // holding the probe position -- Vulkan declares that range for the vertex stage only.
    [[vk::location(0)]] float3 probeRelativePosition : TEXCOORD0;
    [[vk::location(1)]] float3 worldNormal : NORMAL0;
    [[vk::location(2)]] nointerpolation uint materialIndex : TEXCOORD1;
    [[vk::location(3)]] float2 uv : TEXCOORD2;
};

float3 loadFloat3(uint slot, uint wordBase)
{
    return float3(asfloat(VKM_LOAD_VERTEX(slot, wordBase + 0)),
                  asfloat(VKM_LOAD_VERTEX(slot, wordBase + 1)),
                  asfloat(VKM_LOAD_VERTEX(slot, wordBase + 2)));
}

float4 unpackSnorm8x4(uint packed)
{
    const int4 signedBytes = int4(packed << 24, packed << 16, packed << 8, packed) >> 24;
    return max(float4(signedBytes) / 127.0, -1.0);
}

float3 fetchNormal(ObjectData obj, uint wordBase)
{
#if defined(VKM_VERTEX_LAYOUT_STANDARD_PBR)
    return loadFloat3(obj.vertexPoolSlot, wordBase + 4);
#elif defined(VKM_VERTEX_LAYOUT_COMPACT)
    return unpackSnorm8x4(VKM_LOAD_VERTEX(obj.vertexPoolSlot, wordBase + 3)).xyz;
#else
    return float3(0.0, 0.0, 0.0);
#endif
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
    const float3 probeRelative = worldPosition.xyz - g_Face.probePositionWorld;

    VSOutput output;
    output.position = mul(g_Capture.faceViewProjection[g_Face.faceIndex], float4(probeRelative, 1.0));
    output.probeRelativePosition = probeRelative;
    output.worldNormal = mul((float3x3)obj.normalTransform, fetchNormal(obj, wordBase));
    output.materialIndex = obj.materialIndex;
    output.uv = fetchUV(obj, wordBase);
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET0
{
    const float3 geometricNormal =
        normalize(cross(ddx(input.probeRelativePosition), ddy(input.probeRelativePosition)));
    const float3 normal = (dot(input.worldNormal, input.worldNormal) > 0.0)
                              ? normalize(input.worldNormal)
                              : geometricNormal;

    // Textured, not factor-only: a probe records the radiance leaving a surface, so sampling the
    // albedo here is what makes indirect colour bleeding carry the scene's actual colours instead
    // of a per-material average.
    const VkmMaterial material = vkmLoadMaterial(g_FrameData[0].materialPoolSlot, input.materialIndex);
    const float3 baseColor = vkmSampleBaseColor(material, input.uv).rgb;
    const float nDotL = saturate(dot(normal, normalize(g_FrameData[0].lightDirection.xyz)));

    // Lambert only. A probe records low-frequency incoming radiance, and the specular lobe of a
    // surface the probe happens to see says nothing useful about the directions it will be asked
    // about later.
    const float3 radiance = baseColor * nDotL;

    return float4(radiance, length(input.probeRelativePosition));
}
