// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/scene/light_table.h>
#include <vkm/renderer/scene/scene_geometry_pool.h>
#include <vkm/renderer/scene/scene_model.h>
#include <vkm/renderer/scene/texture_streamer.h>

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
    class VkmBindlessResourceManagerBase;
    class VkmDriverBase;
    class VkmPipelineStateBase;
    class VkmPipelineStateManager;
    class VkmStagingBuffer;
    // Forward-declared rather than included: only a pointer and a vector-of-them appear here, so
    // the acceleration structure header stays out of everything that draws a scene.
    class VkmAccelerationStructure;
    struct VkmAccelerationStructureInstance;

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
        /*
        * offset 152, u32 words per vertex in this object's pool.
        * The rasterizing shaders do not read it, a draw knowing its layout at compile time from the
        * PSO permutation it was built as. A ray-tracing shader cannot: one ray hits whatever is
        * there, across pools of different strides. Carrying the stride lets a single path-tracing
        * kernel fetch positions out of any layout, position being attribute 0 of every
        * VkmVertexLayoutPreset.
        */
        uint32_t _vertexStrideWords = 0;
        uint32_t _pad0 = 0;                 // offset 156, aligns the following float4 to 16
        glm::vec4 _boundsCenterRadius{ 0.0f, 0.0f, 0.0f, 0.0f }; // offset 160, object space
    };
    static_assert(sizeof(VkmObjectData) == 176, "VkmObjectData must match the shader-side ObjectData layout");

    /*
    * @brief Per-frame GPU constants.
    * @details Everything the draw path needs beyond VkmObjectData lives here, which lets an
    * indirect draw carry its object index in firstInstance and the graphics pipelines push no
    * constants at all.
    * Carries no camera: that is descriptor set 1's job (VkmFrameConstants), which the engine
    * rewrites once per frame and every stage, including the culling compute pass, can read. Only
    * the per-frame data set 1 does not have lives here.
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
        // offset 120/124: the scene's emissive-triangle light table -- bindless Buffer-array slot
        // and record count past the header. Overwritten by recordUpdate() from the scene's own
        // registration, like _materialPoolSlot; a caller never fills them.
        uint32_t _lightPoolSlot = 0;
        uint32_t _lightCount = 0;
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
    * @details glTF multiplies factor by texture, so a slot of INVALID_VALUE32, meaning no texture,
    * has to be distinguishable from slot 0, or every untextured material samples whatever lives
    * there. All four slots are present even though only base colour and metallic-roughness are
    * sampled: the importer produces four image references, and the record's word stride is mirrored
    * in vkm_material.hlsli and pinned by TestObjectDataLayout.
    */
    struct VkmMaterialData
    {
        glm::vec4 _baseColorFactor{ 1.0f, 1.0f, 1.0f, 1.0f };
        glm::vec4 _emissive{ 0.0f, 0.0f, 0.0f, 0.0f };            // xyz = emissive factor
        /*
        * x = metallic, y = roughness.
        * z = the base-colour texture's currently streamed base mip level, w = how many levels its
        * full chain has. Both are the base-colour channel's alone: a material's four textures
        * stream independently, and one pair of words cannot describe all of them. They exist for
        * the streaming debug view, and stay 0 wherever nothing is streamed.
        */
        glm::vec4 _metallicRoughness{ 1.0f, 1.0f, 0.0f, 0.0f };
        // Bindless texture-array slots: x = base colour, y = metallic-roughness, z = normal,
        // w = emissive. INVALID_VALUE32 where the material has no texture for that channel, which
        // is also what every slot holds on a backend without VkmDriverCapabilityFlags::TextureUpload.
        glm::uvec4 _textureSlots{ INVALID_VALUE32, INVALID_VALUE32, INVALID_VALUE32, INVALID_VALUE32 };
        /*
        * Finest mip level each of those four textures actually has memory for, in that texture's
        * own level numbering. Zero unless the texture is sparse: a rebuilt texture is physically
        * only as large as what it holds, so its level 0 is always backed, while a sparse one keeps
        * its full extent and the levels streamed off the front have had their tiles taken away.
        * The shader passes this as the sample's min-LOD clamp, which is what keeps them unread.
        */
        glm::uvec4 _streamingMinLod{ 0, 0, 0, 0 };
    };
    static_assert(sizeof(VkmMaterialData) == 80, "VkmMaterialData must match the shader-side material record");

    // One placed instance of an imported mesh.
    struct VkmSceneObject
    {
        glm::mat4 _worldTransform{ 1.0f };
        uint32_t _meshEntryIndex = INVALID_VALUE32;
        // Batching key. 0 is the scene's default opaque pipeline; a caller assigns other ids when
        // it wants a distinct PSO for a subset of objects.
        uint32_t _pipelineId = 0;
        // Which addModel() call placed this object. Batching reorders _objects, so a model's
        // objects are not a contiguous run and this is the only way to name them afterwards.
        uint32_t _modelIndex = INVALID_VALUE32;
    };

    /*
    * @brief Aggregates imported models into per-layout geometry pools plus a bindless per-object
    * attribute array, and produces a batched draw list.
    *
    * Extracts the frustum planes of a view-projection matrix in the engine's clip-space
    * convention (+Y up, [0,1] depth). Free-standing so the culling test can build the same planes
    * the scene does.
    */
    /*
    * @brief How much of the scene one draw batch may span, as a fraction of the scene's diagonal.
    * @details A batch is the unit a viewpoint accepts or rejects whole, so its size is a trade:
    * larger batches encode fewer draws, smaller ones can actually be culled. A material used
    * across a whole model produces a batch that spans it and can never be rejected, which is what
    * this bounds.
    */
    inline constexpr float kBatchSplitFraction = 0.15f;

    void vkmExtractFrustumPlanes(const glm::mat4& viewProjection, glm::vec4* outPlanes);

    /*
    * @brief The six axis-aligned planes of `boxMin`..`boxMax`, in the same inward-facing form
    * vkmExtractFrustumPlanes produces.
    * @details For a view no single frustum describes. A probe capture sees in every direction and
    * all six of its faces share one cull result; a shadow atlas pass fills several lights' tiles
    * from one cull. In both cases a box around what the pass will draw from is the tightest
    * correct test available.
    * @param boxMin Minimum corner.
    * @param boxMax Maximum corner.
    * @param outPlanes Receives six planes, matching VkmFrameData::_frustumPlanes.
    */
    void vkmBuildBoxPlanes(const glm::vec3& boxMin, const glm::vec3& boxMax, glm::vec4* outPlanes);

    /*
    * @brief How many independently culled views one frame may record.
    *
    * @details Three: the camera's, a GI probe refresh's, and a shadow atlas pass's. A probe looks
    * in every direction, so culling its capture against the camera frustum would drop exactly the
    * geometry behind the camera that indirect light comes from; a shadow pass draws from each
    * light rather than from the eye, so it needs its own cull for the same reason. Each view gets
    * its own frame data, its own counts and its own payload regions, so the culls never write the
    * same words and need no barrier between them -- which is also what makes raising this a
    * matter of buffer size alone.
    */
    constexpr uint32_t kVkmSceneMaxCullViews = 3;

    class VkmScene
    {
    public:
        // One run of objects that share a pipeline, a vertex layout and a material -- so they
        // share a PSO, and a per-draw material table if the backend needs one.
        struct DrawBatch
        {
            VkmVertexLayoutPreset _layout = VkmVertexLayoutPreset::StandardPBR;
            uint32_t _pipelineId = 0;
            // Every object in the batch has this material; see buildDrawBatches().
            uint32_t _materialIndex = 0;
            uint32_t _firstObject = 0; // index into the sorted object array == ObjectData index
            uint32_t _objectCount = 0; // == this batch's maxDrawCount

            // Regions this batch owns inside the shared visible-list and argument buffers, in u32
            // words. Assigned by build(); see the layout comment on VkmScene's buffer members.
            uint32_t _countWordOffset = 0;     // the batch's visible count, in both buffers
            uint32_t _visibleWordOffset = 0;   // compacted object indices, in the visible list
            uint32_t _argumentWordOffset = 0;  // VkmDrawIndirectArguments records

            /*
            * World-space bounding sphere of every object in the batch. A batch is a material run,
            * so this is only tight when a material's objects sit together; a material used across
            * the whole model gives a sphere the size of the model, and culling against it removes
            * nothing. Radius 0 means the batch had no valid bounds and must be treated as visible.
            */
            glm::vec3 _boundsCenter{ 0.0f, 0.0f, 0.0f };
            float _boundsRadius = 0.0f;
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

        /*
        * @brief Builds one bottom-level structure per pooled mesh and one top-level structure over
        * the placed objects. Must follow build(); blocking, like it.
        * @details No vertex data is duplicated to trace it: a bottom-level structure is described
        * as a range into the geometry pool's own buffers, which `VkmSceneGeometryPool::MeshRange`
        * already carries. Position is the first attribute of every vertex layout preset, so the
        * pool's stride is all a triangle geometry needs.
        * Separate from build() so a scene that is only rasterized does not pay for structures
        * nothing traverses. The top-level structure is created rebuildable, an object that moves
        * changing only its instance transform; bottom-level structures are built once, a mesh's
        * geometry being unchanged in object space wherever the object goes.
        * @param driver Driver that allocates and builds the structures.
        * @param outError Receives the failure reason.
        * @return False on a driver without `VkmDriverCapabilityFlags::RayTracing`, rather than
        * silently doing nothing, and on any build failure.
        */
        bool buildAccelerationStructures(VkmDriverBase* driver, std::string* outError);

        /*
        * @brief Republishes the object transforms and records the top-level rebuild.
        *
        * Must be recorded outside a render pass. Rebuilds unconditionally: which frames need one
        * is the caller's decision, and a top-level rebuild over the instance list is cheap.
        */
        void recordAccelerationStructureUpdate(VkmCommandBufferBase* commandBuffer);

        // Invalid until buildAccelerationStructures() succeeds. This is what a ray query traverses.
        inline VkmResourceHandle getTopLevelAccelerationStructure() const { return _topLevelStructure; }

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
        * native indirect draw arguments.
        * @details Must be recorded into a compute subgraph, outside any render pass, after
        * recordUpdate() and before the draws. Culling is one shared shader on every backend; only
        * the emit step is backend-specific. The two are separate dispatches with the compute pass
        * closed and reopened between them, which orders the emit's reads after the cull's
        * compacting writes.
        * @param commandBuffer Command buffer to record into.
        * @param viewIndex Which cull view this pass fills.
        */
        /*
        * @brief Brings every batch's world bounds back in line with its objects' transforms.
        * @details A no-op unless setObjectTransform() has been called since the last one. Called
        * by recordUpdate(), so a frame's draws always cull against current bounds; public because
        * a caller that moves objects and inspects the bounds without recording a frame has no
        * other way to ask.
        */
        void refreshBatchBounds();

        void recordCull(VkmCommandBufferBase* commandBuffer, uint32_t viewIndex = 0);

        /*
        * @brief Records the frame's draws: one PSO bind and one GPU-driven indirect draw per batch.
        * @param commandBuffer Command buffer to record into.
        * @param pipelineResolver Maps a batch to the PSO permutation matching its vertex layout. A
        * batch it returns nullptr for is skipped with a warning.
        * @param beforeDraw Runs after the batch's pipeline is bound and before its draw, the only
        * point at which per-draw state can be set, push constants requiring a bound pipeline. The
        * probe capture uses it to push which cube face is being rendered.
        * @param viewIndex Which cull view's results to draw.
        * @param batchFilter Optional. A batch it returns false for is not drawn at all -- no
        * pipeline bind, no push, no indirect draw. For a caller that renders one cull result from
        * several viewpoints, like the probe capture's six cube faces, this is the only place a
        * per-viewpoint decision can be made: the GPU cull runs once per view, not once per
        * viewport. Skipping is silent, unlike the nullptr-pipeline path, because a batch a
        * viewpoint cannot see is the expected case rather than a misconfiguration.
        */
        void recordDrawBatches(VkmCommandBufferBase* commandBuffer,
                               const std::function<VkmPipelineStateBase*(const DrawBatch&)>& pipelineResolver,
                               const std::function<void(VkmCommandBufferBase*, const DrawBatch&)>& beforeDraw = {},
                               uint32_t viewIndex = 0,
                               const std::function<bool(const DrawBatch&)>& batchFilter = {});

        /*
        * @brief Which of a frame's three scene subgraphs is asking. The same buffer is touched
        * differently by each -- the object data a transfer subgraph copies into is the same one
        * the cull dispatch and the draws read -- so the access a declaration carries depends on
        * the phase, not just the buffer.
        */
        enum class ReferencePhase : uint8_t
        {
            Update, // recordUpdate: the staging copies
            Cull,   // recordCull: the count clear, the cull dispatch and the emit dispatch
            Draw,   // recordDrawBatches: the indirect draws
        };

        /*
        * @brief Every buffer `phase` touches, tagged with how it touches it, for
        * VkmRenderSubGraph::addReferencedResource. Appends to `outDeclarations`.
        */
        void collectReferencedResources(ReferencePhase phase,
                                        std::vector<VkmResourceAccessDeclaration>* outDeclarations) const;

        inline const std::vector<DrawBatch>& getDrawBatches() const { return _drawBatches; }
        inline const std::vector<VkmSceneObject>& getObjects() const { return _objects; }
        // How many models addModel() has placed; a VkmSceneObject::_modelIndex is below this.
        inline uint32_t getModelCount() const { return _modelCount; }
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

        // The textures one material samples, for a backend that binds them per draw rather than
        // indexing a bindless array. Invalid where the material has no texture for that channel.
        struct MaterialTextures
        {
            VkmResourceHandle _baseColor{ VKM_INVALID_RESOURCE_HANDLE };
            VkmResourceHandle _metallicRoughness{ VKM_INVALID_RESOURCE_HANDLE };
            VkmResourceHandle _emissive{ VKM_INVALID_RESOURCE_HANDLE };
        };

        inline uint32_t getMaterialCount() const { return static_cast<uint32_t>(_materials.size()); }

        /*
        * @brief Sets the scene's directional light: the single place its direction and radiance
        * are stated.
        * @details Must precede build() -- the light table uploads once. Both halves live here so
        * they cannot disagree: the radiance reaches the traced tier through the light table's
        * header and the raster tier through vkmBuildDeferredLightConstants, and the direction
        * reaches every per-frame consumer through getDirectionalDirection().
        * @param directionToLight World-space direction TOWARDS the light; normalized here.
        * @param radiance Colour times intensity.
        */
        void setDirectionalLight(const glm::vec3& directionToLight, const glm::vec3& radiance);

        inline const glm::vec3& getDirectionalDirection() const { return _directionalDirection; }
        inline const glm::vec3& getDirectionalRadiance() const { return _directionalRadiance; }

        // Emissive triangles the built light table holds, past its header. CPU-known after
        // build(), so tests can assert the gather without a GPU readback.
        inline uint32_t getLightTriangleCount() const { return _lightTriangleCount; }

        /*
        * @brief Every punctual light the scene's models placed, in world space.
        * @details Filled by addModel from each model's node hierarchy, so it is valid before
        * build(). The scene's own directional light is NOT in here -- it is not a model's light;
        * vkmBuildDeferredLightConstants appends it.
        */
        inline const std::vector<VkmPunctualLight>& getPunctualLights() const { return _punctualLights; }

        /*
        * @brief The textures a material samples, for building a per-draw (set 3) table.
        *
        * @details Only meaningful on a backend without VkmDriverCapabilityFlags::BindlessTextures;
        * elsewhere the shader indexes the bindless array from the slots in VkmMaterialData and
        * these are the same textures reached the other way. A channel with no texture is
        * VKM_INVALID_RESOURCE_HANDLE, which a table builder substitutes a placeholder for -- an
        * unbound entry is a validation error, not a silently absent one.
        */
        MaterialTextures getMaterialTextures(uint32_t materialIndex) const;

        /*
        * @brief Moves each material texture towards the mip range this camera needs.
        *
        * @details Call once per frame, before the frame records anything: the streamer creates and
        * uploads textures through the driver directly, which a recording command buffer cannot be
        * open across, and its retire delay is counted in calls to this.
        * A rebuilt texture lands on a new bindless slot, so this also rewrites the affected
        * material records and widens the material dirty range recordUpdate() uploads.
        * A no-op without VkmDriverCapabilityFlags::BindlessTextures -- there is no slot to
        * re-point, and the per-draw tables that stand in for one are immutable once built.
        *
        * @param driver Driver the scene was built against.
        * @param view Where the camera is and how wide its projection is this frame.
        */
        void updateTextureStreaming(VkmDriverBase* driver, const VkmTextureStreamingView& view);

        inline const VkmTextureStreamingSettings& getTextureStreamingSettings() const
        {
            return _textureStreamer.getSettings();
        }
        inline void setTextureStreamingSettings(const VkmTextureStreamingSettings& settings)
        {
            _textureStreamer.setSettings(settings);
        }
        // False where the backend has no bindless texture array, which is where streaming cannot run.
        inline bool isTextureStreamingAvailable() const { return _textureStreamingAvailable; }

        /*
        * @brief What the streamed textures occupy now, beside what their full chains would.
        * @details Cheap enough for a per-frame UI readout. Call from the thread that drives
        * updateTextureStreaming(); see VkmTextureStreamer::computeStats.
        */
        inline VkmTextureStreamingStats getTextureStreamingStats() const
        {
            return _textureStreamer.computeStats();
        }

        /*
        * @brief The mip level a material channel's texture currently starts at.
        * @details The only externally observable proof that a rebuild actually happened -- the slot
        * and the texture handle are both internal, and the rendered pixels of a mip chain do not
        * have to change when the level does.
        * @param materialIndex Material to query.
        * @param channel 0 base colour, 1 metallic-roughness, 2 normal, 3 emissive.
        * @return The resident base level, or INVALID_VALUE32 where that channel streams no texture.
        */
        uint32_t getStreamedBaseMip(uint32_t materialIndex, uint32_t channel) const;

    private:
        /*
        * @brief One material's texture references, as resolved file paths plus the colour space
        * each is sampled in.
        * @details Held as paths from addModel() until build(), where a driver first exists. Colour
        * space is a property of the channel that references the image rather than of the image:
        * base colour and emissive are sRGB-encoded, metallic-roughness and normal are linear data.
        * The upload cache is keyed on (path, sRGB), so an image used as both gets two textures
        * rather than one that is wrong for one of them.
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

        // Widens the half-open material dirty range recordUpdate() uploads, the way
        // setObjectTransform() widens the object one.
        void markMaterialDirty(uint32_t materialIndex);
        // Writes one material channel's streamed level into the record, for the debug view.
        void publishStreamingMip(uint32_t materialIndex, uint32_t channel, uint32_t baseMip,
                                 uint32_t totalMipCount);

        // One mesh appended into a pool, plus the object-space data every instance of it shares.
        struct MeshEntry
        {
            VkmVertexLayoutPreset _layout = VkmVertexLayoutPreset::StandardPBR;
            VkmSceneGeometryPool::MeshRange _range{};
            uint32_t _materialIndex = 0;
            VkmSceneAABB _bounds;
            // Object-space emissive triangles, gathered at addModel while the model still owns
            // its CPU vertex bytes (the pool clears its own at upload). Expanded per placed
            // object in build(), where the world transforms are final.
            std::vector<VkmLightTableTriangle> _emissiveTriangles;
        };

        // Sorts _objects by (pipelineId, layout, materialIndex) and emits one batch per run of
        // all three. The shader still reads the material index out of ObjectData; the split
        // exists so a backend that binds material textures per draw has somewhere to bind them.
        void buildDrawBatches();
        // Recomputes one batch's world bounding sphere from its objects' current transforms.
        void updateBatchBounds(DrawBatch& batch);

        void fillObjectData();
        bool buildLightTable(VkmDriverBase* driver, VkmBindlessResourceManagerBase* bindlessManager,
                             std::string* outError);
        // Assigns each batch its word regions and returns the two buffers' sizes in bytes.
        void assignBatchRegions(uint64_t* outVisibleListSize, uint64_t* outArgumentSize);

        // One instance per placed object whose mesh has a bottom-level structure, in object order.
        void collectInstances(std::vector<VkmAccelerationStructureInstance>* outInstances) const;
        // No-op when nothing was built, so destroy() and the build's own rollback share it.
        void releaseAccelerationStructures(VkmDriverBase* driver);

        std::array<std::unique_ptr<VkmSceneGeometryPool>, static_cast<size_t>(VkmVertexLayoutPreset::Count)> _pools;
        std::vector<MeshEntry> _meshEntries;
        std::vector<VkmSceneObject> _objects;
        uint32_t _modelCount = 0; // addModel() calls so far; stamped into VkmSceneObject::_modelIndex
        std::vector<VkmMaterialData> _materials;
        std::vector<MaterialImageRefs> _materialImages; // 1:1 with _materials
        /*
        * Textures this scene owns directly, one per (path, colour space) actually uploaded.
        * Only the no-bindless path fills it: elsewhere VkmTextureStreamer owns them, a rebuild
        * replacing both the texture and its slot.
        */
        std::vector<VkmResourceHandle> _materialTextures;
        std::vector<MaterialTextures> _materialTextureHandles; // 1:1 with _materials
        // Set when an object moves; cleared by refreshBatchBounds().
        bool _batchBoundsDirty = false;
        std::vector<VkmObjectData> _objectData;
        std::vector<DrawBatch> _drawBatches;

        VkmResourceHandle _materialBuffer{ VKM_INVALID_RESOURCE_HANDLE };
        uint32_t _materialPoolSlot = INVALID_VALUE32;

        VkmResourceHandle _lightBuffer{ VKM_INVALID_RESOURCE_HANDLE };
        uint32_t _lightPoolSlot = INVALID_VALUE32;
        uint32_t _lightTriangleCount = 0;
        glm::vec3 _directionalRadiance{ 0.0f };
        glm::vec3 _directionalDirection{ 0.0f, 1.0f, 0.0f };
        std::vector<VkmPunctualLight> _punctualLights;

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

        // Ray tracing, and empty unless buildAccelerationStructures() was called. One bottom-level
        // structure per mesh entry (1:1 with _meshEntries), instanced by the top-level one; an
        // instance's id is its object index, which is also its VkmObjectData index, so a hit
        // recovers the object it belongs to without a side table.
        std::vector<VkmResourceHandle> _meshStructures;
        VkmResourceHandle _topLevelStructure{ VKM_INVALID_RESOURCE_HANDLE };
        // Resolved once, for the same reason _stagingPointers is: the per-frame update runs from a
        // VkmCommandBufferBase, which exposes no driver to look the handle up through.
        VkmAccelerationStructure* _topLevelStructurePointer = nullptr;

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
        // Named explicitly rather than left to `{}`, which value-initializes each entry to id 0 --
        // a handle destroy() would hand to the reclaimer, releasing whatever really owns slot 0.
        std::array<VkmResourceHandle, FRAME_BUFFER_COUNT> _stagingBuffers{
            VKM_INVALID_RESOURCE_HANDLE, VKM_INVALID_RESOURCE_HANDLE, VKM_INVALID_RESOURCE_HANDLE };
        static_assert(FRAME_BUFFER_COUNT == 3, "_stagingBuffers' initializer names one handle per slot");
        // Resolved once in build(): the staging buffers live as long as the scene does, and
        // VkmCommandBufferBase exposes no driver, so recordUpdate() cannot look them up per frame.
        std::array<VkmStagingBuffer*, FRAME_BUFFER_COUNT> _stagingPointers{};
        uint64_t _frameDataStagingOffset = 0;

        // Half-open dirty range of _objectData, in object indices. Empty when _dirtyFirst == _dirtyEnd.
        uint32_t _dirtyFirst = 0;
        uint32_t _dirtyEnd = 0;

        // The same, for _materials: a streamed texture lands on a new bindless slot, so its
        // records have to reach the GPU again.
        uint32_t _materialDirtyFirst = 0;
        uint32_t _materialDirtyEnd = 0;
        uint64_t _materialStagingOffset = 0;

        VkmTextureStreamer _textureStreamer;
        bool _textureStreamingAvailable = false;

        /*
        * Texture streaming's GPU feedback channel: one u32 per bindless texture slot, written by
        * the G-buffer pass and read back a few frames later.
        *
        * The ring is one longer than the frame count on purpose. At the point this is read -- the
        * top of updateTextureStreaming, before the frame records anything -- the newest frame
        * provably complete is FRAME_BUFFER_COUNT + 1 back, because this frame's own wait has not
        * happened yet. A ring of exactly FRAME_BUFFER_COUNT would hand back a slot still in
        * flight; one more slot makes the read safe without a single extra wait.
        */
        static constexpr uint32_t kFeedbackRingSize = FRAME_BUFFER_COUNT + 1;
        VkmResourceHandle _feedbackBuffer{ VKM_INVALID_RESOURCE_HANDLE };
        // Zero-filled source for the per-frame reset, the same trick _countClearBuffer uses.
        VkmResourceHandle _feedbackClearBuffer{ VKM_INVALID_RESOURCE_HANDLE };
        // Explicit for the same reason _stagingBuffers is.
        std::array<VkmResourceHandle, kFeedbackRingSize> _feedbackStaging{
            VKM_INVALID_RESOURCE_HANDLE, VKM_INVALID_RESOURCE_HANDLE, VKM_INVALID_RESOURCE_HANDLE,
            VKM_INVALID_RESOURCE_HANDLE };
        static_assert(kFeedbackRingSize == 4, "_feedbackStaging's initializer names one handle per ring slot");
        std::array<VkmStagingBuffer*, kFeedbackRingSize> _feedbackStagingPointers{};
        // Monotonic, incremented once per updateTextureStreaming, which indexes the ring.
        uint64_t _feedbackFrameCounter = 0;
        std::vector<uint32_t> _feedbackScratch;
        // Scratch for updateTextureStreaming, kept so a per-frame pass allocates nothing.
        std::vector<VkmTextureStreamingObject> _streamingObjects;
        std::vector<VkmTextureStreamingUpdate> _streamingUpdates;
    };
} // namespace vkm
