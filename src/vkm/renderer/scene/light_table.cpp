// Copyright (c) 2026 Snowapril

#include <vkm/renderer/scene/light_table.h>

#include <vkm/base/common.h>
#include <vkm/renderer/scene/scene_model.h>
#include <vkm/renderer/scene/vertex_layout.h>

#include <glm/geometric.hpp>

#include <algorithm>

namespace vkm
{
    void vkmGatherEmissiveTriangles(const VkmSceneMesh& mesh, const glm::vec3& radiance,
                                    std::vector<VkmLightTableTriangle>* outTriangles)
    {
        VKM_ASSERT(outTriangles != nullptr, "vkmGatherEmissiveTriangles requires an output vector");

        const uint32_t stride = mesh._layout._stride;
        const size_t triangleCount = mesh._indices.size() / 3;
        outTriangles->reserve(outTriangles->size() + triangleCount);

        for (size_t triangle = 0; triangle < triangleCount; ++triangle)
        {
            VkmLightTableTriangle record{};
            float* corners[3] = { record._p0, record._p1, record._p2 };
            for (uint32_t corner = 0; corner < 3; ++corner)
            {
                const uint32_t vertexIndex = mesh._indices[triangle * 3 + corner];
                const uint8_t* vertex = mesh._vertexData.data() + static_cast<size_t>(vertexIndex) * stride;
                vkmReadVertexAttribute(vertex, mesh._layout, VkmVertexSemantic::Position, corners[corner], 3);
            }
            record._radiance[0] = radiance.x;
            record._radiance[1] = radiance.y;
            record._radiance[2] = radiance.z;
            outTriangles->push_back(record);
        }
    }

    size_t vkmFinalizeLightTable(std::vector<VkmLightTableTriangle>* triangles)
    {
        VKM_ASSERT(triangles != nullptr, "vkmFinalizeLightTable requires a triangle vector");

        double totalPower = 0.0;
        for (VkmLightTableTriangle& record : *triangles)
        {
            const glm::vec3 p0(record._p0[0], record._p0[1], record._p0[2]);
            const glm::vec3 p1(record._p1[0], record._p1[1], record._p1[2]);
            const glm::vec3 p2(record._p2[0], record._p2[1], record._p2[2]);
            record._area = 0.5f * glm::length(glm::cross(p1 - p0, p2 - p0));
            const float luminance = 0.2126f * record._radiance[0] + 0.7152f * record._radiance[1] +
                                    0.0722f * record._radiance[2];
            totalPower += static_cast<double>(record._area) * luminance;
        }

        triangles->erase(std::remove_if(triangles->begin(), triangles->end(),
                                        [](const VkmLightTableTriangle& record) {
                                            const float luminance = 0.2126f * record._radiance[0] +
                                                                    0.7152f * record._radiance[1] +
                                                                    0.0722f * record._radiance[2];
                                            return record._area * luminance <= 0.0f;
                                        }),
                         triangles->end());

        if (triangles->empty() || totalPower <= 0.0)
        {
            triangles->clear();
            return 0;
        }

        double cumulative = 0.0;
        for (VkmLightTableTriangle& record : *triangles)
        {
            const float luminance = 0.2126f * record._radiance[0] + 0.7152f * record._radiance[1] +
                                    0.0722f * record._radiance[2];
            cumulative += static_cast<double>(record._area) * luminance;
            record._cdf = static_cast<float>(cumulative / totalPower);
        }
        // Exactly 1, not merely close: the shader's binary search treats the last entry as the
        // ceiling, and float accumulation error must not leave u = 0.9999 past it.
        triangles->back()._cdf = 1.0f;
        return triangles->size();
    }
} // namespace vkm
