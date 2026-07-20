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
        // Texture-only fields
        VkmFormat format = VkmFormat::Undefined;
        glm::uvec3 extent{0, 0, 0};
        // Buffer-only field
        uint64_t size = 0;
    };

    // One output attachment of a captured graphics pass.
    struct VkmCapturedAttachment
    {
        VkmCapturedResourceInfo info;
        VkmLoadAction loadAction = VkmLoadAction::DontCare;
        VkmStoreAction storeAction = VkmStoreAction::DontCare;
        // True when the attachment is a swapchain back buffer (AllowPresent).
        bool isPresentTarget = false;
        // Capture-owned post-pass copy of the attachment contents. Stays invalid when
        // content capture is unsupported on this backend, or when the attachment is a
        // swapchain back buffer (AllowPresent) -- those cannot be copy sources
        // (CAMetalLayer.framebufferOnly stays YES).
        VkmResourceHandle snapshotTexture = VKM_INVALID_RESOURCE_HANDLE;
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
    *
    * Flow: arm() (hotkey/CLI) -> the engine passes this object to exactly one
    * VkmRenderGraph::execute() via VkmRenderGraphCommitOptions::capture, which calls
    * beginCapture() and then recordSubGraph() after each subgraph's commit() -- recording
    * metadata always, and (when the driver reports TextureContentCapture) post-pass
    * snapshot copies of color attachments plus staging readback copies of referenced
    * buffers into the same command buffer. After the submit completes (the engine calls
    * ensureCompleted() on the capture frame), finalize() maps the staging readbacks and
    * the capture becomes Ready for UI consumption.
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

        // Request a capture of the next executed render graph. Any previous capture's
        // data stays readable until that graph's beginCapture() releases it.
        void arm();

        State getState() const { return _state; }
        const std::vector<VkmCapturedPass>& getPasses() const { return _passes; }
        uint32_t getCapturedFrameIndex() const { return _capturedFrameIndex; }
        // True when the capturing driver reported TextureContentCapture at capture time.
        bool hasContentCapture() const { return _hasContentCapture; }
        // Capture-owned snapshot texture handles of the current capture, for GPU-usage
        // tracking while the inspector displays them (see VkmEngine::render()).
        const std::vector<VkmResourceHandle>& getSnapshotTextureHandles() const { return _snapshotTextures; }

        // Called by VkmRenderGraph::execute() when armed: releases the previous capture's
        // resources and transitions Armed -> Pending.
        void beginCapture(VkmDriverBase* driver, uint32_t frameIndex);
        // Called by VkmRenderGraph::execute() right after subGraph->commit() (outside any
        // render pass). pipelineHistoryBegin is the command buffer's bound-pipeline-history
        // size snapshotted before commit().
        void recordSubGraph(VkmDriverBase* driver, VkmCommandBufferBase* commandBuffer,
                            const VkmRenderSubGraph* subGraph, size_t pipelineHistoryBegin);
        // Called by the engine once the capture frame's submit has completed on the GPU:
        // maps buffer readbacks into CPU memory, releases stagings, Pending -> Ready.
        void finalize(VkmDriverBase* driver);
        // Releases all capture-owned GPU resources (via the deferred reclaimer) and
        // returns to Idle. Also called implicitly by the next beginCapture().
        void releaseResources(VkmDriverBase* driver);

        static constexpr uint64_t kMaxCapturedBufferBytes = 64 * 1024;

    private:
        State _state = State::Idle;
        uint32_t _capturedFrameIndex = 0;
        bool _hasContentCapture = false;
        std::vector<VkmCapturedPass> _passes;
        std::vector<VkmResourceHandle> _snapshotTextures;
        // Stable storage for snapshot-texture debug names: VkmTextureInfo keeps a raw
        // const char* pointer to them (a deque never relocates elements).
        std::deque<std::string> _snapshotNames;
    };
}
