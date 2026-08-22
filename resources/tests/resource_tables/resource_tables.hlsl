// Copyright (c) 2026 Snowapril
//
// Exercises the two PSO-declared descriptor sets end to end, with nothing from sets 0 or 1
// involved.
//
// CSMain covers the pair *together*: a uniform buffer at set 2, a uniform buffer at set 3, and a
// storage buffer at set 2 that receives a value derived from both. Two sets that alias onto one
// another -- the failure mode Metal invites, since it has no set index and separates them only by
// argument-table index bases -- produce wrong values here rather than plausible ones.
//
// CSPerDrawOnly covers a PSO that declares set 3 and NOT set 2. That is the shape a G-buffer pass
// wanting only per-material textures has, and it is what forces the backends to keep set 3 at set
// index 3 (via a placeholder layout at 2) instead of sliding it down to the next free slot.
//
// Writing `base + threadId` rather than a constant means a table bound at the wrong index, or a
// buffer bound at the wrong binding, is visible as wrong values.
//
// space2/space3 match VkmTableResourceType's documented register spaces; the vk::binding
// attributes pin the same (binding, set) pairs the PSO JSON declares.

// Padded with scalars rather than a uint3, because the two layouts disagree about that. HLSL packs
// a uint3 into the remaining three slots of the row the uint started, giving 16 bytes; WGSL gives
// vec3<u32> an alignment of 16, pushing it to offset 16 and the struct to 32. Scalars are 4-aligned
// in both, so this is 16 bytes everywhere -- which is also what the C++ mirror declares.
struct TableConstants
{
    uint base;
    uint _pad0;
    uint _pad1;
    uint _pad2;
};

[[vk::binding(0, 2)]] ConstantBuffer<TableConstants> g_Pass : register(b0, space2);
[[vk::binding(1, 2)]] RWStructuredBuffer<uint>       g_Out  : register(u0, space2);

[[vk::binding(0, 3)]] ConstantBuffer<TableConstants> g_Draw : register(b0, space3);

[numthreads(64, 1, 1)]
void CSMain(uint3 threadId : SV_DispatchThreadID)
{
    // Both sets contribute, so neither can be dropped or aliased onto the other unnoticed.
    g_Out[threadId.x] = g_Pass.base + g_Draw.base + threadId.x;
}
