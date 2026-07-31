// Copyright (c) 2025 Snowapril
//
// Exercises descriptor set 2 (per-pass resources) end to end: a uniform buffer read from set 2
// and a storage buffer written to it, with nothing from sets 0 or 1 involved. Writing
// `base + threadId` rather than a constant means a table bound at the wrong index, or a buffer
// bound at the wrong binding, produces wrong values rather than plausible ones.
//
// space2 matches VkmPerPassResourceType's documented register spaces; the vk::binding attributes
// pin the same (binding, set) pairs the PSO JSON declares.

struct PassConstants
{
    uint base;
    uint3 _pad0;
};

[[vk::binding(0, 2)]] ConstantBuffer<PassConstants> g_Pass : register(b0, space2);
[[vk::binding(1, 2)]] RWStructuredBuffer<uint>      g_Out  : register(u0, space2);

[numthreads(64, 1, 1)]
void CSMain(uint3 threadId : SV_DispatchThreadID)
{
    g_Out[threadId.x] = g_Pass.base + threadId.x;
}
