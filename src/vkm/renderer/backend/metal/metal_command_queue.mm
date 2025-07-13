// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_command_queue.h>
#include <vkm/renderer/backend/metal/metal_command_buffer.h>
#include <vkm/renderer/backend/metal/metal_driver.h>

#include <Metal/MTLCommandQueue.h>

namespace vkm
{
    VkmCommandBufferPoolMetal::VkmCommandBufferPoolMetal(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue)
        : VkmCommandBufferPoolBase(driver, commandQueue)
    {

    }

    VkmCommandBufferPoolMetal::~VkmCommandBufferPoolMetal()
    {

    }

    VkmCommandBufferBase* VkmCommandBufferPoolMetal::newCommandBuffer()
    {
        VkmCommandBufferMetal* commandBufferMetal = new VkmCommandBufferMetal(_driver, _commandQueue, this);
        return commandBufferMetal;
    }

    VKM_COMMAND_BUFFER_HANDLE VkmCommandBufferPoolMetal::getOrCreateRHICommandBuffer()
    {
        VkmCommandQueueMetal* commandQueueMetal = static_cast<VkmCommandQueueMetal*>(_commandQueue);
        id<MTLCommandBuffer> mtlCommandBuffer = [commandQueueMetal->getMTLCommandQueue() commandBuffer];
        return (__bridge VKM_COMMAND_BUFFER_HANDLE)mtlCommandBuffer;
    }

    VkmCommandQueueMetal::VkmCommandQueueMetal(VkmDriverBase* driver)
        : VkmCommandQueueBase(driver)
    {

    }

    VkmCommandQueueMetal::~VkmCommandQueueMetal()
    {

    }
    
    void VkmCommandQueueMetal::submit(const CommandSubmitInfo& submitInfos)
    {
        for (uint32_t i = 0; i < std::min(submitInfos.commandBufferCount, MAX_NUM_COMMAND_BUFFER_SUBMITS); ++i)
        {
            VkmCommandBufferMetal* commandBufferMetal = static_cast<VkmCommandBufferMetal*>(submitInfos.commandBuffers[i]);
            id<MTLCommandBuffer> mtlCommandBuffer = commandBufferMetal->getMTLCommandBuffer();
            [mtlCommandBuffer commit];
        }
    }

    void VkmCommandQueueMetal::waitIdle()
    {
        /*
        for (auto& commandBuffer : _commandBuffersSubmitted)
        {
            // Wait until the command buffer is completed
        }
        */
    }

    void VkmCommandQueueMetal::setDebugName(const char* name)
    {
        [_mtlCommandQueue setLabel:[NSString stringWithUTF8String:name]];
    }

    bool VkmCommandQueueMetal::initializeInner()
    {
        VkmDriverMetal* driverMetal = static_cast<VkmDriverMetal*>(_driver);
        _mtlCommandQueue = [driverMetal->getMTLDevice() newCommandQueue];

        _commandBufferPool.reset(new VkmCommandBufferPoolMetal(_driver, this));

        return _mtlCommandQueue != nil;
    }
}
