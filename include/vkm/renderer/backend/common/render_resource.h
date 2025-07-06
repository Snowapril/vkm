// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/driver_resource.h>

namespace vkm
{
    class VkmDriverBase;

    /**
     * @brief Render resource base class
     * @details 
     */
    class VkmRenderResource : public VkmDriverResourceBase
    {
    public:
        VkmRenderResource(VkmDriverBase* driver);
        virtual ~VkmRenderResource();
        
        inline VkmResourceHandle getHandle() const { return _handle; }
        virtual VkmResourceType getResourceType() const = 0;

    protected:
        bool initializeCommon(VkmResourceHandle handle)
        {
            if ( _handle.isValid() == false )
            {
                VKM_DEBUG_ERROR("Invalid resource handle");
                return false;
            }
            
            _handle = handle;
            return true;
        }
        
    protected:
        VkmDriverBase* _driver;
        VkmResourceHandle _handle;
    };
} // namespace vkm