// Copyright (c) 2025 Snowapril
//
// Procedural unit sphere. Nothing else in the engine generates geometry in code -- every other
// sample pulls it out of a glTF file -- but a sphere that has to follow a simulated position is
// not scene content, so it is built here rather than imported.

#pragma once

#include <glm/geometric.hpp>
#include <glm/vec3.hpp>

#include <cmath>
#include <cstdint>
#include <vector>

namespace vkm
{
    /*
    * @brief One sphere vertex, laid out the way the shader's vertex pulling reads it.
    * @details The padding is part of the layout, not decoration: DXC's default storage-buffer
    * rules for -spirv targets align a float3 to 16 bytes, so the normal has to start at offset
    * 16. Stride is 32 bytes -- eight u32 words, which is what hand_sphere.hlsl indexes by.
    */
    struct SphereVertex
    {
        float position[3];
        float _pad0;
        float normal[3];
        float _pad1;
    };

    static_assert(sizeof(SphereVertex) == 32, "hand_sphere.hlsl assumes a 32-byte vertex stride");

    struct SphereMesh
    {
        std::vector<SphereVertex> _vertices;
        std::vector<uint32_t> _indices;
    };

    /*
    * @brief Builds a unit-radius UV sphere centred on the origin.
    * @details Wound counter-clockwise seen from outside, matching the "counter_clockwise" front
    * face and back-face culling declared in hand_sphere.json. The rings at the two poles collapse
    * to a point, so the first and last band carry degenerate triangles -- the standard cost of a
    * UV sphere, and harmless since a zero-area triangle rasterizes nothing.
    * @param stacks Rings from pole to pole; at least 2.
    * @param slices Segments around the equator; at least 3.
    * @param outMesh Receives the vertices and indices.
    */
    inline void buildSphereMesh(uint32_t stacks, uint32_t slices, SphereMesh* outMesh)
    {
        stacks = stacks < 2 ? 2 : stacks;
        slices = slices < 3 ? 3 : slices;

        constexpr float kPi = 3.14159265358979323846f;

        outMesh->_vertices.clear();
        outMesh->_vertices.reserve(static_cast<size_t>(stacks + 1) * (slices + 1));
        for (uint32_t stack = 0; stack <= stacks; ++stack)
        {
            const float theta = kPi * static_cast<float>(stack) / static_cast<float>(stacks);
            const float y = std::cos(theta);
            const float ringRadius = std::sin(theta);

            // The seam vertex is duplicated (slice == slices repeats slice 0) so the ring can be
            // indexed without wrapping.
            for (uint32_t slice = 0; slice <= slices; ++slice)
            {
                const float phi = 2.0f * kPi * static_cast<float>(slice) / static_cast<float>(slices);
                const glm::vec3 position(ringRadius * std::cos(phi), y, ringRadius * std::sin(phi));
                // Unit sphere about the origin, so the position is already the normal, except at
                // the poles where the ring radius is zero.
                const glm::vec3 normal = glm::length(position) > 1e-6f ? glm::normalize(position)
                                                                       : glm::vec3(0.0f, y, 0.0f);

                SphereVertex vertex{};
                vertex.position[0] = position.x;
                vertex.position[1] = position.y;
                vertex.position[2] = position.z;
                vertex.normal[0] = normal.x;
                vertex.normal[1] = normal.y;
                vertex.normal[2] = normal.z;
                outMesh->_vertices.push_back(vertex);
            }
        }

        const uint32_t ringStride = slices + 1;
        outMesh->_indices.clear();
        outMesh->_indices.reserve(static_cast<size_t>(stacks) * slices * 6);
        for (uint32_t stack = 0; stack < stacks; ++stack)
        {
            for (uint32_t slice = 0; slice < slices; ++slice)
            {
                const uint32_t topLeft = stack * ringStride + slice;
                const uint32_t topRight = topLeft + 1;
                const uint32_t bottomLeft = topLeft + ringStride;
                const uint32_t bottomRight = bottomLeft + 1;

                outMesh->_indices.push_back(topLeft);
                outMesh->_indices.push_back(topRight);
                outMesh->_indices.push_back(bottomLeft);

                outMesh->_indices.push_back(topRight);
                outMesh->_indices.push_back(bottomRight);
                outMesh->_indices.push_back(bottomLeft);
            }
        }
    }
} // namespace vkm
