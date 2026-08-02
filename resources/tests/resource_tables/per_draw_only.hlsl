// Copyright (c) 2025 Snowapril
//
// The companion to resource_tables.hlsl: a compute shader whose only resources live in descriptor
// set 3, with NO set 2 declared at all. That is the shape a G-buffer pass wanting only per-material
// textures has, and it is what forces every backend to keep set 3 at set index 3 -- via a
// placeholder layout at index 2 -- instead of sliding it down to the next free slot. A backend that
// slid it down reads nothing here, so the output is zeroes rather than `base + threadId`.
//
// A separate file rather than a second entry point in resource_tables.hlsl: a shader cache file is
// named <shader>[<option>].<stage>.<backend> and carries no entry point, so two PSOs sharing one
// HLSL file and option name silently overwrite each other's cache -- which is exactly what this
// test hit while being written.

// Scalar padding rather than a uint3; see the note in resource_tables.hlsl.
struct TableConstants
{
    uint base;
    uint _pad0;
    uint _pad1;
    uint _pad2;
};

[[vk::binding(0, 3)]] ConstantBuffer<TableConstants> g_Draw    : register(b0, space3);
[[vk::binding(1, 3)]] RWStructuredBuffer<uint>       g_DrawOut : register(u0, space3);

[numthreads(64, 1, 1)]
void CSMain(uint3 threadId : SV_DispatchThreadID)
{
    g_DrawOut[threadId.x] = g_Draw.base + threadId.x;
}
