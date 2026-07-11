// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/webgpu/webgpu_staging_buffer.h>
#include <vkm/renderer/backend/webgpu/webgpu_driver.h>
#include <vkm/renderer/backend/webgpu/webgpu_util.h>

#include <emscripten/emscripten.h>

namespace vkm
{
    namespace
    {
        struct MapAsyncResult
        {
            bool done = false;
            WGPUMapAsyncStatus status = WGPUMapAsyncStatus_Error;
        };

        void onBufferMapped(WGPUMapAsyncStatus status, WGPUStringView message, void* userdata1, void*)
        {
            auto* result = static_cast<MapAsyncResult*>(userdata1);
            result->status = status;
            result->done = true;
            if (status != WGPUMapAsyncStatus_Success)
            {
                VKM_DEBUG_ERROR(fmt::format("Failed to map WGPUBuffer: {}", toStdString(message)).c_str());
            }
        }
    } // namespace

    VkmStagingBufferWebGPU::VkmStagingBufferWebGPU(VkmDriverBase* driver)
        : VkmStagingBuffer(driver)
    {
    }

    VkmStagingBufferWebGPU::~VkmStagingBufferWebGPU()
    {
        if (_wgpuBuffer != nullptr)
        {
            wgpuBufferRelease(_wgpuBuffer);
        }
        _wgpuBuffer = nullptr;
    }

    bool VkmStagingBufferWebGPU::initialize(VkmResourceHandle handle, const VkmStagingBufferInfo& info)
    {
        if (!initializeStagingBufferCommon(handle, info))
        {
            return false;
        }

        VkmDriverWebGPU* driverWebGPU = static_cast<VkmDriverWebGPU*>(_driver);

        uint64_t usage = WGPUBufferUsage_CopySrc | WGPUBufferUsage_MapWrite;
        if ((info._flags & VkmResourceCreateInfo::AllowTransferDst) != 0)
        {
            usage |= WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
        }

        const WGPUBufferDescriptor bufferDesc{
            .label            = toWGPUStringView("VkmStagingBufferWebGPU"),
            .usage            = static_cast<WGPUBufferUsage>(usage),
            .size             = info._size,
            .mappedAtCreation = true,
        };
        _wgpuBuffer = wgpuDeviceCreateBuffer(driverWebGPU->getDevice(), &bufferDesc);
        if (_wgpuBuffer == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create WGPUBuffer for staging buffer");
            return false;
        }

        _mappedPointer = wgpuBufferGetMappedRange(_wgpuBuffer, 0, info._size);
        _needsRemap = false;
        return true;
    }

    void* VkmStagingBufferWebGPU::map()
    {
        if (!_needsRemap)
        {
            // Still valid from mappedAtCreation (or a previous still-open map).
            return _mappedPointer;
        }

        MapAsyncResult result;
        const WGPUBufferMapCallbackInfo callbackInfo{
            .mode      = WGPUCallbackMode_AllowSpontaneous,
            .callback  = onBufferMapped,
            .userdata1 = &result,
        };
        wgpuBufferMapAsync(_wgpuBuffer, WGPUMapMode_Write, 0, _stagingBufferInfo._size, callbackInfo);
        while (result.done == false)
        {
            emscripten_sleep(1);
        }

        if (result.status != WGPUMapAsyncStatus_Success)
        {
            return nullptr;
        }

        _mappedPointer = wgpuBufferGetMappedRange(_wgpuBuffer, 0, _stagingBufferInfo._size);
        _needsRemap = false;
        return _mappedPointer;
    }

    void VkmStagingBufferWebGPU::unmap()
    {
        wgpuBufferUnmap(_wgpuBuffer);
        _mappedPointer = nullptr; // invalidated -- callers must not cache it across unmap()
        _needsRemap = true;
    }

    void VkmStagingBufferWebGPU::flush(uint64_t, uint64_t)
    {
        // No-op: writes to a mapped range become visible on unmap()/submit, no explicit
        // flush step exists in the WebGPU API.
    }

    void VkmStagingBufferWebGPU::setDebugName(const char* name)
    {
        if (_wgpuBuffer != nullptr)
        {
            wgpuBufferSetLabel(_wgpuBuffer, toWGPUStringView(name));
        }
    }
} // namespace vkm
