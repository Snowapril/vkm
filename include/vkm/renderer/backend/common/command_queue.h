// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/backend_util.h>

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
    class VkmCommandBufferPoolBase
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
    class VkmCommandQueueBase
    {
    public:
        VkmCommandQueueBase(VkmDriverBase* driver);
        ~VkmCommandQueueBase();

    public:
        virtual void submit(const CommandSubmitInfo& submitInfos) = 0;
        virtual void waitIdle() = 0;

    protected:
        VkmDriverBase* _driver;
    };
}