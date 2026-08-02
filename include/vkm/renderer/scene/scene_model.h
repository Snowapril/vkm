// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/scene/vertex_layout.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace vkm
{
    struct VkmSceneAABB
    {
        glm::vec3 _min{ 0.0f, 0.0f, 0.0f };
        glm::vec3 _max{ 0.0f, 0.0f, 0.0f };
        bool _valid = false;

        void expand(const glm::vec3& point);
        void expand(const VkmSceneAABB& other);

        // Axis-aligned bounds of this box transformed by `transform` (the transformed box
        // is re-fitted, so it is conservative rather than tight).
        VkmSceneAABB transformed(const glm::mat4& transform) const;

        inline glm::vec3 getCenter() const { return (_min + _max) * 0.5f; }
        inline glm::vec3 getExtent() const { return _max - _min; }
    };

    /*
    * @brief One glTF primitive: the smallest unit the renderer draws.
    *
    * Vertices are raw interleaved bytes in `_layout`'s format rather than a fixed struct, so one
    * importer can produce any of the engine's vertex layout presets. Use
    * vkmRead/WriteVertexAttribute to touch them -- nothing should index `_vertexData` with a
    * hand-written offset.
    */
    struct VkmSceneMesh
    {
        std::vector<uint8_t> _vertexData; // _vertexCount * _layout._stride bytes
        std::vector<uint32_t> _indices;
        VkmVertexLayout _layout{};
        uint32_t _vertexCount = 0;
        uint32_t _materialIndex = INVALID_VALUE32;
        VkmSceneAABB _bounds;
    };

    /*
    * @brief glTF metallic-roughness material factors, plus references into VkmSceneModel::_images.
    *
    * @details A texture index is an index into the model's own image list, not a GPU slot -- the
    * model stays GPU-free (see the type's comment), so who uploads the image and how it is
    * addressed afterwards is the scene's business. INVALID_VALUE32 means the material has no
    * texture for that channel and the factor alone applies, which is what every shader has to
    * handle anyway: glTF says the two multiply, so a missing texture is white.
    */
    struct VkmSceneMaterial
    {
        std::string _name;
        glm::vec4 _baseColorFactor{ 1.0f, 1.0f, 1.0f, 1.0f };
        glm::vec3 _emissiveFactor{ 0.0f, 0.0f, 0.0f };
        float _metallicFactor = 1.0f;
        float _roughnessFactor = 1.0f;

        uint32_t _baseColorImage = INVALID_VALUE32;
        // glTF packs occlusion/roughness/metallic into one image's b/g/r channels.
        uint32_t _metallicRoughnessImage = INVALID_VALUE32;
        uint32_t _normalImage = INVALID_VALUE32;
        uint32_t _emissiveImage = INVALID_VALUE32;
    };

    /*
    * @brief An image the model references, as a path to decode later.
    *
    * @details Deliberately a path rather than decoded pixels: a glTF's images are often larger than
    * its geometry, and importGltfModel() is called on the main thread. Decoding is left to whoever
    * uploads, so it can be skipped, deferred or threaded without the importer having an opinion.
    *
    * Empty when the glTF embedded the image as a buffer view or a data URI rather than a file --
    * neither is handled yet, and an empty path is how a consumer tells.
    */
    struct VkmSceneImage
    {
        std::string _uri; // resolved against the glTF's own directory
    };

    struct VkmSceneNode
    {
        std::string _name;
        glm::mat4 _localTransform{ 1.0f };
        std::vector<uint32_t> _meshIndices;  // indices into VkmSceneModel::_meshes
        std::vector<uint32_t> _childIndices; // indices into VkmSceneModel::_nodes
    };

    /*
    * @brief CPU-side representation of an imported scene: a flat mesh/material pool plus
    * the node hierarchy referencing it. Backend-agnostic and GPU-free -- see
    * VkmSceneModelGpu for the upload side.
    */
    struct VkmSceneModel
    {
        // One draw: a mesh plus the world transform accumulated down the node hierarchy.
        struct DrawItem
        {
            uint32_t _meshIndex = INVALID_VALUE32;
            uint32_t _nodeIndex = INVALID_VALUE32;
            glm::mat4 _worldTransform{ 1.0f };
        };

        std::vector<VkmSceneMesh> _meshes;
        std::vector<VkmSceneMaterial> _materials;
        std::vector<VkmSceneImage> _images;
        std::vector<VkmSceneNode> _nodes;
        std::vector<uint32_t> _rootNodeIndices;

        // Flattens the hierarchy into one entry per (node, mesh) pair, in depth-first order.
        std::vector<DrawItem> buildDrawList() const;

        // World-space bounds of every mesh reachable from the root nodes; invalid when the
        // model has no drawable mesh.
        VkmSceneAABB computeWorldBounds() const;

        uint64_t getTotalVertexCount() const;
        uint64_t getTotalIndexCount() const;
    };
} // namespace vkm
