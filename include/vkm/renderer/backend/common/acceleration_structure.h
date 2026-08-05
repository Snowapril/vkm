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
    *
    * @details Both APIs model ray tracing the same way: a bottom-level structure holds geometry in
    * its own object space, and a top-level structure holds instances of bottom-level ones with a
    * transform each. Splitting them is what makes a moved object cost one instance-transform
    * rewrite instead of a geometry rebuild.
    */
    enum class VkmAccelerationStructureType : uint8_t
    {
        BottomLevel = 0,
        TopLevel    = 1,
    };

    /*
    * @brief One indexed triangle range inside a bottom-level structure.
    *
    * @details Deliberately a *range* into buffers the caller already owns rather than geometry of
    * its own: `VkmSceneGeometryPool` keeps every mesh's vertices and indices in one pair of
    * buffers, and a build reads them in place, so no vertex data is duplicated to trace it.
    *
    * **Triangles only.** Both backends can describe procedural/AABB geometry, but the Phase 1
    * spike found it compiles and is silently wrong at runtime through this toolchain
    * (restir.md section 4.2), so there is no shape for it here rather than a shape that cannot be
    * trusted.
    *
    * The vertex format is fixed at three consecutive `float`s at `_vertexByteOffset`, strided by
    * `_vertexStride`. That matches every `VkmVertexLayoutPreset`, whose position is always the
    * first attribute -- see `vkm_vertex_layout.h`.
    */
    struct VkmAccelerationStructureGeometry
    {
        VkmResourceHandle _vertexBuffer{};
        uint64_t          _vertexByteOffset = 0;
        uint32_t          _vertexStride = 0;
        uint32_t          _vertexCount = 0;

        VkmResourceHandle _indexBuffer{};
        uint64_t          _indexByteOffset = 0;
        // Indices are u32, matching VkmSceneGeometryPool's index pool. Must be a multiple of 3.
        uint32_t          _indexCount = 0;
    };

    /*
    * @brief One bottom-level structure placed into a top-level one.
    *
    * @details `_transform` is a full 4x4 for the caller's convenience; only the upper 3x4 reaches
    * the API, because neither backend's instance descriptor carries a projective row. A transform
    * with a non-affine bottom row is therefore silently truncated, which is why callers should
    * pass object-to-world matrices and nothing else.
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
    };

    /*
    * @brief A built acceleration structure.
    *
    * @details Built synchronously by `VkmDriverBase::newAccelerationStructure`, in the same way
    * `uploadToBuffer` uploads: a one-off command buffer submitted and waited on. That suits how it
    * is used today -- a scene builds its structures once at load -- and keeps this slice free of
    * command-buffer API changes. Rebuilding a moved scene per frame needs a recorded build instead,
    * which is a later slice and is recorded in `TODO.md`.
    *
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

    protected:
        // Stores `info` and the handle. The vectors are kept because a later refit needs the
        // instance list to rewrite, and because the memory report wants the geometry count.
        bool initializeAccelerationStructureCommon(VkmResourceHandle handle,
                                                   const VkmAccelerationStructureInfo& info);

        VkmAccelerationStructureInfo _info{};
    };
} // namespace vkm
