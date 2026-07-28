// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/scene/scene_geometry_pool.h>
#include <vkm/renderer/scene/scene_model.h>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace vkm
{
    class VkmCommandBufferBase;
    class VkmDriverBase;
    class VkmPipelineStateBase;
    class VkmPipelineStateManager;
    class VkmStagingBuffer;

    /*
    * @brief Per-object GPU record.
    *
    * Mirrors the ObjectData struct in the scene shaders; the explicit padding is the std430-like
    * layout DXC emits for a StructuredBuffer element (a float4x4 and a float4 are both 16-byte
    * aligned). Any change here must be mirrored in every shader that declares ObjectData --
    * TestObjectDataLayout guards the offsets.
    */
    struct VkmObjectData
    {
        glm::mat4 _worldTransform{ 1.0f };  // offset   0
        glm::mat4 _normalTransform{ 1.0f }; // offset  64, inverse-transpose of _worldTransform
        uint32_t _vertexPoolSlot = 0;       // offset 128, bindless Buffer-array slot
        uint32_t _indexPoolSlot = 0;        // offset 132, bindless IndexBuffer-array slot
        uint32_t _vertexWordOffset = 0;     // offset 136, u32-word base of this mesh in its pool
        uint32_t _materialIndex = 0;        // offset 140
        uint32_t _indexOffset = 0;          // offset 144, element base of this mesh's indices
        uint32_t _indexCount = 0;           // offset 148, becomes the indirect draw's vertexCount
        uint32_t _pad0[2]{ 0, 0 };          // offset 152, aligns the following float4 to 16
        glm::vec4 _boundsCenterRadius{ 0.0f, 0.0f, 0.0f, 0.0f }; // offset 160, object space
    };
    static_assert(sizeof(VkmObjectData) == 176, "VkmObjectData must match the shader-side ObjectData layout");

    /*
    * @brief Per-frame GPU constants.
    *
    * Everything the draw path used to receive as push constants now lives here or in
    * VkmObjectData, which is what lets an indirect draw carry its object index in firstInstance
    * and the graphics pipelines push no constants at all.
    *
    * Deliberately carries no camera: that is descriptor set 1's job (VkmFrameConstants), which the
    * engine already rewrites once per frame and which every stage -- including the culling compute
    * pass -- can read. Only the per-frame data set 1 does not have lives here.
    */
    struct VkmFrameData
    {
        glm::vec4 _frustumPlanes[6]{};            // offset   0, world space, normalized, xyz = n, w = d
        glm::vec4 _lightDirection{ 0.0f, 1.0f, 0.0f, 0.0f }; // offset  96, world space, towards the light
        uint32_t _materialPoolSlot = 0;           // offset 112, bindless Buffer-array slot
        /*
        * offset 116. Carried through to the drawing shader untouched; 0 always means "shade
        * normally". Which visualisation any other value selects is the drawing shader's own
        * business -- the scene neither defines nor reads the meanings (see the model_viewer
        * sample's DebugMode).
        */
        uint32_t _debugMode = 0;
        uint32_t _pad0[2]{ 0, 0 };
    };
    static_assert(sizeof(VkmFrameData) == 128, "VkmFrameData must match the shader-side FrameData layout");

    /*
    * @brief Per-batch push constants for both scene compute passes.
    *
    * Mirrors SceneBatchConstants in resources/Shaders/scene_common.hlsli. A batch is a contiguous
    * run of VkmObjectData records, and its regions inside the shared visible-list and argument
    * buffers are addressed in u32 words so one buffer holds every batch's region back to back.
    */
    struct SceneBatchConstants
    {
        uint32_t _firstObject = 0;
        uint32_t _objectCount = 0;
        uint32_t _visibleWordOffset = 0;
        uint32_t _countWordOffset = 0;
        uint32_t _argumentWordOffset = 0;
        uint32_t _pad0[3]{ 0, 0, 0 };
    };
    static_assert(sizeof(SceneBatchConstants) == 32, "SceneBatchConstants must match the shader-side struct");

    // Material factors as uploaded into the scene's material pool.
    struct VkmMaterialData
    {
        glm::vec4 _baseColorFactor{ 1.0f, 1.0f, 1.0f, 1.0f };
        glm::vec4 _emissive{ 0.0f, 0.0f, 0.0f, 0.0f };            // xyz = emissive factor
        glm::vec4 _metallicRoughness{ 1.0f, 1.0f, 0.0f, 0.0f };   // x = metallic, y = roughness
    };
    static_assert(sizeof(VkmMaterialData) == 48, "VkmMaterialData must match the shader-side material record");

    // One placed instance of an imported mesh.
    struct VkmSceneObject
    {
        glm::mat4 _worldTransform{ 1.0f };
        uint32_t _meshEntryIndex = INVALID_VALUE32;
        // Batching key. 0 is the scene's default opaque pipeline; a caller assigns other ids when
        // it wants a distinct PSO for a subset of objects.
        uint32_t _pipelineId = 0;
    };

    /*
    * @brief Aggregates imported models into per-layout geometry pools plus a bindless per-object
    * attribute array, and produces a batched draw list.
    *
    * Extracts the frustum planes of a view-projection matrix in the engine's clip-space
    * convention (+Y up, [0,1] depth). Free-standing so the culling test can build the same planes
    * the scene does.
    */
    void vkmExtractFrustumPlanes(const glm::mat4& viewProjection, glm::vec4* outPlanes);

    class VkmScene
    {
    public:
        // One run of objects that share a pipeline and a vertex layout, so they share a PSO.
        struct DrawBatch
        {
            VkmVertexLayoutPreset _layout = VkmVertexLayoutPreset::StandardPBR;
            uint32_t _pipelineId = 0;
            uint32_t _firstObject = 0; // index into the sorted object array == ObjectData index
            uint32_t _objectCount = 0; // == this batch's maxDrawCount

            // Regions this batch owns inside the shared visible-list and argument buffers, in u32
            // words. Assigned by build(); see the layout comment on VkmScene's buffer members.
            uint32_t _countWordOffset = 0;     // the batch's visible count, in both buffers
            uint32_t _visibleWordOffset = 0;   // compacted object indices, in the visible list
            uint32_t _argumentWordOffset = 0;  // VkmDrawIndirectArguments records
        };

        // VkmResourceHandle has no default member initializer for its id, so the handle arrays
        // have to be filled explicitly rather than value-initialized (id 0 is a valid handle).
        VkmScene();
        ~VkmScene() = default;

        VkmScene(const VkmScene&) = delete;
        VkmScene& operator=(const VkmScene&) = delete;

        /*
        * @brief Appends every drawable (node, mesh) pair of `model` as a placed object, pooling its
        * geometry by vertex layout, and recomputes the draw batches. Callable repeatedly; all calls
        * must precede build().
        *
        * Driver-free, so getDrawBatches() is meaningful (and testable) before anything is uploaded.
        */
        bool addModel(const VkmSceneModel& model, std::string* outError);

        /*
        * @brief Uploads and registers the geometry pools, the material pool and the per-object /
        * per-frame attribute buffers, then computes the draw batches. Blocking -- setup time only.
        * On failure everything created so far is released again.
        */
        bool build(VkmDriverBase* driver, VkmPipelineStateManager* pipelineStateManager, std::string* outError);

        // Safe to call without idling the GPU: releases go through the deferred reclaimer.
        void destroy(VkmDriverBase* driver);

        // Widens the dirty range recordUpdate() uploads on the next frame.
        void setObjectTransform(uint32_t objectIndex, const glm::mat4& worldTransform);

        /*
        * @brief Records the copies that publish `frameData` and any dirty object transforms for
        * this frame. Must be recorded outside a render pass, before the draws that read them.
        */
        void recordUpdate(VkmCommandBufferBase* commandBuffer, uint32_t frameIndex, const VkmFrameData& frameData);

        /*
        * @brief Records the frustum-culling pass and the emit pass that turns its output into
        * native indirect draw arguments. Must be recorded into a compute subgraph (outside any
        * render pass) after recordUpdate() and before the draws.
        *
        * Culling is one shared shader on every backend; only the emit step is backend-specific.
        * The two are separate dispatches with the compute pass closed and reopened between them,
        * which is what orders the emit's reads after the cull's compacting writes.
        */
        void recordCull(VkmCommandBufferBase* commandBuffer);

        /*
        * @brief Records the frame's draws, one PSO bind and one GPU-driven indirect draw per batch.
        * `pipelineResolver` maps a batch to the PSO permutation matching its vertex layout; a batch
        * whose resolver returns nullptr is skipped with a warning.
        */
        void recordDrawBatches(VkmCommandBufferBase* commandBuffer,
                               const std::function<VkmPipelineStateBase*(const DrawBatch&)>& pipelineResolver);

        // Every buffer a frame's update and draws touch, for VkmRenderSubGraph::addReferencedResource.
        void collectReferencedResources(std::vector<VkmResourceHandle>* outHandles) const;

        inline const std::vector<DrawBatch>& getDrawBatches() const { return _drawBatches; }
        inline const std::vector<VkmSceneObject>& getObjects() const { return _objects; }
        /*
        * @brief The buffer the culling pass fills and the draws fetch from. Exposed so tooling and
        * tests can read back a batch's visible count, which lives at
        * `DrawBatch::_countWordOffset * sizeof(uint32_t)` -- the only externally observable proof
        * that culling actually rejected something, since anything the frustum planes reject would
        * also have been clipped and so cannot be told apart in the rendered pixels.
        */
        inline VkmResourceHandle getArgumentBuffer() const { return _argumentBuffer; }
        uint64_t getTotalIndexCount() const;

        // World-space bounds of every placed object; invalid when the scene is empty.
        VkmSceneAABB computeWorldBounds() const;

    private:
        // One mesh appended into a pool, plus the object-space data every instance of it shares.
        struct MeshEntry
        {
            VkmVertexLayoutPreset _layout = VkmVertexLayoutPreset::StandardPBR;
            VkmSceneGeometryPool::MeshRange _range{};
            uint32_t _materialIndex = 0;
            VkmSceneAABB _bounds;
        };

        // Sorts _objects by (pipelineId, layout, materialIndex) and emits one batch per
        // (pipelineId, layout) run. Material is a per-object index the shader reads out of
        // ObjectData rather than pipeline state, so it orders objects but never splits a batch.
        void buildDrawBatches();
        void fillObjectData();
        // Assigns each batch its word regions and returns the two buffers' sizes in bytes.
        void assignBatchRegions(uint64_t* outVisibleListSize, uint64_t* outArgumentSize);

        std::array<std::unique_ptr<VkmSceneGeometryPool>, static_cast<size_t>(VkmVertexLayoutPreset::Count)> _pools;
        std::vector<MeshEntry> _meshEntries;
        std::vector<VkmSceneObject> _objects;
        std::vector<VkmMaterialData> _materials;
        std::vector<VkmObjectData> _objectData;
        std::vector<DrawBatch> _drawBatches;

        VkmResourceHandle _materialBuffer{ VKM_INVALID_RESOURCE_HANDLE };
        uint32_t _materialPoolSlot = INVALID_VALUE32;

        VkmResourceHandle _objectDataBuffer{ VKM_INVALID_RESOURCE_HANDLE };
        VkmResourceHandle _frameDataBuffer{ VKM_INVALID_RESOURCE_HANDLE };

        /*
        * The two GPU-driven bookkeeping buffers. Both start with one count word per batch, so a
        * batch's count lives at the same word index in each, followed by that batch's own region:
        *
        *   visible list:  [count per batch][batch 0 object indices][batch 1 ...]
        *   arguments:     [count per batch][batch 0 VkmDrawIndirectArguments][batch 1 ...]
        *
        * The cull pass compacts into the visible list; the emit pass reads it and fills the
        * arguments, zeroing the slots past the count so they draw nothing.
        */
        VkmResourceHandle _visibleListBuffer{ VKM_INVALID_RESOURCE_HANDLE };
        VkmResourceHandle _argumentBuffer{ VKM_INVALID_RESOURCE_HANDLE };
        // Zero-filled, sized to the count region: one copy per frame resets every batch's count.
        VkmResourceHandle _countClearBuffer{ VKM_INVALID_RESOURCE_HANDLE };
        uint64_t _countRegionSize = 0;

        VkmPipelineStateBase* _cullPipeline = nullptr;
        VkmPipelineStateBase* _emitPipeline = nullptr;

        /*
        * The device-side ObjectData buffer is a single buffer at a fixed bindless binding, so an
        * object index is frame-invariant -- that is what keeps the draw path free of push
        * constants. The per-frame ring lives on the upload side instead: one staging buffer per
        * frame slot, laid out as [ObjectData array][FrameData], so the CPU never writes memory a
        * frame still in flight may be reading.
        */
        std::array<VkmResourceHandle, FRAME_BUFFER_COUNT> _stagingBuffers{};
        // Resolved once in build(): the staging buffers live as long as the scene does, and
        // VkmCommandBufferBase exposes no driver, so recordUpdate() cannot look them up per frame.
        std::array<VkmStagingBuffer*, FRAME_BUFFER_COUNT> _stagingPointers{};
        uint64_t _frameDataStagingOffset = 0;

        // Half-open dirty range of _objectData, in object indices. Empty when _dirtyFirst == _dirtyEnd.
        uint32_t _dirtyFirst = 0;
        uint32_t _dirtyEnd = 0;
    };
} // namespace vkm
