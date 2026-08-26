// Copyright (c) 2026 Snowapril

#pragma once

#include <glm/vec3.hpp>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vkm
{
    struct VkmSceneMesh;

    /*
    * @brief One emissive triangle in the scene's light table.
    * @details The unit next-event estimation samples: positions are world space, baked at scene
    * build (a moved emitter needs a table rebuild), radiance is the material's emissive factor,
    * and `_cdf` is the normalized cumulative power up to and including this entry — the last
    * entry's is exactly 1, which is what the shader's binary search runs over. 64 bytes = 16
    * words, addressable through the bindless buffer array; mirrored by vkm_lights.hlsli and
    * pinned by TestObjectDataLayout.
    */
    struct VkmLightTableTriangle
    {
        float _p0[3];
        float _p1[3];
        float _p2[3];
        float _radiance[3];
        float _area = 0.0f;
        float _cdf = 0.0f;
        float _pad[2] = { 0.0f, 0.0f };
    };
    static_assert(sizeof(VkmLightTableTriangle) == 64,
                  "VkmLightTableTriangle must match VKM_LIGHT_WORD_STRIDE in vkm_lights.hlsli");

    /*
    * @brief The light table's fixed prefix: what exists once per scene rather than per triangle.
    * @details The directional light's radiance. Its per-frame direction stays in
    * VkmFrameData::_lightDirection; the radiance is upload-once static, which is what lets it
    * live in a buffer written at build.
    */
    struct VkmLightTableHeader
    {
        float _sunRadiance[3] = { 0.0f, 0.0f, 0.0f };
        /*
        * @brief Punctual light records following the triangles, as a count.
        * @details The blob is [header][triangle * N][punctual * M]. N rides FrameData's
        * _lightCount because every consumer already needs it to sample the triangles; M lives
        * here because nothing outside the table needs it, and the header had an unused word.
        * A reader finds the first punctual record at header + N * stride -- the two record types
        * share a 64-byte stride so that arithmetic is one multiply.
        */
        uint32_t _punctualCount = 0u;
    };
    static_assert(sizeof(VkmLightTableHeader) == 16,
                  "VkmLightTableHeader must match VKM_LIGHT_HEADER_WORDS in vkm_lights.hlsli");

    /*
    * @brief One placed punctual light, as every GPU consumer sees it.
    * @details A VkmScenePunctualLight resolved against its node's world transform, with the
    * colour and intensity already multiplied together so a shader never has to know glTF's
    * split. Directional lights carry a meaningless position and point lights a meaningless
    * direction; both stay finite rather than zeroed so no consumer divides by them.
    * 64 bytes, matching VkmLightTableTriangle's stride, so the two share one blob: the raster
    * tier reads this record out of a set-2 uniform buffer and the traced tier reads the very
    * same bytes out of the bindless light table.
    */
    struct VkmPunctualLight
    {
        float _positionWorld[3] = { 0.0f, 0.0f, 0.0f };
        // glTF's range: the distance past which the light contributes nothing. 0 = unlimited.
        float _range = 0.0f;
        float _directionWorld[3] = { 0.0f, 0.0f, -1.0f };
        // Cosine of the spot's outer cone half-angle. 1 would be a degenerate zero-width cone,
        // so a non-spot carries -1: every direction is inside it.
        float _cosOuter = -1.0f;
        float _radiance[3] = { 0.0f, 0.0f, 0.0f }; // colour * intensity
        float _cosInner = -1.0f;
        uint32_t _type = 0; // VkmLightType
        // Index of this light's first tile in the shadow atlas, or -1 when it casts none.
        int32_t _shadowTile = -1;
        /*
        * @brief How many consecutive tiles from _shadowTile this light owns.
        * @details Six for a point light, one per cube face; one for a spot; and one per cascade
        * for a directional light. Explicit rather than implied by the type, because the cascade
        * count is a runtime choice and a lookup that assumed one would silently read a
        * neighbouring light's tile.
        */
        uint32_t _shadowTileCount = 0u;
        float _pad = 0.0f;
    };
    static_assert(sizeof(VkmPunctualLight) == 64,
                  "VkmPunctualLight must match VkmPunctualLight in shaders/vkm_punctual_lights.hlsli");

    /*
    * @brief Appends one triangle record per triangle of `mesh`, in object space.
    * @details Positions are read through the mesh's own vertex layout; `_area` and `_cdf` are
    * left zero — they are meaningless before the world transform is applied, which is
    * vkmFinalizeLightTable's job. Free-standing and driver-free, like vkmBuildNeighbourOffsets,
    * so the gather is testable without a GPU.
    * @param mesh The mesh whose triangles to gather.
    * @param radiance The owning material's emissive factor.
    * @param outTriangles Receives one record per triangle, appended.
    */
    void vkmGatherEmissiveTriangles(const VkmSceneMesh& mesh, const glm::vec3& radiance,
                                    std::vector<VkmLightTableTriangle>* outTriangles);

    /*
    * @brief Computes areas and the normalized power CDF in place, dropping zero-power entries.
    * @details Power is luminance(radiance) * area — the emitted-flux ordering up to a constant
    * factor of pi, which cancels in the normalization. Zero-power triangles (degenerate area or
    * black radiance) are removed rather than kept at zero probability: they contribute nothing
    * to either estimator, and dropping them keeps the CDF strictly increasing. The last kept
    * entry's `_cdf` is exactly 1.
    * @param triangles Records with world-space positions and radiance filled in.
    * @return The kept entry count.
    */
    size_t vkmFinalizeLightTable(std::vector<VkmLightTableTriangle>* triangles);
} // namespace vkm
