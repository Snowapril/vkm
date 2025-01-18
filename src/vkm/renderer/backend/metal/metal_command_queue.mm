// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_command_queue.h>
#include <Metal/Metal.h>

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
}