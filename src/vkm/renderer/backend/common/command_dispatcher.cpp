// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/command_dispatcher.h>
#include <vkm/renderer/backend/common/command_queue.h>

namespace vkm
{
    VkmCommandDispatcher::VkmCommandDispatcher(VkmCommandQueueBase* commandQueue)
        : _commandQueue(commandQueue)
    {
    }

    VkmCommandDispatcher::~VkmCommandDispatcher()
    {
    }

    void VkmCommandDispatcher::beginCommandBuffer()
    {
        
    }

    void VkmCommandDispatcher::endCommandBuffer()
    {

    }
}