// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/render_pass.h>
#include <vkm/renderer/backend/common/driver_resource.h>
#include <vkm/renderer/backend/common/command_queue.h>

#include <string>

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
        * @brief In-engine-only bookkeeping name (never pushed to a native driver API, unlike
        * VkmDriverResourceBase::setDebugName()) used by VkmGpuCrashHandler to identify this
        * command buffer's submission in a crash report's breadcrumb trail. Defaults to empty;
        * VkmGpuCrashHandler synthesizes a "<queueName>#<index>" fallback name for breadcrumb
        * entries left unnamed.
        */
        inline void setDebugName(const char* name) { _debugName = name != nullptr ? name : ""; }
        inline const std::string& getDebugName() const { return _debugName; }

    protected:
        virtual void onBeginRenderPass(const VkmFrameBufferDescriptor& frameBufferDesc) = 0;
        virtual void onEndRenderPass() = 0;
        virtual void onBindPipeline(VkmPipelineStateBase* pipelineState) = 0;
        virtual void onUnbindPipeline() = 0;
        virtual void onCopyBuffer(VkmResourceHandle srcBuffer, VkmResourceHandle dstBuffer, uint64_t srcOffset, uint64_t dstOffset, uint64_t size) = 0;
        virtual void onDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) = 0;
        virtual void onSetPushConstants(const void* data, uint32_t size, uint32_t offset) = 0;

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
    };
}