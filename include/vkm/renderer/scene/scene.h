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
        // Which VkmFrameData the cull pass tests against; see kVkmSceneMaxCullViews.
        uint32_t _frameDataIndex = 0;
        // Scalars rather than a uint2/uint3: WebGPU lowers push constants to a uniform buffer,
        // where a vector aligns to its own size, so a vector here would sit at a different offset
        // than it does in the scalar layout Vulkan and Metal use.
        uint32_t _pad0 = 0;
        uint32_t _pad1 = 0;
    };
    static_assert(sizeof(SceneBatchConstants) == 32, "SceneBatchConstants must match the shader-side struct");

    /*
    * @brief Material factors plus texture slots, as uploaded into the scene's material pool.
    *
    * glTF multiplies factor by texture, so a slot of INVALID_VALUE32 ("no texture") has to be
    * distinguishable from slot 0 or every untextured material samples whatever lives there.
    *
    * All four slots are present even though only base colour and metallic-roughness are sampled
    * today: the importer already produces four image references, and the alternative is paying the
    * same ABI churn twice (the record's word stride is mirrored in vkm_material.hlsli and pinned by
    * TestObjectDataLayout).
    */
    struct VkmMaterialData
    {
        glm::vec4 _baseColorFactor{ 1.0f, 1.0f, 1.0f, 1.0f };
        glm::vec4 _emissive{ 0.0f, 0.0f, 0.0f, 0.0f };            // xyz = emissive factor
        glm::vec4 _metallicRoughness{ 1.0f, 1.0f, 0.0f, 0.0f };   // x = metallic, y = roughness
        // Bindless texture-array slots: x = base colour, y = metallic-roughness, z = normal,
        // w = emissive. INVALID_VALUE32 where the material has no texture for that channel, which
        // is also what every slot holds on a backend without VkmDriverCapabilityFlags::TextureUpload.
        glm::uvec4 _textureSlots{ INVALID_VALUE32, INVALID_VALUE32, INVALID_VALUE32, INVALID_VALUE32 };
    };
    static_assert(sizeof(VkmMaterialData) == 64, "VkmMaterialData must match the shader-side material record");

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

    /*
    * @brief How many independently culled views one frame may record.
    *
    * @details Two, because a frame that refreshes GI probes needs a cull the probes can use
    * alongside the camera's: a probe looks in every direction, so culling its capture against the
    * camera frustum would drop exactly the geometry behind the camera that indirect light comes
    * from. Each view gets its own frame data, its own counts and its own payload regions, so the
    * two culls never write the same words and need no barrier between them.
    */
    constexpr uint32_t kVkmSceneMaxCullViews = 2;

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
        void recordUpdate(VkmCommandBufferBase* commandBuffer, uint32_t frameIndex,
                          const VkmFrameData& frameData, uint32_t viewIndex = 0);

        /*
        * @brief Records the frustum-culling pass and the emit pass that turns its output into
        * native indirect draw arguments. Must be recorded into a compute subgraph (outside any
        * render pass) after recordUpdate() and before the draws.
        *
        * Culling is one shared shader on every backend; only the emit step is backend-specific.
        * The two are separate dispatches with the compute pass closed and reopened between them,
        * which is what orders the emit's reads after the cull's compacting writes.
        */
        void recordCull(VkmCommandBufferBase* commandBuffer, uint32_t viewIndex = 0);

        /*
        * @brief Records the frame's draws, one PSO bind and one GPU-driven indirect draw per batch.
        * `pipelineResolver` maps a batch to the PSO permutation matching its vertex layout; a batch
        * whose resolver returns nullptr is skipped with a warning.
        */
        /*
        * @brief Records one indirect draw per batch, resolving each batch's pipeline through
        * `pipelineResolver`.
        *
        * `beforeDraw`, if set, runs after the batch's pipeline is bound and before its draw. That
        * is the only point at which per-draw state can be set, since push constants require a
        * bound pipeline and this method owns the binding. The probe capture uses it to push which
        * cube face is being rendered; the ordinary scene draw passes nothing at all.
        */
        void recordDrawBatches(VkmCommandBufferBase* commandBuffer,
                               const std::function<VkmPipelineStateBase*(const DrawBatch&)>& pipelineResolver,
                               const std::function<void(VkmCommandBufferBase*, const DrawBatch&)>& beforeDraw = {},
                               uint32_t viewIndex = 0);

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
        /*
        * @brief One material's texture references, as resolved file paths plus the colour space
        * each is sampled in.
        *
        * Held as paths from addModel() until build(), which is where a driver first exists. Colour
        * space is a property of the *channel that references* the image, not of the image: base
        * colour and emissive are sRGB-encoded, metallic-roughness and normal are linear data. The
        * upload cache is therefore keyed on (path, sRGB) so an image used as both gets two
        * textures rather than one that is wrong for one of them.
        */
        struct MaterialImageRefs
        {
            std::string _baseColor;
            std::string _metallicRoughness;
            std::string _normal;
            std::string _emissive;
        };

        // Decodes, uploads and registers every referenced image, filling _materials' texture slots.
        // Skipped entirely without VkmDriverCapabilityFlags::TextureUpload, which leaves every slot
        // invalid and every material factor-only -- what WebGPU gets until set 3 carries them.
        bool uploadMaterialTextures(VkmDriverBase* driver, std::string* outError);

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
        std::vector<MaterialImageRefs> _materialImages; // 1:1 with _materials
        // Textures this scene owns, one per (path, colour space) actually uploaded.
        std::vector<VkmResourceHandle> _materialTextures;
        std::vector<uint32_t> _materialTextureSlots; // 1:1 with _materialTextures
        std::vector<VkmObjectData> _objectData;
        std::vector<DrawBatch> _drawBatches;

        VkmResourceHandle _materialBuffer{ VKM_INVALID_RESOURCE_HANDLE };
        uint32_t _materialPoolSlot = INVALID_VALUE32;

        VkmResourceHandle _objectDataBuffer{ VKM_INVALID_RESOURCE_HANDLE };
        VkmResourceHandle _frameDataBuffer{ VKM_INVALID_RESOURCE_HANDLE };

        /*
        * The two GPU-driven bookkeeping buffers. Both start with one count word per batch per cull
        * view, so a batch's count lives at the same word index in each, followed by one payload
        * region per view:
        *
        *   visible list:  [counts: view 0 | view 1][view 0 object indices][view 1 ...]
        *   arguments:     [counts: view 0 | view 1][view 0 VkmDrawIndirectArguments][view 1 ...]
        *
        * The counts are gathered at the front rather than living beside each view's payload because
        * a batch uses ONE index into both buffers, whose payloads have different strides -- there
        * is no single per-view offset that would work for both otherwise.
        *
        * The cull pass compacts into the visible list; the emit pass reads it and fills the
        * arguments, zeroing the slots past the count so they draw nothing.
        */
        VkmResourceHandle _visibleListBuffer{ VKM_INVALID_RESOURCE_HANDLE };
        VkmResourceHandle _argumentBuffer{ VKM_INVALID_RESOURCE_HANDLE };
        // Zero-filled, sized to ONE view's count region: a cull resets only its own view's counts.
        VkmResourceHandle _countClearBuffer{ VKM_INVALID_RESOURCE_HANDLE };
        uint64_t _countRegionSize = 0;
        // Per-view strides within each buffer's payload, in u32 words.
        uint32_t _visibleViewStrideWords = 0;
        uint32_t _argumentViewStrideWords = 0;

        VkmPipelineStateBase* _cullPipeline = nullptr;
        VkmPipelineStateBase* _emitPipeline = nullptr;

        /*
        * The device-side ObjectData buffer is a single buffer at a fixed bindless binding, so an
        * object index is frame-invariant -- that is what keeps the draw path free of push
        * constants. The per-frame ring lives on the upload side instead: one staging buffer per
        * frame slot, laid out as [ObjectData array][FrameData per cull view], so the CPU never
        * writes memory a frame still in flight may be reading. One FrameData region per view is
        * what makes two culls per frame possible: both recordUpdate() calls write host memory
        * immediately, long before either GPU copy runs, so sharing one region would leave the
        * first cull reading the second view's frustum.
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
