// Copyright (c) 2026 Snowapril
//
// Phase 8.1: the reservoir, and how it is packed.
//
// A reservoir is the unit ReSTIR resamples. Its layout is an ABI in the strongest sense in this
// engine: a screen's worth is millions of them, every pass reads and writes them, and widening one
// field later costs a re-write of every pass plus the memory that goes with it. So the packing is
// chosen for what the resampling maths needs rather than for what is convenient to store
// (restir.md section 8.1):
//
//   position   fp32x3. NOT fp16 -- it feeds the reconnection Jacobian's 1/d^2, where a half-float
//              position at any distance from the origin loses the precision that term needs.
//   normal     octahedral, snorm16x2 in one word. Enough for a rejection test and a cosine.
//   radiance   RGB9E5, one word. NOT fp16x3: indirect radiance is HDR and unbounded, and fp16
//              both clips it at 65504 and quantizes the low end badly, while a shared exponent
//              spends its bits on the range that is actually occupied.
//   weight     fp32. W, the unbiased contribution weight.
//   M, age     8 bits each, so M <= 255 and age <= 255 -- which is also the confidence cap and
//              the age cap those two exist to enforce.
//
// 32 bytes, eight u32 words. One word is reserved: 8.4 may claim it to cache the sample's target
// pdf rather than recomputing it per neighbour.
//
// Usage: declare `RWStructuredBuffer<uint> g_Reservoirs` at whatever binding, then invoke
// VKM_RESERVOIR_DECLARE(). It refers to that buffer by name, the same convention
// VKM_MATERIAL_LOADER uses for the arrays it reads.

#ifndef VKM_RESERVOIR_HLSLI
#define VKM_RESERVOIR_HLSLI

#include "vkm_gbuffer.hlsli"

// Mirrored by vkm::kVkmReservoirWordStride; TestReservoirLayout pins the two together.
#define VKM_RESERVOIR_WORD_STRIDE 8

// Set on a reservoir whose sample is the environment rather than a surface. Its position is a
// point far along the ray rather than a real hit, so 8.4 must not apply a 1/d^2 Jacobian to it --
// an infinitely distant sample has none.
#define VKM_RESERVOIR_FLAG_ENVIRONMENT 0x1u

struct VkmReservoir
{
    float3 samplePosition;  // world space; where the light this reservoir carries came from
    float3 sampleNormal;    // geometric normal at that point
    float3 radiance;        // radiance leaving the sample towards the shading point
    float  weight;          // W, the unbiased contribution weight
    uint   sampleCount;     // M
    uint   age;             // frames since this sample was generated
    uint   flags;
};

VkmReservoir vkmEmptyReservoir()
{
    VkmReservoir reservoir;
    reservoir.samplePosition = float3(0.0, 0.0, 0.0);
    reservoir.sampleNormal = float3(0.0, 0.0, 1.0);
    reservoir.radiance = float3(0.0, 0.0, 0.0);
    reservoir.weight = 0.0;
    reservoir.sampleCount = 0;
    reservoir.age = 0;
    reservoir.flags = 0;
    return reservoir;
}

/*
* @brief The target function a reservoir's sample is resampled against.
*
* Luminance of the carried radiance, which is the standard choice for ReSTIR GI: it is what makes
* a bright sample more likely to survive resampling, and it is cheap enough to re-evaluate at a
* neighbour's shading point -- which 8.4 has to do, because p_hat is defined with respect to the
* receiving pixel and not the one the sample came from.
*
* Defined here rather than at its call sites so generation, resampling and the bias-correction
* denominator cannot drift apart; disagreeing about p_hat is a bias no image inspection finds.
*/
float vkmReservoirTargetPdf(float3 radiance)
{
    return dot(radiance, float3(0.2126, 0.7152, 0.0722));
}

/*
* @brief The target function of a sample as seen from a particular shading point.
*
* @details Luminance times the cosine at the receiver. The cosine is what makes p_hat depend on
* who is asking, and that dependence is the entire reason resampling helps: without it a
* neighbour's sample would be weighted the same wherever it went, and reuse would only add
* correlation.
*
* The receiver's BRDF is deliberately left out. For a Lambertian surface `f_s = albedo/pi` is
* constant across the pixel, and p_hat appears in both the resampling weight and the denominator
* that divides it, so a per-receiver constant cancels exactly. Leaving it out also keeps p_hat
* non-zero on a surface with a black channel, which the bias-correction denominator tests against.
*
* Returns 0 for a sample at the shading point itself or below its horizon -- both are cases the
* reconnection shift does not define.
*/
float vkmReservoirTargetPdfAt(float3 radiance, float3 shadingPosition, float3 shadingNormal,
                              float3 samplePosition)
{
    const float3 toSample = samplePosition - shadingPosition;
    const float distanceSquared = dot(toSample, toSample);
    if (distanceSquared <= 0.0)
    {
        return 0.0;
    }
    const float cosTheta = saturate(dot(shadingNormal, toSample * rsqrt(distanceSquared)));
    return vkmReservoirTargetPdf(radiance) * cosTheta;
}

/*
* @brief The reconnection shift's Jacobian, moving a sample from `fromPosition` to `toPosition`.
*
* @details A sample is stored as a direction from the pixel that generated it, but it names a
* POINT. Handing that point to another pixel changes the solid angle it subtends, and the estimator
* has to be told by exactly how much or it is biased -- brighter where neighbours are closer to the
* sample than the receiver is, darker where they are further.
*
*   J = (cos(phi_to) * d_from^2) / (cos(phi_from) * d_to^2)
*
* with phi measured at the SAMPLE, between its normal and the direction back to the pixel in
* question. Returns 0 when either end is at or behind the sample's horizon, which is where the
* shift is undefined rather than merely unlikely.
*
* An environment sample needs no special case: it sits far enough away that both distances and both
* angles are equal to within a rounding error, so this returns 1 on its own -- which is the right
* answer for a sample with no position to speak of.
*/
float vkmReservoirJacobian(float3 samplePosition, float3 sampleNormal,
                           float3 fromPosition, float3 toPosition)
{
    const float3 fromOffset = samplePosition - fromPosition;
    const float3 toOffset = samplePosition - toPosition;
    const float fromDistanceSquared = dot(fromOffset, fromOffset);
    const float toDistanceSquared = dot(toOffset, toOffset);
    if (fromDistanceSquared <= 0.0 || toDistanceSquared <= 0.0)
    {
        return 0.0;
    }
    const float cosFrom = -dot(fromOffset * rsqrt(fromDistanceSquared), sampleNormal);
    const float cosTo = -dot(toOffset * rsqrt(toDistanceSquared), sampleNormal);
    if (cosFrom <= 0.0 || cosTo <= 0.0)
    {
        return 0.0;
    }
    return (cosTo * fromDistanceSquared) / (cosFrom * toDistanceSquared);
}

// RGB9E5: three 9-bit mantissas and a shared 5-bit exponent, unsigned. The largest representable
// value is (511/512) * 2^15; the encode clamps to it rather than wrapping the exponent.
#define VKM_RGB9E5_MAX 65408.0

/*
* The bias is 15 and the mantissa 9 bits, so a channel's scale is 2^(exponent - 15 - 9). Getting
* that bias wrong is not the harmless-looking off-by-one it appears to be: pack and unpack use the
* same scale, so a wrong bias round-trips perfectly for small values and silently CLAMPS every
* channel whose mantissa lands above 511 -- which is the top half of every exponent range. It cost
* 7.4% of the image's energy and looked exactly like a plausible quantization loss.
*/
#define VKM_RGB9E5_EXPONENT_BIAS 15
#define VKM_RGB9E5_MANTISSA_BITS 9

uint vkmPackRgb9e5(float3 color)
{
    color = clamp(color, 0.0, VKM_RGB9E5_MAX);
    const float largest = max(color.r, max(color.g, color.b));
    // exp2/log2 rather than frexp: the shared exponent comes from the largest channel, and the
    // bias plus one puts it in the five bits available.
    int exponent = max(-(VKM_RGB9E5_EXPONENT_BIAS + 1),
                       (int)floor(log2(max(largest, 1.0e-30)))) + 1 + VKM_RGB9E5_EXPONENT_BIAS;
    float denominator = exp2((float)(exponent - VKM_RGB9E5_EXPONENT_BIAS - VKM_RGB9E5_MANTISSA_BITS));
    // Rounding the largest channel can carry into the exponent (511.5 rounds to 512, which does
    // not fit in nine bits), so the carry is handled before the other two are quantized.
    if ((int)floor(largest / denominator + 0.5) >= 512)
    {
        denominator *= 2.0;
        exponent += 1;
    }
    const uint3 mantissa = (uint3)clamp(floor(color / denominator + 0.5), 0.0, 511.0);
    return mantissa.r | (mantissa.g << 9) | (mantissa.b << 18) | ((uint)exponent << 27);
}

float3 vkmUnpackRgb9e5(uint packed)
{
    const int exponent = (int)(packed >> 27);
    const float scale = exp2((float)(exponent - VKM_RGB9E5_EXPONENT_BIAS - VKM_RGB9E5_MANTISSA_BITS));
    return float3(packed & 0x1FFu, (packed >> 9) & 0x1FFu, (packed >> 18) & 0x1FFu) * scale;
}

uint vkmPackNormalOct16(float3 normal)
{
    const float2 octahedral = vkmEncodeOctahedralNormal(normal);
    const int2 quantized = int2(round(clamp(octahedral, -1.0, 1.0) * 32767.0));
    return (uint(quantized.x) & 0xFFFFu) | ((uint(quantized.y) & 0xFFFFu) << 16);
}

float3 vkmUnpackNormalOct16(uint packed)
{
    // Both halves sign-extended by shifting them up to the sign bit and back down arithmetically.
    const int2 quantized = int2(packed << 16, packed) >> 16;
    return vkmDecodeOctahedralNormal(float2(quantized) / 32767.0);
}

#define VKM_RESERVOIR_DECLARE()                                                                     \
    /* Word offset of one reservoir. Slices are laid out back to back, so a pass reads one and   */ \
    /* writes another by index alone -- which is what makes a bypass or validation mode a        */ \
    /* different index rather than a different buffer (restir.md section 8.1).                   */ \
    uint vkmReservoirWordBase(uint slice, uint pixelIndex, uint pixelCount)                         \
    {                                                                                                \
        return (slice * pixelCount + pixelIndex) * VKM_RESERVOIR_WORD_STRIDE;                        \
    }                                                                                                \
                                                                                                     \
    void vkmStoreReservoir(uint wordBase, VkmReservoir reservoir)                                     \
    {                                                                                                \
        g_Reservoirs[wordBase + 0] = asuint(reservoir.samplePosition.x);                              \
        g_Reservoirs[wordBase + 1] = asuint(reservoir.samplePosition.y);                              \
        g_Reservoirs[wordBase + 2] = asuint(reservoir.samplePosition.z);                              \
        g_Reservoirs[wordBase + 3] = vkmPackNormalOct16(reservoir.sampleNormal);                      \
        g_Reservoirs[wordBase + 4] = vkmPackRgb9e5(reservoir.radiance);                               \
        g_Reservoirs[wordBase + 5] = asuint(reservoir.weight);                                        \
        g_Reservoirs[wordBase + 6] = (min(reservoir.sampleCount, 255u)) |                             \
                                     (min(reservoir.age, 255u) << 8) |                                \
                                     (reservoir.flags << 16);                                         \
        g_Reservoirs[wordBase + 7] = 0;                                                               \
    }                                                                                                 \
                                                                                                      \
    VkmReservoir vkmLoadReservoir(uint wordBase)                                                       \
    {                                                                                                  \
        VkmReservoir reservoir;                                                                         \
        reservoir.samplePosition = float3(asfloat(g_Reservoirs[wordBase + 0]),                          \
                                          asfloat(g_Reservoirs[wordBase + 1]),                          \
                                          asfloat(g_Reservoirs[wordBase + 2]));                         \
        reservoir.sampleNormal = vkmUnpackNormalOct16(g_Reservoirs[wordBase + 3]);                      \
        reservoir.radiance = vkmUnpackRgb9e5(g_Reservoirs[wordBase + 4]);                               \
        reservoir.weight = asfloat(g_Reservoirs[wordBase + 5]);                                         \
        const uint packed = g_Reservoirs[wordBase + 6];                                                 \
        reservoir.sampleCount = packed & 0xFFu;                                                         \
        reservoir.age = (packed >> 8) & 0xFFu;                                                          \
        reservoir.flags = (packed >> 16) & 0xFFu;                                                        \
        return reservoir;                                                                                \
    }

#endif // VKM_RESERVOIR_HLSLI
