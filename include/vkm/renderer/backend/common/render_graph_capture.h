// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/render_pass.h>
#include <vkm/renderer/backend/common/render_graph.h>

#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace vkm
{
    class VkmDriverBase;
    class VkmCommandBufferBase;

    // Identity/metadata of one resource referenced by a captured pass.
    struct VkmCapturedResourceInfo
    {
        VkmResourceHandle handle = VKM_INVALID_RESOURCE_HANDLE;
        VkmResourceType type = VkmResourceType::Undefined;
        std::string debugName;
        // How the pass touched it. Declared accesses come from the subgraph; an attachment's is
        // synthesized from the frame buffer descriptor, the same way VkmRenderGraph::compile()
        // derives it.
        VkmResourceAccess access = VkmResourceAccess::None;
        // Texture-only fields
        VkmFormat format = VkmFormat::Undefined;
        glm::uvec3 extent{0, 0, 0};
        uint32_t numArrayLayers = 1;
        VkmTextureType textureType = VkmTextureType::Auto;
        // Capture-owned copy of this texture's contents at pass time. Stays invalid when the
        // texture could not be snapshotted (unsupported backend, swapchain back buffer,
        // depth/stencil format, 3D, or over the capture's snapshot budget) -- the inspector
        // then falls back to showing the live texture.
        VkmResourceHandle snapshotTexture = VKM_INVALID_RESOURCE_HANDLE;
        // Buffer-only field
        uint64_t size = 0;
    };

    // One output attachment of a captured graphics pass.
    struct VkmCapturedAttachment
    {
        VkmCapturedResourceInfo info;
        VkmLoadAction loadAction = VkmLoadAction::DontCare;
        VkmStoreAction storeAction = VkmStoreAction::DontCare;
        // True when the attachment is a swapchain back buffer (AllowPresent). Those cannot be
        // copy sources (CAMetalLayer.framebufferOnly stays YES), so info.snapshotTexture
        // stays invalid for them.
        bool isPresentTarget = false;
    };

    // CPU copy of (a prefix of) one buffer referenced by a captured pass.
    struct VkmCapturedBuffer
    {
        VkmCapturedResourceInfo info;
        std::vector<uint8_t> data; // filled in finalize(); capped at kMaxCapturedBufferBytes
        // Transient readback staging buffer; valid only between recordSubGraph() and
        // finalize().
        VkmResourceHandle stagingBuffer = VKM_INVALID_RESOURCE_HANDLE;
    };

    // Everything recorded about one subgraph of the captured frame.
    struct VkmCapturedPass
    {
        uint32_t subGraphId = 0;
        std::string name;
        VkmRenderSubGraphType type = VkmRenderSubGraphType::Graphics;
        std::vector<uint32_t> dependencies;
        std::vector<std::string> pipelineNames;
        uint32_t width = 0, height = 0;
        std::vector<VkmCapturedAttachment> colorAttachments;
        std::optional<VkmCapturedAttachment> depthStencilAttachment; // metadata only, never snapshotted
        std::vector<VkmCapturedResourceInfo> referencedResources;
        std::vector<VkmCapturedBuffer> capturedBuffers;
    };

    /*
    * @brief One-shot capture of a single frame's render graph for debug inspection.
    * @details arm() flags the next graph. The engine passes this object to exactly one
    * VkmRenderGraph::execute() via VkmRenderGraphCommitOptions::capture, which calls beginCapture()
    * and then recordSubGraph() after each subgraph's commit(). Metadata is always recorded; when
    * the driver reports TextureContentCapture, so are post-pass snapshot copies of color
    * attachments and staging readback copies of referenced buffers, into the same command buffer.
    * Once the submit completes, finalize() maps the staging readbacks and the capture becomes Ready.
    */
    class VkmRenderGraphCapture
    {
    public:
        enum class State : uint8_t
        {
            Idle,    // no capture data
            Armed,   // capture requested; next executed graph will be recorded
            Pending, // recording/GPU in flight; waiting for finalize()
            Ready    // capture data complete and inspectable
        };

        /*
        * @brief Request a capture of the next executed render graph.
        * @details Any previous capture's data stays readable until that graph's beginCapture()
        * releases it.
        */
        void arm();

        State getState() const { return _state; }
        const std::vector<VkmCapturedPass>& getPasses() const { return _passes; }
        uint32_t getCapturedFrameIndex() const { return _capturedFrameIndex; }
        // True when the capturing driver reported TextureContentCapture at capture time.
        bool hasContentCapture() const { return _hasContentCapture; }
        // Capture-owned snapshot texture handles of the current capture, for GPU-usage
        // tracking while the inspector displays them (see VkmEngine::render()).
        const std::vector<VkmResourceHandle>& getSnapshotTextureHandles() const { return _snapshotTextures; }

        /*
        * @brief Called by VkmRenderGraph::execute() when armed.
        * @details Releases the previous capture's resources and moves Armed -> Pending.
        * @param driver Driver executing the graph.
        * @param frameIndex Frame slot being captured.
        */
        void beginCapture(VkmDriverBase* driver, uint32_t frameIndex);

        /*
        * @brief Called by VkmRenderGraph::execute() right after subGraph->commit(), outside any
        * render pass.
        * @param driver Driver executing the graph.
        * @param commandBuffer Command buffer the snapshot copies are recorded into.
        * @param subGraph Subgraph that just committed.
        * @param pipelineHistoryBegin The command buffer's bound-pipeline-history size, snapshotted
        * before commit().
        */
        void recordSubGraph(VkmDriverBase* driver, VkmCommandBufferBase* commandBuffer,
                            const VkmRenderSubGraph* subGraph, size_t pipelineHistoryBegin);

        /*
        * @brief Called by the engine once the capture frame's submit has completed on the GPU.
        * @details Maps buffer readbacks into CPU memory, releases the stagings, and moves
        * Pending -> Ready.
        * @param driver Driver that executed the graph.
        */
        void finalize(VkmDriverBase* driver);

        /*
        * @brief Releases all capture-owned GPU resources through the deferred reclaimer and returns
        * to Idle. Also called implicitly by the next beginCapture().
        * @param driver Driver that owns the resources.
        */
        void releaseResources(VkmDriverBase* driver);

        static constexpr uint64_t kMaxCapturedBufferBytes = 64 * 1024;
        // Ceiling on all snapshot textures of one capture. A graph with several passes, each
        // referencing a few 4K inputs, would otherwise allocate hundreds of MB on one keypress.
        static constexpr uint64_t kMaxSnapshotBytes = 256ull * 1024 * 1024;

    private:
        /*
        * @brief Allocate a capture-owned copy of a texture's mip 0 / layer 0 and record the copy.
        * @param driver Driver that allocates the copy.
        * @param commandBuffer Command buffer the copy is recorded into.
        * @param source Texture to snapshot.
        * @param debugName Name given to the snapshot texture.
        * @return The snapshot handle, or an invalid handle when the source cannot be snapshotted,
        * in which case the caller keeps metadata only.
        */
        VkmResourceHandle takeTextureSnapshot(VkmDriverBase* driver, VkmCommandBufferBase* commandBuffer,
                                              VkmResourceHandle source, std::string debugName);

        State _state = State::Idle;
        uint32_t _capturedFrameIndex = 0;
        bool _hasContentCapture = false;
        uint64_t _snapshotBytes = 0;
        std::vector<VkmCapturedPass> _passes;
        std::vector<VkmResourceHandle> _snapshotTextures;
        // Stable storage for snapshot-texture debug names: VkmTextureInfo keeps a raw
        // const char* pointer to them (a deque never relocates elements).
        std::deque<std::string> _snapshotNames;
    };
}
