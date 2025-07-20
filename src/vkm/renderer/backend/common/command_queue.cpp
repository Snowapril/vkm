// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/command_queue.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/driver.h>

namespace vkm
{
    VkmCommandBufferPoolBase::VkmCommandBufferPoolBase(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue)
        : _driver(driver), _commandQueue(commandQueue)
    {
    }

    VkmCommandBufferPoolBase::~VkmCommandBufferPoolBase()
    {

    }
    
    VkmCommandBufferBase* VkmCommandBufferPoolBase::allocate()
    {
        // TODO(snowapril) : API command buffer handle and our command buffer instance have different life cycle.
        // Our command buffer can be reused as soon as it is submitted to the driver, but API command buffer handle should be reused after it is completed.
        
        VkmCommandBufferBase* commandBuffer = nullptr;
        std::lock_guard<std::mutex> lock(_commandBufferMutex);
        if (_commandBuffers.empty() == false)
        {
            commandBuffer = _commandBuffers.back();
            _commandBuffers.pop_back();
        }
        else
        {
            commandBuffer = newCommandBuffer();
        }

        commandBuffer->setRHICommandBuffer(getOrCreateRHICommandBuffer());
        return commandBuffer;
    }

    void VkmCommandBufferPoolBase::release(VkmCommandBufferBase* commandBuffer)
    {
        std::lock_guard<std::mutex> lock(_commandBufferMutex);
        _commandBuffers.push_back(commandBuffer);
    }

    VkmCommandQueueBase::VkmCommandQueueBase(VkmDriverBase* driver)
        : _driver(driver), _queueType(VkmCommandQueueType::Undefined), _queueName(nullptr), _queueIndex(INVALID_VALUE32)
    {

    }
    
    VkmCommandQueueBase::~VkmCommandQueueBase()
    {
        
    }

    bool VkmCommandQueueBase::initialize(VkmCommandQueueType queueType, uint32_t queueIndex, const char* queueName)
    {
        _queueType = queueType;
        _queueIndex = queueIndex;
        _queueName = queueName;

        if (initializeInner() == false)
            return false;

        setDebugName(_queueName);

        return true;
    }

    void VkmCommandQueueBase::waitIdle( const uint64_t timeoutMs )
    {
        _gpuEventTimeline->waitIdle(timeoutMs);
    }
}
