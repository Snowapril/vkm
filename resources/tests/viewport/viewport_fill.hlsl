// Copyright (c) 2026 Snowapril
//
// Test fixture: a fullscreen triangle that writes one constant colour, so what a viewport does is
// visible purely as which texels changed.

#include "vkm_fullscreen.hlsli"

typedef VkmFullscreenVSOutput VSOutput;

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    return vkmFullscreenTriangle(vertexId);
}

float4 PSMain(VSOutput input) : SV_TARGET0
{
    return float4(1.0, 1.0, 1.0, 1.0);
}
