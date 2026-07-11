// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_buffer_view.h>
#include <vkm/renderer/backend/metal/metal_buffer.h>
#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>

namespace vkm
{
    VkmBufferViewMetal::VkmBufferViewMetal(VkmDriverBase* driver)
        : VkmBufferView(driver)
    {
    }

    VkmBufferViewMetal::~VkmBufferViewMetal()
    {
    }

    bool VkmBufferViewMetal::initialize(VkmResourceHandle handle, const VkmBufferViewInfo& info)
    {
        if (!initializeBufferViewCommon(handle, info))
        {
            return false;
        }

        if (info._buffer.type != VkmResourceType::Buffer)
        {
            VKM_DEBUG_ERROR("VkmBufferViewInfo::_buffer must reference a Buffer handle");
            return false;
        }

        VkmBufferMetal* parentBuffer = static_cast<VkmBufferMetal*>(resolveParent());
        if (parentBuffer == nullptr)
        {
            return false;
        }

        // Unlike Vulkan, Metal buffers (committed or pooled) are always independent
        // MTLBuffer objects with no shared-pool offset to add -- info._offset is used as-is.
        return true;
    }

    void VkmBufferViewMetal::setDebugName(const char*)
    {
        // No-op: no native object to name.
    }
} // namespace vkm
