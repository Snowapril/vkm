// Copyright (c) 2026 Snowapril
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
#include "vkm_punctual_lights.hlsli"
#include "vkm_shadow.hlsli"

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
    uint     vertexStrideWords; // read only by the ray-tracing kernels; a draw knows its own layout
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

// Mirrors vkm::VkmProbeCaptureConstants (renderer/probe_volume.h). Built for a probe at the
// origin, and shared by every probe: this shader works in probe-relative space, so the probe's
// position cancels out of the matrices (see the struct's comment for the algebra).
struct ProbeCaptureConstants
{
    float4x4 faceViewProjection[6];
    // The sun as a light record rather than a bare direction: the shadow lookup takes one, and
    // its shadowTile is only known once VkmShadowAtlas has assigned tiles.
    VkmPunctualLight sun;
    // x = shadow atlas tiles per row, y = tile size in texels. Either zero disables the lookup,
    // which is what an updater with no atlas leaves in place.
    uint4 shadowParams;
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
[[vk::binding(1, 2)]] Texture2D    g_ShadowAtlas   : register(t0, space2);
[[vk::binding(2, 2)]] SamplerState g_ShadowSampler : register(s0, space2);
[[vk::binding(3, 2)]] ConstantBuffer<VkmShadowAtlasConstants> g_ShadowAtlasConstants : register(b1, space2);

VKM_SHADOW_LOADER(g_ShadowAtlas, g_ShadowSampler, g_ShadowAtlasConstants);

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
    // The probe position the offsets above are relative to. Carried as a varying for the same
    // reason they are relative in the first place: the fragment stage cannot read the push
    // constant that holds it. Constant across the primitive, so it interpolates to itself.
    [[vk::location(4)]] nointerpolation float3 probePositionWorld : TEXCOORD3;
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
    output.probePositionWorld = g_Face.probePositionWorld;
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
    const float4 sampledBaseColor = vkmSampleBaseColor(material, input.uv);
    // Same alpha-mask test the G-buffer applies: a probe that captures an unmasked leaf card
    // records a dark wall where there is open air, and hands that to every lookup around it.
    if (sampledBaseColor.a < material.alphaCutoff)
    {
        discard;
    }
    const float3 baseColor = sampledBaseColor.rgb;
    const float nDotL = saturate(dot(normal, normalize(g_FrameData[0].lightDirection.xyz)));

    // Shadowed, and this is the term that decides whether the probe tier over-reports indoors: a
    // probe that captures a sunlit wall it cannot actually see the sun from hands that brightness
    // to every surface around it. The lookup wants a world position and this shader works in
    // probe-relative space, so the probe's own position -- already pushed per draw -- puts it
    // back. Zero shadowParams skips the lookup entirely, for a capture with no atlas.
    float visibility = 1.0;
    if (g_Capture.shadowParams.x > 0u && g_Capture.shadowParams.y > 0u)
    {
        const float3 worldPosition = input.probeRelativePosition + input.probePositionWorld;
        visibility = vkmShadowFactor(g_Capture.sun, worldPosition, geometricNormal, nDotL,
                                     g_Capture.shadowParams.x, g_Capture.shadowParams.y);
    }

    // Lambert only. A probe records low-frequency incoming radiance, and the specular lobe of a
    // surface the probe happens to see says nothing useful about the directions it will be asked
    // about later.
    const float3 radiance = baseColor * nDotL * visibility;

    return float4(radiance, length(input.probeRelativePosition));
}
