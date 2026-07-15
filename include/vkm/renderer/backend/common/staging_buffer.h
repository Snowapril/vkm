// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/render_resource.h>

namespace vkm
{
    class VkmStagingBuffer : public VkmRenderResource
    {
    public:
        VkmStagingBuffer(VkmDriverBase* driver);
        ~VkmStagingBuffer();

        virtual bool initialize(VkmResourceHandle handle, const VkmStagingBufferInfo& info) = 0;
        virtual void* map() = 0;
        virtual void unmap() = 0;
        virtual void flush(uint64_t offset, uint64_t size) = 0;

        /*
        * @brief Makes GPU writes visible to the CPU before reading a mapped range back
        * (mirror of flush()). Default no-op: Metal Shared storage and WebGPU MapRead
        * mappings are coherent; only Vulkan overrides this (vmaInvalidateAllocation) for
        * potentially non-coherent host memory.
        */
        virtual void invalidate(uint64_t offset, uint64_t size) { (void)offset; (void)size; }

        /*
        * @brief Writes `size` bytes from `data` into this buffer at `offset` from the CPU,
        * without requiring the buffer to be in a mapped state (unlike map()+memcpy()+flush()).
        * On Vulkan/Metal this is equivalent to map()+memcpy()+flush(), since those buffers stay
        * persistently mapped and coherent regardless of concurrent GPU access. On WebGPU it's
        * implemented via wgpuQueueWriteBuffer(): a WebGPU buffer must be unmapped for the GPU
        * to read/write it, so a buffer a GPU command stream also writes into (e.g.
        * VkmGpuCrashHandler's completion-marker buffer) can't stay persistently mapped there --
        * this lets the CPU still update it without an explicit map()/unmap() round trip.
        */
        virtual void writeDirect(uint64_t offset, const void* data, uint64_t size) = 0;

        inline const VkmStagingBufferInfo& getStagingBufferInfo() const { return _stagingBufferInfo; }
        VkmResourceType getResourceType() const override { return VkmResourceType::StagingBuffer; }

    protected:
        bool initializeStagingBufferCommon(VkmResourceHandle handle, const VkmStagingBufferInfo& info);

    protected:
        VkmStagingBufferInfo _stagingBufferInfo;
        void* _mappedPointer{nullptr};
    };
} // namespace vkm
