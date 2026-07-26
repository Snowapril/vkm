// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/scene/scene_model.h>

#include <string>
#include <vector>

namespace vkm
{
    class VkmDriverBase;

    /*
    * @brief One mega vertex buffer plus one mega index buffer for a single vertex layout preset.
    *
    * Meshes are appended CPU-side and the whole pool is uploaded and bindless-registered once, so
    * a scene costs two bindless slots per layout instead of two per primitive. Both buffers are
    * published as untyped u32 word arrays; shaders reach a vertex through the owning object's
    * word offset (see VkmObjectData::_vertexWordOffset and the DATA_WORD macro in the scene
    * shaders), which is what lets one pool hold any stride.
    *
    * Indices stay mesh-local rather than being rebased onto the pool: the per-object word offset
    * already supplies the base, so rebasing would double-count it.
    *
    * Append-then-upload rather than incremental upload is deliberate: the WebGPU bindless manager
    * copies a registered buffer's contents into its emulated mega-buffer at registerBuffer() time,
    * so a pool registered while still empty would publish stale bytes there. Registering only once
    * the pool is complete keeps all three backends on one code path.
    *
    * Meshlets (future): clusterized geometry adds a third pool of meshlet descriptors registered
    * as one more bindless slot here, and MeshRange gains _meshletOffset/_meshletCount. The
    * pool-per-layout, range-per-mesh shape does not otherwise change.
    */
    class VkmSceneGeometryPool
    {
    public:
        // Where one appended mesh ended up inside the pool.
        struct MeshRange
        {
            uint32_t _vertexWordOffset = 0; // u32-word offset of this mesh's vertex bytes
            uint32_t _vertexCount = 0;
            uint32_t _indexOffset = 0;      // element offset of this mesh's indices
            uint32_t _indexCount = 0;
        };

        explicit VkmSceneGeometryPool(VkmVertexLayoutPreset preset);

        /*
        * @brief Appends `mesh`'s vertex bytes and indices. Driver-free, so this half of the pool
        * is unit-testable without a GPU.
        *
        * `mesh` must be in this pool's layout. Returns false when appending would push a word or
        * element offset past UINT32_MAX (the width of the shader-side offsets).
        */
        bool appendMesh(const VkmSceneMesh& mesh, MeshRange* outRange, std::string* outError);

        /*
        * @brief Creates, fills and bindless-registers the accumulated bytes, then drops the CPU
        * staging copies. Blocking (uses VkmDriverBase::uploadToBuffer) -- call at setup time.
        * A pool with no geometry succeeds without creating anything.
        */
        bool upload(VkmDriverBase* driver, std::string* outError);

        // Releases through the deferred reclaimer, so callers do not have to idle the GPU first.
        void destroy(VkmDriverBase* driver);

        inline const VkmVertexLayout& getLayout() const { return _layout; }
        inline uint32_t getVertexPoolSlot() const { return _vertexPoolSlot; }
        inline uint32_t getIndexPoolSlot() const { return _indexPoolSlot; }
        inline VkmResourceHandle getVertexBuffer() const { return _vertexBuffer; }
        inline VkmResourceHandle getIndexBuffer() const { return _indexBuffer; }
        inline bool isEmpty() const { return _indexCount == 0; }

    private:
        VkmVertexLayout _layout{};

        // Cleared by upload(); the GPU copy is authoritative afterwards.
        std::vector<uint8_t> _vertexBytes;
        std::vector<uint32_t> _indices;
        // Survives upload() so the pool can still report its size for debug overlays.
        uint32_t _indexCount = 0;

        VkmResourceHandle _vertexBuffer{ VKM_INVALID_RESOURCE_HANDLE };
        VkmResourceHandle _indexBuffer{ VKM_INVALID_RESOURCE_HANDLE };
        uint32_t _vertexPoolSlot = INVALID_VALUE32;
        uint32_t _indexPoolSlot = INVALID_VALUE32;
    };
} // namespace vkm
