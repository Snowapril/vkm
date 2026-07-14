// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/staging_buffer.h>
#include <webgpu/webgpu.h>

namespace vkm
{
    /*
    * @brief `mappedAtCreation` gives a mapped pointer valid only until the first
    * wgpuBufferUnmap(). Re-mapping after that needs the async wgpuBufferMapAsync, bridged
    * with the same Asyncify emscripten_sleep-spin pattern VkmDriverWebGPU::initializeInner
    * uses for adapter/device requests. unmap() invalidates the previously-returned pointer --
    * callers must not cache it across an unmap() call.
    */
    class VkmStagingBufferWebGPU final : public VkmStagingBuffer
    {
    public:
        VkmStagingBufferWebGPU(VkmDriverBase* driver);
        ~VkmStagingBufferWebGPU();

        virtual bool initialize(VkmResourceHandle handle, const VkmStagingBufferInfo& info) override final;
        virtual void* map() override final;
        virtual void unmap() override final;
        virtual void flush(uint64_t offset, uint64_t size) override final;
        virtual void writeDirect(uint64_t offset, const void* data, uint64_t size) override final;
        virtual void setDebugName(const char* name) override final;

        uint64_t getAllocatedSize() const override { return _stagingBufferInfo._size; }
        uint32_t getMemoryAlignment() const override { return 256; }

        inline WGPUBuffer getBuffer() const { return _wgpuBuffer; }

    private:
        WGPUBuffer _wgpuBuffer{nullptr};
        bool _needsRemap{false}; // true once unmap() has been called at least once
    };
} // namespace vkm
