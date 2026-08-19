// Copyright (c) 2025 Snowapril
//
// Draws the captured camera frame behind everything else, as one oversized clip-space triangle
// generated from SV_VertexID -- no vertex buffer, no index buffer.
//
// The image is mirrored horizontally. A camera pointed at its own user is expected to behave
// like a mirror: without the flip, moving a hand right moves the on-screen hand left, and the
// whole interaction stops making sense.
//
// The texture slot reaches the fragment stage as a flat varying rather than through the push
// constants, because the push-constant range is vertex-only (see VkmPipelineStateVulkan
// ::createInner). It is uniform per draw, so it cannot be interpolated either.

#include "vkm_bindless.hlsli"

struct PushConstants
{
    uint cameraTextureSlot;
};

VKM_PUSH_CONSTANTS(PushConstants, g_PushConstants);
VKM_BINDLESS_TEXTURE_2D_ARRAY(g_BindlessTextures);
VKM_BINDLESS_SAMPLER(g_DefaultSampler);

struct VSOutput
{
    float4 position : SV_POSITION;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
    [[vk::location(1)]] nointerpolation uint cameraTextureSlot : TEXCOORD1;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    // The classic three-vertex fullscreen triangle: ids 0/1/2 give (0,0), (2,0), (0,2), whose
    // clip-space image covers the whole viewport with one primitive.
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);

    VSOutput output;
    // +Y up in clip space, the engine convention on every backend.
    output.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    output.uv = float2(1.0 - uv.x, uv.y);
    output.cameraTextureSlot = g_PushConstants.cameraTextureSlot;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    const float4 texel = g_BindlessTextures[input.cameraTextureSlot].Sample(g_DefaultSampler, input.uv);
    // The capture delivers BGRA and there is no BGRA VkmFormat to upload into, so the bytes go
    // into an RGBA texture as-is and the channel order is undone here. A free swizzle in the
    // fragment stage beats a per-frame byte swap over a megapixel on the CPU.
    return float4(texel.bgr, 1.0);
}
