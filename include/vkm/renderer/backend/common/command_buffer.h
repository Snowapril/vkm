// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/render_pass.h>
#include <vkm/renderer/backend/common/driver_resource.h>
#include <vkm/renderer/backend/common/command_queue.h>

namespace vkm
{
    class VkmDriverBase;
    class VkmCommandQueueBase;
    class VkmCommandBufferPoolBase;
    class VkmGpuEventTimelineBase;
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
        void bindPipeline();
        void unbindPipeline();

        inline const VkmGpuEventTimelineObject& getGpuEventTimelineObject() const { return _gpuEventTimelineObject; }

    protected:
        virtual void onBeginRenderPass(const VkmFrameBufferDescriptor& frameBufferDesc) = 0;
        virtual void onEndRenderPass() = 0;

    protected:
        VkmDriverBase* _driver;
        VkmCommandQueueBase* _commandQueue;
        VkmCommandBufferPoolBase* _commandBufferPool;

        VkmGpuEventTimelineObject _gpuEventTimelineObject; // GPU event timeline object for synchronization

    protected:
        bool _isRecording; // Flag to indicate if the command buffer is currently recording commands
        bool _isInRenderPass; // Flag to indicate if the command buffer is currently in a render pass

        VkmFrameBufferDescriptor _currentFrameBufferDesc;
    };
}