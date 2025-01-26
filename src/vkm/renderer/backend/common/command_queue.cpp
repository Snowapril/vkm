// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/command_queue.h>

namespace vkm
{
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