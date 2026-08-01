// Copyright (c) 2025 Snowapril
//
// The fullscreen pass building block: a vertex shader that needs no vertex buffer, no index
// buffer and no input layout, so a PSO using it declares none.
//
// One oversized triangle rather than two quad triangles. Two triangles meet along a diagonal, and
// quad-based derivatives are wrong for the pixels straddling it -- a real artifact for any pass
// that uses ddx/ddy, and free to avoid.

#ifndef VKM_FULLSCREEN_HLSLI
#define VKM_FULLSCREEN_HLSLI

struct VkmFullscreenVSOutput
{
    float4 position : SV_POSITION;
    [[vk::location(0)]] float2 uv : TEXCOORD0;
};

/*
* @brief Covers the viewport from the vertex index alone; draw with vertexCount = 3.
*
* vertexId 0,1,2 give UVs (0,0), (2,0), (0,2), whose clip-space corners span twice the viewport.
* The half outside is clipped, leaving full coverage with no interior edge.
*
* UV is +Y down while the engine's clip space is +Y up on every backend (see camera.h), hence the
* sign on y.
*/
VkmFullscreenVSOutput vkmFullscreenTriangle(uint vertexId)
{
    VkmFullscreenVSOutput output;
    output.uv = float2((vertexId << 1) & 2, vertexId & 2);
    output.position = float4(output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

#endif // VKM_FULLSCREEN_HLSLI
