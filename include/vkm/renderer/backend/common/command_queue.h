// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/driver_resource.h>

namespace vkm
{
    class VkmCommandBufferBase;
    class VkmCommandQueueBase;
    class VkmDriverBase;

    /*
    * @brief Command submit informations
    */
    struct CommandSubmitInfo
    {
        VkmCommandBufferBase* commandBuffers;
        uint32_t commandBufferCount;
    };

    /*
    * @brief Command buffer ppol base class
    */
    class VkmCommandBufferPoolBase : public VkmDriverResourceBase
    {
    public:
        VkmCommandBufferPoolBase(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue);
        ~VkmCommandBufferPoolBase();

    public:
        VkmCommandBufferBase* allocate();
        void release(VkmCommandBufferBase* commandBuffer);
    
    protected:
        virtual VkmCommandBufferBase* newCommandBuffer() = 0;

    protected:
        VkmDriverBase* _driver;
        VkmCommandQueueBase* _commandQueue;

        std::mutex _commandBufferMutex;
        std::vector<VkmCommandBufferBase*> _commandBuffers;
    };

    /*
    * @brief Command queue base class
    */
    class VkmCommandQueueBase : public VkmDriverResourceBase
    {
    public:
        VkmCommandQueueBase(VkmDriverBase* driver);
        ~VkmCommandQueueBase();

        bool initialize(VkmCommandQueueType queueType, uint32_t queueIndex, const char* queueName);

        inline VkmCommandQueueType getQueueType() const { return _queueType; }
        inline const char* getQueueName() const { return _queueName; }
        inline uint32_t getQueueIndex() const { return _queueIndex; }
        
    public:
        virtual void submit(const CommandSubmitInfo& submitInfos) = 0;
        virtual void waitIdle() = 0;

    protected:
        virtual bool initializeInner() = 0;

    protected:
        VkmDriverBase* _driver;
        VkmCommandQueueType _queueType;
        const char* _queueName;
        uint32_t _queueIndex;
    };
}