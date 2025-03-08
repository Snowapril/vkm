// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/driver_resource.h>
#include <vkm/renderer/backend/common/command_stream.h>
#include <memory>
#include <string>

namespace vkm
{
    class VkmCommandQueueBase;
    /*
    * @brief Command dispatcher
    * @details Record command to command stream that will be executed by druver
    */
    class VkmCommandDispatcher
    {
    public:
        VkmCommandDispatcher(VkmCommandQueueBase* commandQueue);
        ~VkmCommandDispatcher();

        void beginCommandBuffer();
        void endCommandBuffer();

    private:
        VkmCommandQueueBase* _commandQueue;
        std::unique_ptr<VkmCommandStream> _commandStream;
    };
}