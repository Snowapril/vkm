// Copyright (c) 2026 Snowapril
//
// Shader-side mirror of the shadow atlas's layout, shared by the pass that fills it and every
// pass that reads it -- so a tile's rectangle is computed in one place rather than restated.
//
// The atlas stores LINEAR world-space distance from the light in .r. See shadow_depth.hlsl for
// why linear distance rather than post-projection depth, and renderer/shadow_atlas.h for why an
// atlas rather than a texture array.

#ifndef VKM_SHADOW_HLSLI
#define VKM_SHADOW_HLSLI

// Mirrors vkm::kVkmMaxShadowTiles (renderer/shadow_atlas.h). Tiles, not lights: a point light
// owns six.
#define VKM_MAX_SHADOW_TILES 16

// What an untouched atlas texel holds. Far enough that every real query reads as unoccluded, and
// finite in a half-float, whose largest value is 65504.
#define VKM_SHADOW_FAR_SENTINEL 60000.0

// Mirrors vkm::VkmShadowAtlasConstants (renderer/shadow_atlas.h), byte for byte. Declared here
// rather than in each consuming shader so the pass that fills the atlas and every pass that reads
// it provably project by the same matrices -- there is one declaration and one buffer.
struct VkmShadowAtlasConstants
{
    float4x4 tileViewProjection[VKM_MAX_SHADOW_TILES];
    // xyz = the position this tile measures distance from, w = 1 positional / 0 directional.
    float4 tileLightPosition[VKM_MAX_SHADOW_TILES];
    // xyz = the direction the light points, w = the world size of one texel at unit distance.
    float4 tileLightDirection[VKM_MAX_SHADOW_TILES];
};

/*
* @brief Which cube face a direction from a point light falls in.
* @details Face order matches vkmBuildProbeFaceViewProjections (renderer/probe_volume.cpp):
* +X, -X, +Y, -Y, +Z, -Z. Selected by the major axis, which is what makes it agree with the six
* 90-degree frusta that filled those tiles.
*/
uint vkmShadowCubeFace(float3 direction)
{
    const float3 magnitude = abs(direction);
    if (magnitude.x >= magnitude.y && magnitude.x >= magnitude.z)
    {
        return direction.x >= 0.0 ? 0u : 1u;
    }
    if (magnitude.y >= magnitude.z)
    {
        return direction.y >= 0.0 ? 2u : 3u;
    }
    return direction.z >= 0.0 ? 4u : 5u;
}

/*
* @brief The atlas UV rectangle of one tile.
* @details Tiles run left to right, top to bottom, in the same order the fill pass sets its
* viewports -- so this and VkmShadowAtlas::record cannot disagree about where a tile is.
* @param tileIndex Linear tile index.
* @param tilesPerRow Tiles across the atlas.
* @return xy = the tile's origin in UV, zw = its size in UV.
*/
float4 vkmShadowTileRect(uint tileIndex, uint tilesPerRow)
{
    const float inverse = 1.0 / float(tilesPerRow);
    const float2 origin = float2(float(tileIndex % tilesPerRow), float(tileIndex / tilesPerRow)) * inverse;
    return float4(origin, inverse, inverse);
}

/*
* @brief Declares the shadow lookup against a named atlas texture and sampler.
* @details A macro rather than functions taking resource parameters, matching VKM_LIGHT_LOADER:
* passing a Texture2D through a function signature is a path SPIRV-Cross to MSL and WGSL is not
* exercised on anywhere else in this engine, and there is no reason to be the first.
*/
#define VKM_SHADOW_LOADER(AtlasTexture, AtlasSampler, AtlasConstants)                              \
    /* Distance a fragment is from the light that owns `tile`, in the same measure the atlas   */  \
    /* stores: radial for a positional light, along the axis for a directional one.            */  \
    float vkmShadowReceiverDistance(uint tile, float3 worldPosition)                               \
    {                                                                                              \
        const float4 lightPosition = AtlasConstants.tileLightPosition[tile];                       \
        const float3 relative = worldPosition - lightPosition.xyz;                                 \
        return lightPosition.w > 0.5                                                               \
                   ? length(relative)                                                              \
                   : dot(relative, normalize(AtlasConstants.tileLightDirection[tile].xyz));         \
    }                                                                                              \
                                                                                                   \
    /* One tap: 1 when the atlas says nothing nearer to the light covers this point.           */  \
    float vkmShadowTap(uint tile, uint tilesPerRow, uint tileSize, float2 tileUv,                  \
                       float receiverDistance, float bias)                                         \
    {                                                                                              \
        /* Clamped inside the tile, and this is not optional: a tap that walks off the edge     */  \
        /* reads the NEIGHBOURING light's tile, which is the atlas's one real hazard.           */  \
        const float halfTexel = 0.5 / float(tileSize);                                             \
        const float2 clamped = clamp(tileUv, halfTexel, 1.0 - halfTexel);                          \
        const float4 rect = vkmShadowTileRect(tile, tilesPerRow);                                  \
        const float2 atlasUv = rect.xy + clamped * rect.zw;                                        \
        const float stored = AtlasTexture.SampleLevel(AtlasSampler, atlasUv, 0).r;                 \
        return (receiverDistance - bias) <= stored ? 1.0 : 0.0;                                    \
    }                                                                                              \
                                                                                                   \
    /*                                                                                          */ \
    /* How much of `light` reaches `worldPosition`: 1 lit, 0 fully shadowed.                    */ \
    /*                                                                                          */ \
    /* The bias is in WORLD UNITS, expressed as a multiple of the shadow texel's world-space    */ \
    /* footprint at the receiver. That is what lets one constant work on a Cornell box and on   */ \
    /* a 3721-unit Sponza -- the same discipline the probe volume's normal bias landed under.   */ \
    /* It has to be in-shader: the RHI has no depth-bias state at all, and the value being      */ \
    /* compared is a colour attachment's contents rather than the rasterizer's depth.           */ \
    float vkmShadowFactor(VkmPunctualLight light, float3 worldPosition, float3 geometricNormal,    \
                          float nDotL, uint tilesPerRow, uint tileSize)                            \
    {                                                                                              \
        if (light.shadowTile < 0)                                                                  \
        {                                                                                          \
            return 1.0;                                                                            \
        }                                                                                          \
                                                                                                   \
        uint tile = uint(light.shadowTile);                                                        \
        const uint tileCount = max(light.shadowTileCount, 1u);                                     \
        float2 tileUv = float2(0.0, 0.0);                                                          \
                                                                                                   \
        if (light.type == VKM_LIGHT_TYPE_POINT)                                                    \
        {                                                                                          \
            /* A cube face is chosen, not searched: the six 90-degree frusta tile every        */  \
            /* direction exactly, so the major axis names the one face that can contain it.    */  \
            tile += vkmShadowCubeFace(worldPosition - light.positionWorld);                        \
            const float texelGuess = AtlasConstants.tileLightDirection[tile].w *                   \
                                     length(worldPosition - light.positionWorld);                  \
            const float3 faceQuery = worldPosition + geometricNormal * texelGuess;                 \
            const float4 faceClip =                                                                \
                mul(AtlasConstants.tileViewProjection[tile], float4(faceQuery, 1.0));               \
            if (faceClip.w <= 0.0)                                                                 \
            {                                                                                      \
                return 1.0;                                                                        \
            }                                                                                      \
            const float3 faceNdc = faceClip.xyz / faceClip.w;                                      \
            if (any(abs(faceNdc.xy) > 1.0) || faceNdc.z < 0.0 || faceNdc.z > 1.0)                  \
            {                                                                                      \
                return 1.0;                                                                        \
            }                                                                                      \
            tileUv = float2(faceNdc.x * 0.5 + 0.5, 0.5 - faceNdc.y * 0.5);                         \
        }                                                                                          \
        else                                                                                       \
        {                                                                                          \
            /*                                                                                  */ \
            /* Cascades, searched near to far. A directional light owns one tile per cascade,   */ \
            /* each fitted to a slice of the view, and the first one that CONTAINS the point is */ \
            /* the tightest that covers it. Selecting by containment rather than by distance    */ \
            /* from the camera is what lets this same lookup serve the probe capture, which has */ \
            /* no camera to measure against. A spot light has one tile and takes the same path. */ \
            /*                                                                                  */ \
            bool found = false;                                                                    \
            for (uint cascade = 0; cascade < tileCount; ++cascade)                                 \
            {                                                                                      \
                const uint candidate = uint(light.shadowTile) + cascade;                           \
                const float texelGuess = AtlasConstants.tileLightDirection[candidate].w;           \
                const float3 query = worldPosition + geometricNormal * texelGuess;                 \
                const float4 clip =                                                                \
                    mul(AtlasConstants.tileViewProjection[candidate], float4(query, 1.0));          \
                if (clip.w <= 0.0)                                                                 \
                {                                                                                  \
                    continue;                                                                      \
                }                                                                                  \
                const float3 ndc = clip.xyz / clip.w;                                              \
                if (any(abs(ndc.xy) > 1.0) || ndc.z < 0.0 || ndc.z > 1.0)                          \
                {                                                                                  \
                    continue;                                                                      \
                }                                                                                  \
                tile = candidate;                                                                  \
                tileUv = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);                             \
                found = true;                                                                      \
                break;                                                                             \
            }                                                                                      \
            /* Past the last cascade: unshadowed rather than wrongly dark. This is the edge a   */ \
            /* shadow distance buys -- beyond it nothing casts, which reads as flat lighting    */ \
            /* rather than as a black band.                                                     */ \
            if (!found)                                                                            \
            {                                                                                      \
                return 1.0;                                                                        \
            }                                                                                      \
        }                                                                                          \
                                                                                                   \
        const float receiverDistance = vkmShadowReceiverDistance(tile, worldPosition);             \
        /* A texel's world footprint, from the value the atlas stored for THIS tile: a          */  \
        /* perspective tile's grows with the receiver's distance, an orthographic one's does    */  \
        /* not. Deriving it here from a single formula would be right for one and wrong for     */  \
        /* the other, and the directional case is the one where it is wrong by the scene's      */  \
        /* whole size.                                                                          */  \
        const float4 lightPosition = AtlasConstants.tileLightPosition[tile];                       \
        const float perDistance = AtlasConstants.tileLightDirection[tile].w;                       \
        const float texelWorld =                                                                   \
            max(lightPosition.w > 0.5 ? perDistance * receiverDistance : perDistance, 1e-6);       \
        /* Slope-scaled: a surface seen edge-on by the light spans many texels of depth in one  */  \
        /* texel of area, and needs proportionally more room.                                   */  \
        const float slope = sqrt(saturate(1.0 - nDotL * nDotL)) / max(nDotL, 1e-3);                \
        const float bias = texelWorld * (1.0 + 2.0 * clamp(slope, 0.0, 8.0));                      \
                                                                                                   \
        /* 3x3 PCF. Enough to soften the staircase a tile's resolution puts on a shadow edge    */  \
        /* without pretending to be a soft shadow, which needs a light with area.               */  \
        const float texel = 1.0 / float(tileSize);                                                 \
        float sum = 0.0;                                                                           \
        for (int y = -1; y <= 1; ++y)                                                              \
        {                                                                                          \
            for (int x = -1; x <= 1; ++x)                                                          \
            {                                                                                      \
                sum += vkmShadowTap(tile, tilesPerRow, tileSize,                                   \
                                    tileUv + float2(float(x), float(y)) * texel,                   \
                                    receiverDistance, bias);                                       \
            }                                                                                      \
        }                                                                                          \
        return sum * (1.0 / 9.0);                                                                  \
    }

#endif // VKM_SHADOW_HLSLI
