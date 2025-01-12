// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/backend_util.h>

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

        // TODO(snowapril) : represent that this resource is managed by vkm resource pool or by external code.
        bool _isPooledResource{true};

    };
} // namespace vkm