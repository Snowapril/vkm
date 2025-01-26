// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/backend_util.h>
#include <vkm/renderer/backend/common/driver_resource.h>

namespace vkm
{
    class VkmDriverBase;

    /**
     * @brief Render resource base class
     * @details 
     */
    class VkmRenderResource : public IVkmDriverResource
    {
    public:
        VkmRenderResource(VkmDriverBase* driver);
        virtual ~VkmRenderResource();

    protected:
        VkmDriverBase* _driver;

        // TODO(snowapril) : represent that this resource is managed by vkm resource pool or by external code.
        bool _isPooledResource{true};

    };
} // namespace vkm