// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>

namespace vkm
{
    class VkmCommandBufferBase;
    class VkmDriverBase;

    class VkmDispatcher
    {
    public:
        using ExecuteCommandType = void(*)(VkmDriverBase& driver, VkmCommandBufferBase* command, intptr_t* next);


    };
}