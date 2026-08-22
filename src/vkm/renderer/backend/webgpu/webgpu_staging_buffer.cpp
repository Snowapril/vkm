// Copyright (c) 2026 Snowapril

#include <vkm/renderer/backend/webgpu/webgpu_staging_buffer.h>
#include <vkm/renderer/backend/webgpu/webgpu_driver.h>
#include <vkm/renderer/backend/webgpu/webgpu_util.h>

#include <emscripten/emscripten.h>

#include <algorithm>
#include <cstring>

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
        // AllowTransferDst selects the readback shape (GPU copies in, CPU maps to read). Otherwise
        // this is an upload buffer, and it deliberately carries NO map usage: it must be a
        // wgpuQueueWriteBuffer destination for writeDirect(), and WebGPU allows MapWrite only
        // alongside CopySrc, so CopyDst and MapWrite cannot coexist. See the class comment -- the
        // upload shape is served by a CPU shadow instead.
        const uint64_t usage = (info._flags & VkmResourceCreateInfo::AllowTransferDst) != 0
            ? (WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead)
            : (WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst);

        // Created UNMAPPED, unlike the Vulkan and Metal staging buffers which stay persistently
        // mapped. WebGPU forbids a mapped buffer from being used by the GPU at all: a submit that
        // references one is rejected with "used in submit while mapped" and the whole command
        // buffer is discarded, and wgpuQueueWriteBuffer rejects it too. Every use of a staging
        // buffer here starts with the GPU or the queue touching it -- a readback is copied into
        // before it is ever read, and writeDirect() is a queue write -- so mapping at creation
        // makes the first operation on a fresh buffer invalid. Callers that want the pointer ask
        // for it with map(), which is what the async round trip below is for.
        // See VkmBufferWebGPU: the caller's name reaches Dawn's validation messages.
        const std::string label = (info._debugName != nullptr) ? std::string(info._debugName)
                                                               : std::string("VkmStagingBufferWebGPU");
        // wgpuQueueWriteBuffer works in 4-byte units, so round up rather than leave a tail that
        // can never be written.
        const uint64_t alignedSize = (info._size + 3ull) & ~3ull;

        const WGPUBufferDescriptor bufferDesc{
            .label            = toWGPUStringView(label.c_str()),
            .usage            = static_cast<WGPUBufferUsage>(usage),
            .size             = alignedSize,
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
        if (!isReadback())
        {
            _shadow.assign(static_cast<size_t>(alignedSize), 0);
        }
        return true;
    }

    void* VkmStagingBufferWebGPU::map()
    {
        if (!isReadback())
        {
            // No map usage on this buffer at all; the shadow *is* the mapped range, and unmap()
            // is what pushes it. Synchronous, unlike the readback path below.
            return _shadow.data();
        }

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
        if (!isReadback())
        {
            // Everything the caller wrote through map() lives in the shadow; this is where it
            // reaches the buffer. Per spec the queue write lands before the next submit.
            flushShadow(0, _shadow.size());
            return;
        }

        wgpuBufferUnmap(_wgpuBuffer);
        _mappedPointer = nullptr; // invalidated -- callers must not cache it across unmap()
        _needsRemap = true;
    }

    void VkmStagingBufferWebGPU::flush(uint64_t offset, uint64_t size)
    {
        // A readback buffer needs nothing: writes to a mapped range become visible on
        // unmap()/submit, and WebGPU has no explicit flush. An upload buffer pushes the range,
        // so a caller that flushes without unmapping still gets its bytes across.
        if (!isReadback())
        {
            flushShadow(offset, size);
        }
    }

    void VkmStagingBufferWebGPU::flushShadow(uint64_t offset, uint64_t size)
    {
        if (_shadow.empty() || size == 0)
        {
            return;
        }
        const uint64_t alignedOffset = offset & ~3ull;
        const uint64_t end = std::min<uint64_t>((offset + size + 3ull) & ~3ull, _shadow.size());
        if (end <= alignedOffset)
        {
            return;
        }
        VkmDriverWebGPU* driverWebGPU = static_cast<VkmDriverWebGPU*>(_driver);
        wgpuQueueWriteBuffer(driverWebGPU->getQueue(), _wgpuBuffer, alignedOffset,
                             _shadow.data() + alignedOffset, static_cast<size_t>(end - alignedOffset));
    }

    void VkmStagingBufferWebGPU::writeDirect(uint64_t offset, const void* data, uint64_t size)
    {
        // Per spec the write takes effect before the next wgpuQueueSubmit(). The shadow is updated
        // too, so a later map() sees what was written here rather than stale bytes -- the two
        // routes address the same buffer and must not disagree.
        if (isReadback())
        {
            VkmDriverWebGPU* driverWebGPU = static_cast<VkmDriverWebGPU*>(_driver);
            wgpuQueueWriteBuffer(driverWebGPU->getQueue(), _wgpuBuffer, offset, data, size);
            return;
        }

        if (offset + size > _shadow.size())
        {
            VKM_DEBUG_ERROR("writeDirect: range exceeds the staging buffer");
            return;
        }
        std::memcpy(_shadow.data() + offset, data, static_cast<size_t>(size));
        flushShadow(offset, size);
    }

    void VkmStagingBufferWebGPU::setDebugName(const char* name)
    {
        if (_wgpuBuffer != nullptr)
        {
            wgpuBufferSetLabel(_wgpuBuffer, toWGPUStringView(name));
        }
    }
} // namespace vkm
