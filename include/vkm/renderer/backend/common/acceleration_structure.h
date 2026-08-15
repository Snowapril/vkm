// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/render_resource.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <vector>

namespace vkm
{
    /*
    * @brief Which level of the two-level hierarchy a structure is.
    * @details Both APIs model ray tracing the same way: a bottom-level structure holds geometry in
    * its own object space, and a top-level structure holds instances of bottom-level ones with a
    * transform each. The split makes a moved object cost one instance-transform rewrite instead of
    * a geometry rebuild.
    */
    enum class VkmAccelerationStructureType : uint8_t
    {
        BottomLevel = 0,
        TopLevel    = 1,
    };

    /*
    * @brief One indexed triangle range inside a bottom-level structure.
    * @details A range into buffers the caller already owns rather than geometry of its own:
    * `VkmSceneGeometryPool` keeps every mesh's vertices and indices in one pair of buffers, and a
    * build reads them in place, so no vertex data is duplicated to trace it.
    * Triangles only. Both backends can describe procedural/AABB geometry, but it is silently wrong
    * at runtime through this toolchain, so there is no shape for it here.
    * The vertex format is fixed at three consecutive `float`s at the vertex view's offset, strided
    * by `_vertexStride`. That matches every `VkmVertexLayoutPreset`, whose position is always the
    * first attribute -- see `vkm_vertex_layout.h`.
    */
    struct VkmAccelerationStructureGeometry
    {
        /*
        * Both ranges are `VkmBufferView` handles rather than (buffer, byte offset) pairs, a view
        * being what already names a range inside a buffer someone else owns. A view meant for a
        * build carries no format, so it costs nothing on either backend: `VkmBufferViewVulkan`
        * creates no `VkBufferView` for a format-less view, and Metal's is metadata only.
        * A view carries an offset and a size but no stride and no element count, so those stay here.
        */
        VkmResourceHandle _vertexView{};
        uint32_t          _vertexStride = 0;
        uint32_t          _vertexCount = 0;

        VkmResourceHandle _indexView{};
        // Indices are u32, matching VkmSceneGeometryPool's index pool. Must be a multiple of 3.
        uint32_t          _indexCount = 0;
    };

    /*
    * @brief One bottom-level structure placed into a top-level one.
    * @details `_transform` is a full 4x4 for the caller's convenience, but only the upper 3x4
    * reaches the API, neither backend's instance descriptor carrying a projective row. A transform
    * with a non-affine bottom row is silently truncated, so pass object-to-world matrices only.
    */
    struct VkmAccelerationStructureInstance
    {
        glm::mat4         _transform{ 1.0f };
        VkmResourceHandle _blas{};
        // Readable from a shader as the hit's instance id; the engine uses it to recover which
        // VkmObjectData a hit belongs to.
        uint32_t          _instanceId = 0;
    };

    struct VkmAccelerationStructureInfo : public VkmResourceInfo
    {
        VkmAccelerationStructureType _type = VkmAccelerationStructureType::BottomLevel;
        // Exactly one of these is read, chosen by _type. A structure with neither is valid and
        // builds an empty one -- a scene with no geometry still needs something to bind.
        std::vector<VkmAccelerationStructureGeometry> _geometries;
        std::vector<VkmAccelerationStructureInstance> _instances;
        /*
        * Whether this structure can be rebuilt after creation. Off by default: keeping a structure
        * rebuildable costs a scratch buffer for its whole lifetime, and most geometry never moves.
        * On for the top-level structure of any scene with a dynamic object. A falling rigid body
        * changes only its instance transform, so its bottom-level structure stays built once.
        */
        bool _allowUpdate = false;
        /*
        * Object-space bounds of the geometry a bottom-level structure is built over, for debug
        * visualization only -- no backend reads them. Both zero means unknown.
        * VkmScene::buildAccelerationStructures fills them from the mesh entry's imported bounds.
        */
        glm::vec3 _boundsMin{ 0.0f };
        glm::vec3 _boundsMax{ 0.0f };
    };

    /*
    * @brief A built acceleration structure.
    * @details Built once by `VkmDriverBase::newAccelerationStructure`, synchronously, the way
    * `uploadToBuffer` uploads: a one-off command buffer submitted and waited on. That is the whole
    * lifetime of a structure whose geometry never moves.
    * A structure created with `_allowUpdate` can also be rebuilt: call `updateInstances` with the
    * new transforms, then record `VkmCommandBufferBase::buildAccelerationStructure` in a render
    * graph pass. It keeps its scratch buffer for that, so it costs more memory than a static one.
    * A structure is addressed by a shader through the bindless set, not by handle; see
    * `VkmBindlessResourceManagerBase`.
    */
    class VkmAccelerationStructure : public VkmRenderResource
    {
    public:
        explicit VkmAccelerationStructure(VkmDriverBase* driver);
        ~VkmAccelerationStructure() override;

        virtual bool initialize(VkmResourceHandle handle, const VkmAccelerationStructureInfo& info) = 0;

        inline const VkmAccelerationStructureInfo& getAccelerationStructureInfo() const { return _info; }
        inline VkmAccelerationStructureType getType() const { return _info._type; }
        VkmResourceType getResourceType() const override { return VkmResourceType::AccelerationStructure; }

        /*
        * @brief Replaces the instance list a later `buildAccelerationStructure` will read.
        * @details Top-level structures only, and only those created with `_allowUpdate`. Writes the
        * backend's instance descriptors into the buffer the build reads; the rebuild is recorded
        * separately, so a caller can update every structure and then record every build.
        * A successful call also replaces the retained info's instance list, so
        * `getAccelerationStructureInfo()` describes the list the next build reads; a refused call
        * leaves it untouched.
        * @param instances New instance list. Must be no longer than the list the structure was
        * created with, the buffer being sized once. A shorter list leaves the extra instances out
        * of the rebuilt structure.
        * @return False if the structure is not an updatable top-level one, or the list is too long.
        */
        virtual bool updateInstances(const std::vector<VkmAccelerationStructureInstance>& instances) = 0;

    protected:
        // Stores `info` and the handle. The vectors are kept because a rebuild is described by the
        // same geometry list, and because updateInstances validates its argument against the
        // instance count the structure was sized for.
        bool initializeAccelerationStructureCommon(VkmResourceHandle handle,
                                                   const VkmAccelerationStructureInfo& info);

        VkmAccelerationStructureInfo _info{};
    };
} // namespace vkm
