// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/command_buffer.h>

@protocol MTLCommandBuffer;

namespace vkm
{
    class VkmCommandBufferMetal : public VkmCommandBufferBase
    {
        friend class VkmCommandBufferPoolMetal;
    public:
        VkmCommandBufferMetal(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue, VkmCommandBufferPoolBase* commandBufferPool);
        ~VkmCommandBufferMetal();

        virtual void setRHICommandBuffer(VKM_COMMAND_BUFFER_HANDLE handle) override final;

    private:
        id<MTLCommandBuffer> _mtlCommandBuffer;
    };
}