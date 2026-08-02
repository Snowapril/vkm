// Copyright (c) 2025 Snowapril
//
// Stateless hash-based RNG, seeded per (pixel, frame, pass).
//
// Stateless is the point: there is no per-pixel state to store, load or ping-pong, and any pass can
// reproduce the same stream by asking for the same seed. That matters more here than it looks --
// ReSTIR's validation modes replay a frame's sampling decisions, which is only possible if the
// stream is a pure function of what identifies the sample.
//
// The pass id is part of the seed because two passes running on the same pixel in the same frame
// must not draw the same numbers. A shader picks a constant for itself; the value only has to be
// distinct, not registered anywhere.

#ifndef VKM_RANDOM_HLSLI
#define VKM_RANDOM_HLSLI

/*
* @brief PCG hash: one uint in, one well-mixed uint out.
*
* One multiply, one xorshift and a final scramble. Chosen over a cheaper integer hash (Wang,
* xxhash32) because those leave visible structure in the low bits, which shows up as banding the
* moment the output drives a direction rather than a colour.
*/
uint vkmPcgHash(uint value)
{
    const uint state = value * 747796405u + 2891336453u;
    const uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// Seeds from what identifies a sample. Hashing the pixel coordinate first keeps neighbouring
// pixels from sharing a stream when the frame index is small.
uint vkmSeed(uint2 pixel, uint frameIndex, uint passId)
{
    return vkmPcgHash(vkmPcgHash(pixel.x + pixel.y * 65536u) + frameIndex * 9781u + passId * 26699u);
}

// Advances `state` and returns the next uint. Call repeatedly for a stream.
uint vkmRandomUint(inout uint state)
{
    state = vkmPcgHash(state);
    return state;
}

// Next float in [0, 1). 24 bits of mantissa is all a float carries, so the low 8 are dropped
// rather than rounded -- taking them would only re-introduce the low-bit structure the hash exists
// to remove.
float vkmRandomFloat(inout uint state)
{
    return float(vkmRandomUint(state) >> 8u) * (1.0 / 16777216.0);
}

float2 vkmRandomFloat2(inout uint state)
{
    const float x = vkmRandomFloat(state);
    const float y = vkmRandomFloat(state);
    return float2(x, y);
}

/*
* @brief A cosine-weighted direction in the hemisphere around `normal`.
*
* Cosine-weighted rather than uniform because the integrand it feeds is already multiplied by
* cos(theta): sampling proportionally to it makes that factor cancel, so an estimator can average
* the radiances directly instead of weighting each one.
*/
float3 vkmCosineHemisphere(float3 normal, float2 uv)
{
    // Malley's method: a disk sample lifted onto the hemisphere is already cosine-distributed.
    const float radius = sqrt(uv.x);
    const float theta = 6.28318530718 * uv.y;
    const float3 disk = float3(radius * cos(theta), radius * sin(theta), sqrt(max(0.0, 1.0 - uv.x)));

    // Frisvad's branchless orthonormal basis, with the sign trick that avoids the singularity at
    // normal.z == -1 that the naive form has.
    const float sign = normal.z >= 0.0 ? 1.0 : -1.0;
    const float a = -1.0 / (sign + normal.z);
    const float b = normal.x * normal.y * a;
    const float3 tangent = float3(1.0 + sign * normal.x * normal.x * a, sign * b, -sign * normal.x);
    const float3 bitangent = float3(b, sign + normal.y * normal.y * a, -normal.y);

    return normalize(disk.x * tangent + disk.y * bitangent + disk.z * normal);
}

#endif // VKM_RANDOM_HLSLI
