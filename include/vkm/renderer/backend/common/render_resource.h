// Copyright (c) 2025 Snowapril

#pragma once

#include "vkm/base/common.h"

namespace vkm
{
    class VkmDriverBase;

    /**
     * @brief Render resource base class
     * @details 
     */
    class VkmRenderResource
    {
    public:
        VkmRenderResource(VkmDriverBase* driver);
        ~VkmRenderResource();

    protected:
        VkmDriverBase* _driver;
    };
} // namespace vkm