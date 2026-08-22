// Copyright (c) 2026 Snowapril

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
        /*
        * @brief glTF alphaMode MASK's cutoff, or 0 for OPAQUE and BLEND.
        * @details A masked material's base-colour alpha is a stencil, not an opacity: a leaf card
        * is a rectangle whose texture is transparent everywhere the leaf is not. Rendering one
        * without the test draws the whole rectangle, and since a masked texture's hidden texels
        * are near-black, the result is a hard-edged dark card. 0 means "no test", which is what
        * OPAQUE wants and the honest approximation for BLEND until real blending exists.
        */
        float _alphaCutoff = 0.0f;
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

    /*
    * @brief Which of glTF's three punctual light types a VkmScenePunctualLight is.
    * @details Values match the order the GPU record's `_type` word uses; do not renumber without
    * changing shaders/vkm_punctual_lights.hlsli with it.
    */
    enum class VkmLightType : uint32_t
    {
        Point = 0,
        Spot = 1,
        Directional = 2,
    };

    /*
    * @brief One KHR_lights_punctual light, in the units glTF states them in.
    * @details Position and direction are NOT here: a glTF light is positioned by the node that
    * references it, so a placed light is this record plus that node's world transform (see
    * VkmSceneModel::buildLightList). Units are glTF's own -- candela for point and spot, lux for
    * directional -- and the engine applies them unscaled, so a scene's absolute brightness is the
    * author's business rather than the importer's.
    */
    struct VkmScenePunctualLight
    {
        std::string _name;
        VkmLightType _type = VkmLightType::Point;
        glm::vec3 _color{ 1.0f, 1.0f, 1.0f };
        float _intensity = 1.0f;
        // Distance past which the light contributes nothing. 0 means unlimited, which is what
        // glTF says an absent `range` means.
        float _range = 0.0f;
        // Spot only, radians from the cone axis. Full intensity inside the inner angle, zero
        // outside the outer one.
        float _innerConeAngle = 0.0f;
        float _outerConeAngle = 0.7853982f; // glTF's default, pi/4
    };

    struct VkmSceneNode
    {
        std::string _name;
        glm::mat4 _localTransform{ 1.0f };
        std::vector<uint32_t> _meshIndices;  // indices into VkmSceneModel::_meshes
        std::vector<uint32_t> _childIndices; // indices into VkmSceneModel::_nodes
        // Index into VkmSceneModel::_lights, or INVALID_VALUE32 when this node carries no light.
        uint32_t _lightIndex = INVALID_VALUE32;
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

        /*
        * @brief One placed light: the light's parameters plus where the hierarchy put it.
        * @details glTF orients a light down its node's local -Z, so `_directionWorld` is the
        * normalized negated third column of `_worldTransform`. Meaningless for a point light and
        * left normalized anyway rather than zeroed, so a consumer never divides by it.
        */
        struct LightItem
        {
            uint32_t _lightIndex = INVALID_VALUE32;
            uint32_t _nodeIndex = INVALID_VALUE32;
            glm::vec3 _positionWorld{ 0.0f, 0.0f, 0.0f };
            glm::vec3 _directionWorld{ 0.0f, 0.0f, -1.0f };
        };

        std::vector<VkmSceneMesh> _meshes;
        std::vector<VkmSceneMaterial> _materials;
        std::vector<VkmSceneImage> _images;
        std::vector<VkmSceneNode> _nodes;
        std::vector<VkmScenePunctualLight> _lights;
        std::vector<uint32_t> _rootNodeIndices;

        // Flattens the hierarchy into one entry per (node, mesh) pair, in depth-first order.
        std::vector<DrawItem> buildDrawList() const;

        // The same walk for lights: one entry per node carrying one, in depth-first order.
        std::vector<LightItem> buildLightList() const;

        // World-space bounds of every mesh reachable from the root nodes; invalid when the
        // model has no drawable mesh.
        VkmSceneAABB computeWorldBounds() const;

        uint64_t getTotalVertexCount() const;
        uint64_t getTotalIndexCount() const;
    };
} // namespace vkm
