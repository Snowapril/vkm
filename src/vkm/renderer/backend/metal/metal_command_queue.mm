// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_command_queue.h>
#include <vkm/renderer/backend/metal/metal_driver.h>

#include <Metal/MTLCommandQueue.h>

namespace vkm
{
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
        return _mtlCommandQueue != nil;
    }
}