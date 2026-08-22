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

#endif // VKM_SHADOW_HLSLI
