// Copyright (c) 2025 Snowapril

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
        float _pad = 0.0f;
    };
    static_assert(sizeof(VkmLightTableHeader) == 16,
                  "VkmLightTableHeader must match VKM_LIGHT_HEADER_WORDS in vkm_lights.hlsli");

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
