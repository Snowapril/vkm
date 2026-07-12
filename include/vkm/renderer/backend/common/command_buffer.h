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

        // Draw related -- indices, if any, are fetched manually in-shader via a bindless
        // index buffer rather than a bound VkBuffer, so there is no separate "indexed" draw.
        void draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex = 0, uint32_t firstInstance = 0);
        void setPushConstants(const void* data, uint32_t size, uint32_t offset = 0);

        // GPU frame-time profiling hooks for the engine debug overlay. Only Vulkan overrides
        // these with real behavior; Metal/WebGPU keep the empty default (reports 0).
        virtual void writeGpuTimestampBegin() {}
        virtual void writeGpuTimestampEnd() {}

        inline const VkmGpuEventTimelineObject& getGpuEventTimelineObject() const { return _gpuEventTimelineObject; }

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

        /*
        * @brief In-engine bookkeeping name (see getDebugName()), used by VkmGpuCrashHandler to
        * identify this command buffer's submission in a crash report's breadcrumb trail.
        * Additionally pushes a native label via onSetDebugName() when
        * VkmDriverBase::isDebugNamingEnabled() is true, mirroring how resource/queue debug
        * names work (see common/AGENTS.md "Debug Naming").
        */
        void setDebugName(const char* name);
        inline const std::string& getDebugName() const { return _debugName; }

    protected:
        virtual void onBeginRenderPass(const VkmFrameBufferDescriptor& frameBufferDesc) = 0;
        virtual void onEndRenderPass() = 0;
        virtual void onBindPipeline(VkmPipelineStateBase* pipelineState) = 0;
        virtual void onUnbindPipeline() = 0;
        virtual void onCopyBuffer(VkmResourceHandle srcBuffer, VkmResourceHandle dstBuffer, uint64_t srcOffset, uint64_t dstOffset, uint64_t size) = 0;
        virtual void onDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) = 0;
        virtual void onSetPushConstants(const void* data, uint32_t size, uint32_t offset) = 0;
        virtual void onWriteCompletionMarker(VkmResourceHandle markerBuffer, VkmResourceHandle oneBuffer, uint32_t offset) = 0;
        virtual void onSetDebugName(const char* name) = 0;

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
        std::string _debugName;
        std::vector<uint32_t> _recordedSubgraphIds;
    };
}