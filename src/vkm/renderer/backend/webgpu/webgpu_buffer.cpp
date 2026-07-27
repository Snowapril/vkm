// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/webgpu/webgpu_buffer.h>
#include <vkm/renderer/backend/webgpu/webgpu_driver.h>
#include <vkm/renderer/backend/webgpu/webgpu_util.h>

namespace vkm
{
    VkmBufferWebGPU::VkmBufferWebGPU(VkmDriverBase* driver)
        : VkmBuffer(driver)
    {
    }

    VkmBufferWebGPU::~VkmBufferWebGPU()
    {
        if (_wgpuBuffer != nullptr && _externallyOwned == false)
        {
            wgpuBufferRelease(_wgpuBuffer);
        }
        _wgpuBuffer = nullptr;
    }

    bool VkmBufferWebGPU::initialize(VkmResourceHandle handle, const VkmBufferInfo& info)
    {
        if (!initializeBufferCommon(handle, info))
        {
            return false;
        }

        if ((info._flags & VkmResourceCreateInfo::DeferredCreation) != 0 ||
            (info._flags & VkmResourceCreateInfo::ExternalHandleOwner) != 0)
        {
            return true;
        }

        if (info._placementHint == VkmMemoryPlacementHint::ForcePooled)
        {
            // Dawn/emdawnwebgpu exposes no placement/suballocation API -- always committed.
            VKM_DEBUG_WARN("VkmMemoryPlacementHint::ForcePooled is not supported by WebGPU; buffer will be committed");
        }
        if (info._accessHint == VkmMemoryAccessHint::HostWrite)
        {
            // A WGPUBuffer's usage flags fix its map mode for life and MapWrite only combines
            // with CopySrc, so no buffer that is also sampled, drawn from or written by a shader
            // can ever be CPU-write-mapped. isHostWritable() stays false and uploads keep going
            // through the staging path.
            VKM_DEBUG_WARN("VkmMemoryAccessHint::HostWrite is not supported by WebGPU; buffer will be device-local");
        }

        VkmDriverWebGPU* driverWebGPU = static_cast<VkmDriverWebGPU*>(_driver);

        const WGPUBufferDescriptor bufferDesc{
            .label = toWGPUStringView("VkmBufferWebGPU"),
            .usage = toWGPUBufferUsage(info._flags),
            .size  = info._size,
        };
        _wgpuBuffer = wgpuDeviceCreateBuffer(driverWebGPU->getDevice(), &bufferDesc);
        if (_wgpuBuffer == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create WGPUBuffer");
            return false;
        }
        return true;
    }

    bool VkmBufferWebGPU::overrideExternalHandle(void* externalHandle)
    {
        _wgpuBuffer = static_cast<WGPUBuffer>(externalHandle);
        _externallyOwned = true;
        return true;
    }

    void VkmBufferWebGPU::setDebugName(const char* name)
    {
        if (_wgpuBuffer != nullptr)
        {
            wgpuBufferSetLabel(_wgpuBuffer, toWGPUStringView(name));
        }
    }
} // namespace vkm
