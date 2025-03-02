// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/command_queue.h>

namespace vkm
{
    VkmCommandBufferPoolBase::VkmCommandBufferPoolBase(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue)
        : _driver(driver), _commandQueue(commandQueue)
    {
        _doesCommandBufferReusable = (_driver->getCapabilityFlags() & VkmDriverCapabilityFlags::CommandBufferReusable) != 0;
    }

    VkmCommandBufferPoolBase::~VkmCommandBufferPoolBase()
    {

    }
    
    VkmCommandBufferBase* VkmCommandBufferPoolBase::allocate()
    {
        VkmCommandBufferBase* commandBuffer = nullptr;
        if ( _doesCommandBufferReusable )
        {
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
        }
        else
        {
            commandBuffer = newCommandBuffer();
        }
        return commandBuffer;
    }

    void VkmCommandBufferPoolBase::release(VkmCommandBufferBase* commandBuffer)
    {
        // TODO(snowapril) : reuse VkmCommandBufferBase instance and discard it's rhi resource (MTLCommandBuffer or VkCommandBuffer)
        if ( _doesCommandBufferReusable )
        {
            std::lock_guard<std::mutex> lock(_commandBufferMutex);
            _commandBuffers.push_back(commandBuffer);
        }
        else
        {
            delete commandBuffer;
        }
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
}