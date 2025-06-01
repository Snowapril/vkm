// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/driver_resource.h>

namespace vkm
{
    class VkmDriverBase;
    class VkmCommandQueueBase;
    class VkmCommandBufferPoolBase;
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
        void beginRenderPass();
        void endRenderPass();

        // Pipeline related
        void bindPipeline();
        void unbindPipeline();

    private:
        VkmDriverBase* _driver;
        VkmCommandQueueBase* _commandQueue;
        VkmCommandBufferPoolBase* _commandBufferPool;
    };
}