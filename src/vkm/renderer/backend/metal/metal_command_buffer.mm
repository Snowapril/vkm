// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_command_buffer.h>

#include <Metal/MTLCommandBuffer.h>

namespace vkm
{
    VkmCommandBufferMetal::VkmCommandBufferMetal(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue, VkmCommandBufferPoolBase* commandBufferPool)
        : VkmCommandBufferBase(driver, commandQueue, commandBufferPool)
    {

    }

    VkmCommandBufferMetal::~VkmCommandBufferMetal()
    {

    }
}