// Copyright (c) 2026 Snowapril
//
// Draws one wireframe box per top-level acceleration structure instance, so an instance's
// position, orientation and extent are visible against the geometry it bounds.
//
// One instanced line-list draw of 24 vertices: the box's 12 edges, two endpoints each. The box
// arrives pre-transformed as a corner and three edge vectors (vkm::VkmAsDebugBox), so this does
// no matrix work beyond the view projection, and a rotated instance draws a rotated box rather
// than the inflated axis-aligned one a re-fitted AABB would give.
//
// No depth test. An instance buried inside geometry is the case this view exists to find, and
// depth testing would hide exactly those.

#include "vkm_frame_constants.hlsli"

VKM_FRAME_CONSTANTS(g_VkmFrame);

// Mirrors vkm::VkmAsDebugBox (renderer/acceleration_structure_debug.h).
struct AsDebugBox
{
    float4 origin;  // xyz = the (min, min, min) corner in world space, w = 1.0 when selected
    float4 edgeX;   // xyz = world vector spanning the box's local +X
    float4 edgeY;
    float4 edgeZ;
};

// Mirrors vkm::kVkmAsDebugMaxBoxes.
#define VKM_AS_DEBUG_MAX_BOXES 256

struct AsDebugBoxes
{
    AsDebugBox boxes[VKM_AS_DEBUG_MAX_BOXES];
};

[[vk::binding(0, 2)]] ConstantBuffer<AsDebugBoxes> g_Boxes : register(b0, space2);

struct VSOutput
{
    float4 position : SV_POSITION;
    [[vk::location(0)]] nointerpolation float selected : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID)
{
    // A corner index is a 3-bit mask over the box's own axes: bit 0 = X, bit 1 = Y, bit 2 = Z.
    // The 12 edges are the four of the Y = min face, the four of the Y = max face, and the four
    // uprights joining them.
    const uint corners[24] = {
        0, 1,  1, 5,  5, 4,  4, 0,
        2, 3,  3, 7,  7, 6,  6, 2,
        0, 2,  1, 3,  4, 6,  5, 7,
    };
    const uint corner = corners[vertexId];

    const AsDebugBox box = g_Boxes.boxes[instanceId];
    const float3 world = box.origin.xyz +
                         box.edgeX.xyz * float((corner     ) & 1u) +
                         box.edgeY.xyz * float((corner >> 1) & 1u) +
                         box.edgeZ.xyz * float((corner >> 2) & 1u);

    VSOutput output;
    // The jitter-free matrix: this draws over the tone-mapped image at display extent, where the
    // render extent's sub-pixel jitter does not apply.
    output.position = mul(g_VkmFrame.viewProjectionNoJitter, float4(world, 1.0));
    output.selected = box.origin.w;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET0
{
    // The inspector's selection colour, and the Spatial tab's unselected outline.
    const float3 selectedColor = float3(1.0, 0.784, 0.353);
    const float3 normalColor   = float3(0.588, 0.745, 0.863);

    const bool selected = input.selected > 0.5;
    // The unselected boxes blend back so a dense scene stays readable through them; the selected
    // one is opaque.
    return float4(selected ? selectedColor : normalColor, selected ? 1.0 : 0.65);
}
