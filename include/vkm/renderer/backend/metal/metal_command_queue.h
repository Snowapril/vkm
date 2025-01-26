// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/command_queue.h>
#include <vector>

@protocol MTLCommandQueue;

namespace vkm
{
    /*
    * @brief Command queue 
    */
    class VkmCommandQueueMetal : public VkmCommandQueueBase
    {
    public:
        VkmCommandQueueMetal(VkmDriverBase* driver);
        ~VkmCommandQueueMetal();
    
    public:
        virtual void submit(const CommandSubmitInfo& submitInfos) override final;
        virtual void waitIdle() override final;

    protected:
        virtual bool initializeInner() override final;

    protected:
        id<MTLCommandQueue> _mtlCommandQueue;
        std::vector<VkmCommandBufferBase*> _commandBuffersSubmitted;
    };
}