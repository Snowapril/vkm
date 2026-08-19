// Copyright (c) 2025 Snowapril
//
// Draws the captured camera frame behind everything else, as one oversized clip-space triangle
// generated from SV_VertexID -- no vertex buffer, no index buffer.
//
// The texture comes through descriptor set 2 rather than the bindless set, which is what lets
// this build on WebGPU: WGSL has no runtime-sized texture array for set 0 binding 0 to be. The
// set-2 path exists on every backend, so there is one shader rather than two.
//
// The image is mirrored horizontally. A camera pointed at its own user is expected to behave like
// a mirror: without the flip, moving a hand right moves the on-screen hand left and the whole
// interaction stops making sense. The pose is mirrored to match on the CPU side.

[[vk::binding(0, 2)]] Texture2D    g_CameraFrame : register(t0, space2);
[[vk::binding(1, 2)]] SamplerState g_Sampler     : register(s0, space2);

struct VSOutput
{
    float4 position : SV_POSITION;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
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
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    // The frame's own channel order is carried by the texture's format -- BGRA8_UNORM from an
    // AVFoundation capture, RGBA8 from a browser canvas -- so the sample never reorders bytes and
    // this shader never has to know which one it got.
    return float4(g_CameraFrame.Sample(g_Sampler, input.uv).rgb, 1.0);
}
