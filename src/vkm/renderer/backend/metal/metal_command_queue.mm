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
        VkmCommandQueueMetal* commandQueueMetal = static_cast<VkmCommandQueueMetal*>(_commandQueue);
        commandBufferMetal->_mtlCommandBuffer = [commandQueueMetal->getMTLCommandQueue() commandBuffer];
        return commandBufferMetal;
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
        /* 
        for (uint32_t i = 0; i < submitInfos.commandBufferCount; ++i)
        {
            VkmCommandBufferBase* commandBuffer = submitInfos.commandBuffers[i];
            // submit

            _commandBuffersSubmitted.push_back(commandBuffer);
        }
        */
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