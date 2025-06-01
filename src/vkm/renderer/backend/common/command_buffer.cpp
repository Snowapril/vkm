// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/command_buffer.h>

namespace vkm
{
    VkmCommandBufferBase::VkmCommandBufferBase(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue, VkmCommandBufferPoolBase* commandBufferPool)
        : _driver(driver), _commandQueue(commandQueue), _commandBufferPool(commandBufferPool)
    {
    }

    VkmCommandBufferBase::~VkmCommandBufferBase()
    {

    }

    void VkmCommandBufferBase::beginCommandBuffer()
    {
    }
    
    void VkmCommandBufferBase::endCommandBuffer()
    {
    }
    
    void VkmCommandBufferBase::beginRenderPass()
    {
    }

    void VkmCommandBufferBase::endRenderPass()
    {
    }
    
    void VkmCommandBufferBase::bindPipeline()
    {
    }

    void VkmCommandBufferBase::unbindPipeline()
    {
    }
}