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

        // WebGPU buffer usage is map-mode-exclusive: MapRead may only combine with CopyDst,
        // MapWrite may only combine with CopySrc -- a buffer can never be both CPU-write- and
        // CPU-read-mappable. AllowTransferDst therefore selects a read-back buffer (GPU/CPU
        // writes it via copy/wgpuQueueWriteBuffer, CPU reads it back); otherwise this is the
        // default upload buffer (CPU writes it via map, GPU reads it).
        const uint64_t usage = (info._flags & VkmResourceCreateInfo::AllowTransferDst) != 0
            ? (WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead)
            : (WGPUBufferUsage_CopySrc | WGPUBufferUsage_MapWrite);

        // Created UNMAPPED, unlike the Vulkan and Metal staging buffers which stay persistently
        // mapped. WebGPU forbids a mapped buffer from being used by the GPU at all: a submit that
        // references one is rejected with "used in submit while mapped" and the whole command
        // buffer is discarded, and wgpuQueueWriteBuffer rejects it too. Every use of a staging
        // buffer here starts with the GPU or the queue touching it -- a readback is copied into
        // before it is ever read, and writeDirect() is a queue write -- so mapping at creation
        // makes the first operation on a fresh buffer invalid. Callers that want the pointer ask
        // for it with map(), which is what the async round trip below is for.
        const WGPUBufferDescriptor bufferDesc{
            .label            = toWGPUStringView("VkmStagingBufferWebGPU"),
            .usage            = static_cast<WGPUBufferUsage>(usage),
            .size             = info._size,
            .mappedAtCreation = false,
        };
        _wgpuBuffer = wgpuDeviceCreateBuffer(driverWebGPU->getDevice(), &bufferDesc);
        if (_wgpuBuffer == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create WGPUBuffer for staging buffer");
            return false;
        }

        _mappedPointer = nullptr;
        _needsRemap = true;
        return true;
    }

    void* VkmStagingBufferWebGPU::map()
    {
        if (!_needsRemap)
        {
            // Still valid from mappedAtCreation (or a previous still-open map).
            return _mappedPointer;
        }

        // The map mode must match whichever single mode this buffer's usage was created with
        // (see initialize()) -- WebGPU buffers are map-mode-exclusive, there is no "either" mode.
        const WGPUMapMode mapMode = (_stagingBufferInfo._flags & VkmResourceCreateInfo::AllowTransferDst) != 0
            ? WGPUMapMode_Read : WGPUMapMode_Write;

        MapAsyncResult result;
        const WGPUBufferMapCallbackInfo callbackInfo{
            .mode      = WGPUCallbackMode_AllowSpontaneous,
            .callback  = onBufferMapped,
            .userdata1 = &result,
        };
        wgpuBufferMapAsync(_wgpuBuffer, mapMode, 0, _stagingBufferInfo._size, callbackInfo);
        while (result.done == false)
        {
            emscripten_sleep(1);
        }

        if (result.status != WGPUMapAsyncStatus_Success)
        {
            return nullptr;
        }

        // A read-only mapping must be fetched through the const entry point; wgpuBufferGetMappedRange
        // rejects it outright. The const_cast keeps one signature for every backend -- callers of a
        // readback buffer only ever read through it, which is exactly what the mode says.
        _mappedPointer = mapMode == WGPUMapMode_Read
            ? const_cast<void*>(wgpuBufferGetConstMappedRange(_wgpuBuffer, 0, _stagingBufferInfo._size))
            : wgpuBufferGetMappedRange(_wgpuBuffer, 0, _stagingBufferInfo._size);
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

    void VkmStagingBufferWebGPU::writeDirect(uint64_t offset, const void* data, uint64_t size)
    {
        // Unlike map()+memcpy(), this works regardless of the buffer's current map state --
        // required for a buffer a GPU command stream also writes into (map()/unmap() would
        // otherwise be needed around every write, each a real async round trip on this
        // backend). Per spec, the write takes effect before the next wgpuQueueSubmit().
        VkmDriverWebGPU* driverWebGPU = static_cast<VkmDriverWebGPU*>(_driver);
        wgpuQueueWriteBuffer(driverWebGPU->getQueue(), _wgpuBuffer, offset, data, size);
    }

    void VkmStagingBufferWebGPU::setDebugName(const char* name)
    {
        if (_wgpuBuffer != nullptr)
        {
            wgpuBufferSetLabel(_wgpuBuffer, toWGPUStringView(name));
        }
    }
} // namespace vkm
