// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include <string>
#include <vector>

namespace vkm
{
    /*
    * @brief Interleaved vertex layout every imported mesh is converted to.
    *
    * The explicit padding reproduces the std430-like layout DXC emits for storage-buffer
    * structs on -spirv targets (a float3 is 16-byte aligned, a float2 8-byte), so shaders
    * can index this array straight out of the bindless buffer array without a repack. Any
    * change here must be mirrored in the shader-side VertexData struct.
    */
    struct VkmSceneVertex
    {
        float _position[3];  // offset 0
        float _pad0;         // offset 12
        float _normal[3];    // offset 16
        float _pad1;         // offset 28
        float _uv0[2];       // offset 32
        float _pad2[2];      // offset 40
        float _tangent[4];   // offset 48, xyz = tangent, w = bitangent sign
    };
    static_assert(sizeof(VkmSceneVertex) == 64, "VkmSceneVertex must match the shader-side std430 layout");

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
    * @brief One glTF primitive: the smallest unit the renderer draws, owning its own
    * vertex/index arrays so it maps 1:1 onto a pair of bindless buffers.
    */
    struct VkmSceneMesh
    {
        std::vector<VkmSceneVertex> _vertices;
        std::vector<uint32_t> _indices;
        uint32_t _materialIndex = INVALID_VALUE32;
        VkmSceneAABB _bounds;
    };

    // glTF metallic-roughness material factors. Texture references are not imported yet.
    struct VkmSceneMaterial
    {
        std::string _name;
        glm::vec4 _baseColorFactor{ 1.0f, 1.0f, 1.0f, 1.0f };
        glm::vec3 _emissiveFactor{ 0.0f, 0.0f, 0.0f };
        float _metallicFactor = 1.0f;
        float _roughnessFactor = 1.0f;
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
