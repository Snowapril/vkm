// Copyright (c) 2025 Snowapril
//
// Exercises descriptor set 2 (per-pass resources) end to end: a uniform buffer read from set 2
// and a storage buffer written to it, with nothing from sets 0 or 1 involved. Writing
// `base + threadId` rather than a constant means a table bound at the wrong index, or a buffer
// bound at the wrong binding, produces wrong values rather than plausible ones.
//
// space2 matches VkmPerPassResourceType's documented register spaces; the vk::binding attributes
// pin the same (binding, set) pairs the PSO JSON declares.

// Padded with scalars rather than a uint3, because the two layouts disagree about that. HLSL packs
// a uint3 into the remaining three slots of the row the uint started, giving 16 bytes; WGSL gives
// vec3<u32> an alignment of 16, pushing it to offset 16 and the struct to 32. Scalars are 4-aligned
// in both, so this is 16 bytes everywhere -- which is also what the C++ mirror declares.
struct PassConstants
{
    uint base;
    uint _pad0;
    uint _pad1;
    uint _pad2;
};

[[vk::binding(0, 2)]] ConstantBuffer<PassConstants> g_Pass : register(b0, space2);
[[vk::binding(1, 2)]] RWStructuredBuffer<uint>      g_Out  : register(u0, space2);

[numthreads(64, 1, 1)]
void CSMain(uint3 threadId : SV_DispatchThreadID)
{
    g_Out[threadId.x] = g_Pass.base + threadId.x;
}
