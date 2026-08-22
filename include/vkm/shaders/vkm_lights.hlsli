// Copyright (c) 2026 Snowapril
//
// The scene's light table, as next-event estimation reads it: a fixed header (the directional
// light's radiance) followed by emissive-triangle records carrying world-space corners, radiance,
// area and a normalized cumulative-power CDF. Built by VkmScene at build() (light_table.h owns
// the C++ half; TestObjectDataLayout pins the two strides together), reached through the bindless
// Buffer array at the slot VkmFrameData::_lightPoolSlot publishes.
//
// Usage: VKM_BINDLESS_VERTEX_PULLING(uint) must be in scope (the table rides the same untyped
// u32 word array the material pool does), then invoke VKM_LIGHT_LOADER().

#ifndef VKM_LIGHTS_HLSLI
#define VKM_LIGHTS_HLSLI

// Mirrors sizeof(VkmLightTableHeader) / 4 and sizeof(VkmLightTableTriangle) / 4.
#define VKM_LIGHT_HEADER_WORDS 4
#define VKM_LIGHT_WORD_STRIDE 16

struct VkmLightTriangle
{
    float3 p0;
    float3 p1;
    float3 p2;
    float3 radiance;
    float  area;
    float  cdf;
};

struct VkmLightSample
{
    float3 position;   // the sampled point on the triangle
    float3 normal;     // the triangle's geometric normal, unnormalized orientation as authored
    float3 radiance;
    float  pdfArea;    // probability density of `position` in the area measure
};

#define VKM_LIGHT_LOADER()                                                                          \
    float3 vkmLoadSunRadiance(uint lightPoolSlot)                                                   \
    {                                                                                               \
        return float3(asfloat(VKM_LOAD_VERTEX(lightPoolSlot, 0)),                                   \
                      asfloat(VKM_LOAD_VERTEX(lightPoolSlot, 1)),                                   \
                      asfloat(VKM_LOAD_VERTEX(lightPoolSlot, 2)));                                  \
    }                                                                                               \
                                                                                                    \
    VkmLightTriangle vkmLoadLightTriangle(uint lightPoolSlot, uint index)                           \
    {                                                                                               \
        const uint base = VKM_LIGHT_HEADER_WORDS + index * VKM_LIGHT_WORD_STRIDE;                   \
        VkmLightTriangle record;                                                                  \
        record.p0 = float3(asfloat(VKM_LOAD_VERTEX(lightPoolSlot, base + 0)),                     \
                             asfloat(VKM_LOAD_VERTEX(lightPoolSlot, base + 1)),                     \
                             asfloat(VKM_LOAD_VERTEX(lightPoolSlot, base + 2)));                    \
        record.p1 = float3(asfloat(VKM_LOAD_VERTEX(lightPoolSlot, base + 3)),                     \
                             asfloat(VKM_LOAD_VERTEX(lightPoolSlot, base + 4)),                     \
                             asfloat(VKM_LOAD_VERTEX(lightPoolSlot, base + 5)));                    \
        record.p2 = float3(asfloat(VKM_LOAD_VERTEX(lightPoolSlot, base + 6)),                     \
                             asfloat(VKM_LOAD_VERTEX(lightPoolSlot, base + 7)),                     \
                             asfloat(VKM_LOAD_VERTEX(lightPoolSlot, base + 8)));                    \
        record.radiance = float3(asfloat(VKM_LOAD_VERTEX(lightPoolSlot, base + 9)),               \
                                   asfloat(VKM_LOAD_VERTEX(lightPoolSlot, base + 10)),              \
                                   asfloat(VKM_LOAD_VERTEX(lightPoolSlot, base + 11)));             \
        record.area = asfloat(VKM_LOAD_VERTEX(lightPoolSlot, base + 12));                         \
        record.cdf = asfloat(VKM_LOAD_VERTEX(lightPoolSlot, base + 13));                          \
        return record;                                                                            \
    }                                                                                               \
                                                                                                    \
    /* One light sample distributed proportionally to emitted power: pick the triangle whose  */    \
    /* CDF interval contains u (binary search over the cdf word), then a uniform point on it. */    \
    /* pdfArea = P(triangle) / area, the density NEE's area-measure estimator divides by.     */    \
    VkmLightSample vkmSampleLightTable(uint lightPoolSlot, uint lightCount, float u, float2 uv)     \
    {                                                                                               \
        uint lo = 0;                                                                                \
        uint hi = lightCount - 1;                                                                   \
        while (lo < hi)                                                                             \
        {                                                                                           \
            const uint mid = (lo + hi) / 2;                                                         \
            const float cdf = asfloat(VKM_LOAD_VERTEX(                                              \
                lightPoolSlot, VKM_LIGHT_HEADER_WORDS + mid * VKM_LIGHT_WORD_STRIDE + 13));         \
            if (u > cdf) { lo = mid + 1; } else { hi = mid; }                                       \
        }                                                                                           \
        const VkmLightTriangle record = vkmLoadLightTriangle(lightPoolSlot, lo);                  \
        const float previousCdf =                                                                   \
            lo == 0 ? 0.0                                                                           \
                    : asfloat(VKM_LOAD_VERTEX(                                                      \
                          lightPoolSlot, VKM_LIGHT_HEADER_WORDS + (lo - 1) * VKM_LIGHT_WORD_STRIDE + 13)); \
                                                                                                    \
        /* Uniform point via the sqrt map; the barycentric fold keeps it inside the triangle. */    \
        const float s = sqrt(uv.x);                                                                 \
        const float b1 = 1.0 - s;                                                                   \
        const float b2 = s * uv.y;                                                                  \
                                                                                                    \
        VkmLightSample lightSample;                                                                 \
        lightSample.position = record.p0 * b1 + record.p1 * b2 +                                \
                               record.p2 * (1.0 - b1 - b2);                                       \
        lightSample.normal = normalize(cross(record.p1 - record.p0, record.p2 - record.p0)); \
        lightSample.radiance = record.radiance;                                                   \
        lightSample.pdfArea = max(record.cdf - previousCdf, 1.0e-8) / max(record.area, 1.0e-8); \
        return lightSample;                                                                         \
    }

#endif // VKM_LIGHTS_HLSLI
