// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/backend_util.h>

namespace vkm
{
    class IVkmDriverResource
    {
    public:
        IVkmDriverResource() = default;
        virtual ~IVkmDriverResource() = default;

        virtual void setDebugName(const char* name) = 0;
    };
}