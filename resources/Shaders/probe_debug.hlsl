// Copyright (c) 2025 Snowapril
//
// Draws the probe volume as shaded spheres, one per probe, so a probe's position and what it
// stores are both visible while it is being placed by hand.
//
// One instanced draw of six vertices: each instance is a screen-facing quad, and the fragment
// shader carves a sphere out of it and shades that sphere with the probe's own octahedral
// irradiance. A real mesh would need a vertex buffer and would show the same thing.
//
// No depth test. A probe buried inside a wall is the case this view exists to find, and depth
// testing would hide exactly those. The cost is that overlapping probes resolve by draw order
// rather than by distance, which at a probe's on-screen size is a few pixels along a silhouette.

#include "vkm_bindless.hlsli"
#include "vkm_frame_constants.hlsli"
#include "vkm_probe_volume.hlsli"

VKM_FRAME_CONSTANTS(g_VkmFrame);

// Mirrors vkm::VkmProbeDebugPushConstants (renderer/probe_volume.h).
struct ProbeDebugPushConstants
{
    float radius;        // sphere radius in world units
    uint  selectedProbe; // linear index drawn highlighted; anything out of range selects none
};

VKM_PUSH_CONSTANTS(ProbeDebugPushConstants, g_Debug);

[[vk::binding(0, 2)]] Texture2D    g_ProbeOffsets : register(t0, space2);
[[vk::binding(1, 2)]] Texture2D    g_Irradiance   : register(t1, space2);
[[vk::binding(2, 2)]] SamplerState g_Sampler      : register(s0, space2);
[[vk::binding(3, 2)]] ConstantBuffer<VkmProbeVolumeConstants> g_Volume : register(b0, space2);

// The push constants reach the fragment stage through the vertex stage, as probe_blend.hlsl
// explains: Vulkan declares the range for the vertex and compute stages only.
struct VSOutput
{
    float4 position : SV_POSITION;
    // Position within the impostor quad, -1 to 1 on each axis: the sphere is the unit disc of it.
    [[vk::location(0)]] float2 quadCoord : TEXCOORD0;
    [[vk::location(1)]] nointerpolation uint3 probeCoord : TEXCOORD1;
    [[vk::location(2)]] nointerpolation uint selected : TEXCOORD2;
};

// The camera's world-space basis. The view matrix takes world to view under this engine's
// mul(M, v) convention, so its rows are the axes expressed in world space.
float3 cameraRightWorld() { return float3(g_VkmFrame.view._11, g_VkmFrame.view._12, g_VkmFrame.view._13); }
float3 cameraUpWorld()    { return float3(g_VkmFrame.view._21, g_VkmFrame.view._22, g_VkmFrame.view._23); }
// View +Z points away from what the camera looks at, so toward the eye is the row itself.
float3 cameraTowardEyeWorld() { return float3(g_VkmFrame.view._31, g_VkmFrame.view._32, g_VkmFrame.view._33); }

VSOutput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    const float2 corners[6] = {
        float2(-1.0, -1.0), float2( 1.0, -1.0), float2(-1.0,  1.0),
        float2(-1.0,  1.0), float2( 1.0, -1.0), float2( 1.0,  1.0),
    };
    const float2 quadCoord = corners[vertexId];

    const uint3 counts = g_Volume.probeCounts.xyz;
    const uint3 probeCoord = uint3(instanceId % counts.x,
                                   (instanceId / counts.x) % counts.y,
                                   instanceId / (counts.x * counts.y));

    const int2 offsetTexel = int2(probeCoord.y * counts.x + probeCoord.x, probeCoord.z);
    const float3 probeOffset = g_ProbeOffsets.Load(int3(offsetTexel, 0)).xyz;
    const float3 probePosition = vkmProbeGridOrigin(g_Volume) +
                                 float3(probeCoord) * vkmProbeGridSpacing(g_Volume) + probeOffset;

    const float3 worldPosition = probePosition +
                                 cameraRightWorld() * quadCoord.x * g_Debug.radius +
                                 cameraUpWorld() * quadCoord.y * g_Debug.radius;

    VSOutput output;
    output.position = mul(g_VkmFrame.viewProjection, float4(worldPosition, 1.0));
    output.quadCoord = quadCoord;
    output.probeCoord = probeCoord;
    output.selected = (instanceId == g_Debug.selectedProbe) ? 1u : 0u;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET0
{
    const float radiusSquared = dot(input.quadCoord, input.quadCoord);
    if (radiusSquared > 1.0)
    {
        discard;
    }

    // The visible hemisphere of a unit sphere, in camera space, lifted back into world space.
    const float towardEye = sqrt(max(0.0, 1.0 - radiusSquared));
    const float3 normal = cameraRightWorld() * input.quadCoord.x +
                          cameraUpWorld() * input.quadCoord.y +
                          cameraTowardEyeWorld() * towardEye;

    const float2 uv = vkmProbeAtlasUv(input.probeCoord, g_Volume.probeCounts.xyz,
                                      g_Volume.atlasParams.x, vkmProbeDirectionToOctUv(normal));
    // A probe stores black until its first refresh, and a round takes probeCount/budget frames --
    // so most of the grid is black most of the time, and a black sphere against a dark scene is
    // not a visualization. The base shade keeps every probe's silhouette readable and its
    // falloff toward the rim is what makes the impostor read as a ball; stored irradiance adds
    // on top, so a lit probe is still brighter than an unlit one.
    const float base = 0.10 + 0.30 * towardEye;
    float3 color = g_Irradiance.SampleLevel(g_Sampler, uv, 0).rgb + base;

    if (input.selected != 0u)
    {
        // A rim rather than a tint, so the probe's stored irradiance stays readable while it is
        // the one being moved.
        color = lerp(color, float3(1.0, 0.45, 0.1), smoothstep(0.55, 1.0, radiusSquared));
    }
    return float4(color, 1.0);
}
