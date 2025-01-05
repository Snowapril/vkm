// Copyright (c) 2024 Snowapril

#pragma once

#include "vkm/base/common.h"

namespace vkm
{
    class VkmDriverBase;

    class VkmRenderResource
    {
    public:
        VkmRenderResource(VkmDriverBase* driver);
        ~VkmRenderResource();

    protected:
        VkmDriverBase* _driver;
    };
} // namespace vkm