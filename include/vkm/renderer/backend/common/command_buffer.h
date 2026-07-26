// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/render_pass.h>
#include <vkm/renderer/backend/common/driver_resource.h>
#include <vkm/renderer/backend/common/command_queue.h>

#include <string>
#include <vector>

namespace vkm
{
    class VkmDriverBase;
    class VkmCommandQueueBase;
    class VkmCommandBufferPoolBase;
    class VkmGpuEventTimelineBase;
    class VkmPipelineStateBase;
    struct VkmGpuEventTimelineObject;

    /*
    * @brief Command dispatcher
    * @details Record command to command stream that will be executed by druver
    */
    class VkmCommandBufferBase
    {
    public:
        VkmCommandBufferBase(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue, VkmCommandBufferPoolBase* commandBufferPool);
        ~VkmCommandBufferBase();

        virtual void setRHICommandBuffer(VKM_COMMAND_BUFFER_HANDLE handle) = 0;

        // Command buffer lifecycle related
        void beginCommandBuffer();
        void endCommandBuffer();

        // Render pass related
        void beginRenderPass(const VkmFrameBufferDescriptor& frameBufferDesc);
        void endRenderPass();

        // Pipeline related
        void bindPipeline(VkmPipelineStateBase* pipelineState);
        void unbindPipeline();

        // Buffer-to-buffer copy (e.g. staging -> device-local). Must be recorded while
        // recording but outside a render pass.
        void copyBuffer(VkmResourceHandle srcBuffer, VkmResourceHandle dstBuffer, uint64_t srcOffset, uint64_t dstOffset, uint64_t size);

        // Texture-to-buffer copy (e.g. render target -> readback staging). Copies mip 0 of
        // one array layer of an uncompressed color texture, tightly packed. Must be recorded
        // while recording but outside a render pass.
        void copyTextureToBuffer(VkmResourceHandle srcTexture, VkmResourceHandle dstBuffer, uint64_t dstOffset = 0,
                                 uint32_t arrayLayer = 0);

        /*
        * @brief Buffer-to-texture copy (e.g. staging -> sampled texture). Copies one
        * tightly-packed mip level of one array layer of an uncompressed color texture; a
        * cubemap face is just arrayLayer 0..5. Must be recorded while recording but outside
        * a render pass.
        * @details Leaves the destination shader-readable, so a texture is ready to sample as
        * soon as its copies have executed -- there is deliberately no separate "transition"
        * entry point, mirroring how copyTextureToBuffer manages layout on the caller's
        * behalf. Only backends reporting VkmDriverCapabilityFlags::TextureUpload implement
        * this; the others log an error and record nothing.
        */
        void copyBufferToTexture(VkmResourceHandle srcBuffer, VkmResourceHandle dstTexture,
                                 uint64_t srcOffset = 0, uint32_t mipLevel = 0, uint32_t arrayLayer = 0);

        // Full texture-to-texture copy of mip 0 / layer 0; src and dst must share format and
        // extent. Must be recorded while recording but outside a render pass. Only backends
        // reporting VkmDriverCapabilityFlags::TextureContentCapture implement this (render
        // graph capture snapshots); the others log an error and record nothing.
        void copyTexture(VkmResourceHandle srcTexture, VkmResourceHandle dstTexture);

        // Draw related -- indices, if any, are fetched manually in-shader via a bindless
        // index buffer rather than a bound VkBuffer, so there is no separate "indexed" draw.
        void draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex = 0, uint32_t firstInstance = 0);

        /*
        * @brief GPU-driven draw: `argumentBuffer` holds `maxDrawCount` consecutive records of
        * `layout` starting at `argumentOffset`, and `countBuffer`/`countOffset` name a uint32 the
        * GPU wrote with how many of them are live.
        *
        * `layout` is what the record stride comes from (vkmGetIndirectArgumentStride), so a caller
        * declares the buffer's structure instead of every backend assuming it.
        *
        * Only Vulkan consumes the count buffer (vkCmdDrawIndirectCount). Metal 4 and WebGPU core
        * have neither a GPU-side draw count nor multi-draw, so they encode exactly `maxDrawCount`
        * indirect draws and rely on all-zero records being no-ops. The producing pass must
        * therefore ALWAYS (a) compact surviving draws to the front of the range and (b) leave the
        * rest zeroed -- otherwise the backends disagree about what gets drawn.
        *
        * Only VkmIndirectArgumentLayout::NonIndexed is accepted today: an indexed draw needs a
        * bound index buffer, and this engine deliberately has none (see draw() above). Indexed is
        * rejected here rather than in each backend so none of them carries a dead branch.
        *
        * Callers must have recorded barrierIndirectArgumentBuffer() for `argumentBuffer` after the
        * writing dispatch and before beginRenderPass().
        */
        void drawIndirectCount(VkmIndirectArgumentLayout layout,
                               VkmResourceHandle argumentBuffer, uint64_t argumentOffset,
                               VkmResourceHandle countBuffer, uint64_t countOffset,
                               uint32_t maxDrawCount);

        /*
        * @brief Compute dispatch of `groupCount*` threadgroups. Must be recorded outside a render
        * pass with a compute pipeline bound; the backing compute pass is opened by that bindPipeline()
        * and closed by unbindPipeline(). The threadgroup width is the engine-wide
        * kVkmComputeThreadGroupSizeX, which Metal needs here and cannot query from its pipeline.
        */
        void dispatch(uint32_t groupCountX, uint32_t groupCountY = 1, uint32_t groupCountZ = 1);

        /*
        * @brief Orders GPU-driven bookkeeping buffers: transfer and compute writes to `buffer`
        * recorded before this call become visible to compute reads and indirect-argument fetches
        * recorded after it. Must be recorded outside a render pass.
        */
        void barrierIndirectArgumentBuffer(VkmResourceHandle buffer);

        void setPushConstants(const void* data, uint32_t size, uint32_t offset = 0);

        // GPU frame-time profiling hooks for the engine debug overlay. Only Vulkan overrides
        // these with real behavior; Metal/WebGPU keep the empty default (reports 0).
        virtual void writeGpuTimestampBegin() {}
        virtual void writeGpuTimestampEnd() {}

        inline const VkmGpuEventTimelineObject& getGpuEventTimelineObject() const { return _gpuEventTimelineObject; }

#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
        /*
        * @brief Records the last command of a subgraph: copies 4 bytes from a small constant-
        * `1` buffer (oneBuffer) into markerBuffer at offset (subgraphIndex * 4), so
        * VkmGpuCrashHandler::reportCrash() can later read that offset and know this subgraph's
        * GPU commands definitely executed by the time of a crash. Also remembers subGraphId
        * (see getRecordedSubGraphIds()) so the crash report can label which subgraph each
        * offset corresponds to. Callers (VkmRenderGraph::execute()) are expected to only call
        * this when VkmDriverBase::isGpuCrashDumpEnabled() is true.
        */
        void writeCompletionMarker(VkmResourceHandle markerBuffer, VkmResourceHandle oneBuffer, uint32_t subGraphId, uint32_t offset);

        /*
        * @brief Subgraph IDs (see VkmRenderSubGraph::getSubGraphId()) written into this command
        * buffer via writeCompletionMarker() since the last beginCommandBuffer(). Read by
        * VkmGpuCrashHandler::recordSubmission() to attach per-subgraph completion tracking to
        * this command buffer's breadcrumb entry.
        */
        inline const std::vector<uint32_t>& getRecordedSubGraphIds() const { return _recordedSubgraphIds; }
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS

        /*
        * @brief In-engine bookkeeping name (see getDebugName()), used by VkmGpuCrashHandler to
        * identify this command buffer's submission in a crash report's breadcrumb trail.
        * Additionally pushes a native label via onSetDebugName() when
        * VkmDriverBase::isDebugNamingEnabled() is true, mirroring how resource/queue debug
        * names work (see common/AGENTS.md "Debug Naming").
        */
        void setDebugName(const char* name);
        inline const std::string& getDebugName() const { return _debugName; }

        /*
        * @brief Open/close a named GPU debug group around the commands recorded until the
        * matching popDebugGroup(). Used to bracket each render subgraph so a GPU capture shows
        * named, collapsible scopes (e.g. "TrianglePass", "EngineImGuiOverlay"). Both are no-ops
        * unless VkmDriverBase::getLaunchOptions().enableGpuCapture is set -- gating both on the
        * same flag keeps push/pop balanced. The native call is delegated to onPushDebugGroup()/
        * onPopDebugGroup() in each backend.
        */
        void pushDebugGroup(const char* name);
        void popDebugGroup();

        /*
        * @brief Every pipeline bound via bindPipeline() since the last beginCommandBuffer(),
        * in bind order. Read by VkmRenderGraphCapture to attribute pipelines to subgraphs
        * (it snapshots the size before a subgraph's commit() and takes the delta after).
        * Pointers are only valid while the frame that recorded them is in flight.
        */
        inline const std::vector<VkmPipelineStateBase*>& getBoundPipelineHistory() const { return _boundPipelineHistory; }

    protected:
        virtual void onBeginRenderPass(const VkmFrameBufferDescriptor& frameBufferDesc) = 0;
        virtual void onEndRenderPass() = 0;
        virtual void onBindPipeline(VkmPipelineStateBase* pipelineState) = 0;
        virtual void onUnbindPipeline() = 0;
        virtual void onCopyBuffer(VkmResourceHandle srcBuffer, VkmResourceHandle dstBuffer, uint64_t srcOffset, uint64_t dstOffset, uint64_t size) = 0;
        virtual void onCopyTextureToBuffer(VkmResourceHandle srcTexture, VkmResourceHandle dstBuffer, uint64_t dstOffset, uint32_t arrayLayer) = 0;
        virtual void onCopyBufferToTexture(VkmResourceHandle srcBuffer, VkmResourceHandle dstTexture, uint64_t srcOffset, uint32_t mipLevel, uint32_t arrayLayer) = 0;
        virtual void onCopyTexture(VkmResourceHandle srcTexture, VkmResourceHandle dstTexture) = 0;
        virtual void onDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) = 0;
        // `layout` is guaranteed to be NonIndexed today (drawIndirectCount rejects the rest), but
        // the record stride must still be taken from it via vkmGetIndirectArgumentStride rather
        // than assumed.
        virtual void onDrawIndirectCount(VkmIndirectArgumentLayout layout,
                                         VkmResourceHandle argumentBuffer, uint64_t argumentOffset,
                                         VkmResourceHandle countBuffer, uint64_t countOffset,
                                         uint32_t maxDrawCount) = 0;
        virtual void onDispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) = 0;
        virtual void onBarrierIndirectArgumentBuffer(VkmResourceHandle buffer) = 0;
        virtual void onSetPushConstants(const void* data, uint32_t size, uint32_t offset) = 0;
        virtual void onSetDebugName(const char* name) = 0;
        virtual void onPushDebugGroup(const char* name) = 0;
        virtual void onPopDebugGroup() = 0;

#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
        virtual void onWriteCompletionMarker(VkmResourceHandle markerBuffer, VkmResourceHandle oneBuffer, uint32_t offset) = 0;

        /*
        * @brief Called by endCommandBuffer() before it flips _isRecording off. No-op on
        * Vulkan/WebGPU (writeCompletionMarker() there records its copy immediately). Metal
        * uses this to flush every writeCompletionMarker() call made during this command
        * buffer's recording as a single batched compute pass instead of one short-lived
        * compute encoder per call -- opening/closing a compute encoder per subgraph was
        * observed to cause progressively worsening MTL4CommandQueueErrorTimeout and eventual
        * command-queue stalls under real interactive use.
        */
        virtual void onEndCommandBuffer() = 0;
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS

    protected:
        VkmDriverBase* _driver;
        VkmCommandQueueBase* _commandQueue;
        VkmCommandBufferPoolBase* _commandBufferPool;

        VkmGpuEventTimelineObject _gpuEventTimelineObject; // GPU event timeline object for synchronization

    protected:
        bool _isRecording; // Flag to indicate if the command buffer is currently recording commands
        bool _isInRenderPass; // Flag to indicate if the command buffer is currently in a render pass

        VkmFrameBufferDescriptor _currentFrameBufferDesc;

    private:
        VkmPipelineStateBase* _boundPipelineState = nullptr;
        std::vector<VkmPipelineStateBase*> _boundPipelineHistory;
        std::string _debugName;
#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
        std::vector<uint32_t> _recordedSubgraphIds;
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS
    };
}