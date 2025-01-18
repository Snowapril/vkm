// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/command_queue.h>
#include <vector>

class MTLCommandQueue;

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
        MTLCommandQueue* _mtlCommandQueue;
        std::vector<VkmCommandBufferBase*> _commandBuffersSubmitted;
    };
}