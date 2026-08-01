// Copyright (c) 2025 Snowapril
//
// Shader-side mirror of the G-buffer's channel layout (see renderer/gbuffer.h, the single source
// of truth for formats and target order).
//
// Both the pass that writes the G-buffer and every pass that reads it include this header, so the
// packing is defined once. A GI pass that decoded normals with its own copy of the octahedral
// maths would drift the moment the layout changed, and the symptom -- subtly wrong normals -- is
// far easier to introduce than to notice.
//
//   target 0  RGBA16F  xy = octahedral shading normal, zw = octahedral geometric normal
//   target 1  RGBA8    rgb = base colour, a = roughness
//   target 2  RGBA16F  xy = screen-space motion vector (UV units), z = metallic,
//                      w = distance from the camera (0 where no geometry was drawn)
//   depth     D32      hardware depth
//
// Two normals, because they answer different questions: the shading normal drives lighting and
// temporal-tap rejection, while a secondary ray must be offset along the GEOMETRIC normal or it
// re-hits its own surface on normal-mapped geometry.

#ifndef VKM_GBUFFER_HLSLI
#define VKM_GBUFFER_HLSLI

/*
* @brief Octahedral normal encoding: a unit vector to two values in [-1, 1].
*
* Chosen over storing xyz because the engine exposes no two-channel format, so one RGBA16F target
* holds both normals at two channels each instead of costing a target apiece. Octahedral is the
* standard choice here -- near-uniform angular error, and cheap in both directions.
*/
float2 vkmEncodeOctahedralNormal(float3 normal)
{
    const float3 n = normal / (abs(normal.x) + abs(normal.y) + abs(normal.z));
    float2 encoded = n.xy;
    if (n.z < 0.0)
    {
        // Fold the lower hemisphere outward across the octahedron's diagonals.
        encoded = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return encoded;
}

float3 vkmDecodeOctahedralNormal(float2 encoded)
{
    float3 n = float3(encoded.x, encoded.y, 1.0 - abs(encoded.x) - abs(encoded.y));
    if (n.z < 0.0)
    {
        n.xy = (1.0 - abs(n.yx)) * float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
    }
    return normalize(n);
}

// Packs both normals into target 0. Inputs must be normalized and in world space.
float4 vkmPackGBufferNormals(float3 shadingNormal, float3 geometricNormal)
{
    return float4(vkmEncodeOctahedralNormal(shadingNormal),
                  vkmEncodeOctahedralNormal(geometricNormal));
}

float3 vkmUnpackShadingNormal(float4 packed)   { return vkmDecodeOctahedralNormal(packed.xy); }
float3 vkmUnpackGeometricNormal(float4 packed) { return vkmDecodeOctahedralNormal(packed.zw); }

/*
* @brief Screen-space motion, in UV units, from a fragment's current and previous clip positions.
*
* Returns where this pixel WAS: subtract it from the current UV to sample last frame. Both inputs
* are pre-divide clip positions, so the perspective divide happens here rather than being
* interpolated -- interpolating post-divide screen positions is wrong under perspective.
*
* The clip-to-UV mapping flips Y because the engine's clip space is +Y up on every backend while
* UV space is +Y down (see camera.h).
*/
float2 vkmComputeMotionVector(float4 currentClip, float4 previousClip)
{
    // A vertex behind the eye can reach the fragment stage with w <= 0 after clipping; reporting
    // no motion is better than a division that produces an enormous bogus vector a temporal pass
    // would then chase.
    if (currentClip.w <= 0.0 || previousClip.w <= 0.0)
    {
        return float2(0.0, 0.0);
    }
    const float2 currentNdc = currentClip.xy / currentClip.w;
    const float2 previousNdc = previousClip.xy / previousClip.w;
    const float2 ndcMotion = currentNdc - previousNdc;
    return float2(ndcMotion.x * 0.5, ndcMotion.y * -0.5);
}

/*
* @brief World position from a pixel's UV and its stored camera distance.
*
* Consumers read distance out of the G-buffer rather than sampling the depth attachment. Depth is
* still written and still depth-tests; it is simply never bound as a texture, because WebGPU
* validates a depth-format view against a depth sample type and would reject it as an ordinary
* sampled texture -- and because half-float *hardware* depth would be far too imprecise to
* reconstruct from, while a linear distance at the same width is not.
*/
float3 vkmReconstructWorldPosition(float2 uv, float distanceFromCamera,
                                   float4x4 inverseViewProjection, float3 cameraPositionWorld)
{
    // UV is +Y down, clip space is +Y up.
    const float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    const float4 farPointH = mul(inverseViewProjection, float4(ndc, 1.0, 1.0));
    const float3 rayDirection = normalize(farPointH.xyz / farPointH.w - cameraPositionWorld);
    return cameraPositionWorld + rayDirection * distanceFromCamera;
}

#endif // VKM_GBUFFER_HLSLI
