// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>

namespace vkm
{
    class VkmDriverResourceBase
    {
    public:
        VkmDriverResourceBase() = default;
        virtual ~VkmDriverResourceBase() = default;

        virtual void setDebugName(const char* name) = 0;

        inline VkmResourceHandle getHandle() const { return _handle; }
        
    protected:
        VkmResourceHandle _handle;
    };
}