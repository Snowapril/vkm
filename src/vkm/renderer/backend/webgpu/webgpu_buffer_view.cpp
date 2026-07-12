// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/webgpu/webgpu_buffer_view.h>
#include <vkm/renderer/backend/webgpu/webgpu_buffer.h>
#include <vkm/renderer/backend/webgpu/webgpu_driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>

namespace vkm
{
    VkmBufferViewWebGPU::VkmBufferViewWebGPU(VkmDriverBase* driver)
        : VkmBufferView(driver)
    {
    }

    VkmBufferViewWebGPU::~VkmBufferViewWebGPU()
    {
    }

    bool VkmBufferViewWebGPU::initialize(VkmResourceHandle handle, const VkmBufferViewInfo& info)
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

        VkmBufferWebGPU* parentBuffer = static_cast<VkmBufferWebGPU*>(resolveParent());
        if (parentBuffer == nullptr)
        {
            return false;
        }

        // WebGPU buffers are always committed, independent WGPUBuffer objects -- info._offset
        // is used as-is, no pool offset to add.
        return true;
    }

    void VkmBufferViewWebGPU::setDebugName(const char*)
    {
        // No-op: no native object to name.
    }
} // namespace vkm
