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
        * @brief Makes GPU writes visible to the CPU before reading a mapped range back, the mirror
        * of flush().
        * @details A no-op by default: Metal Shared storage and WebGPU MapRead mappings are
        * coherent. Only Vulkan overrides it, for potentially non-coherent host memory.
        * @param offset Byte offset of the range.
        * @param size Byte length of the range.
        */
        virtual void invalidate(uint64_t offset, uint64_t size) { (void)offset; (void)size; }

        /*
        * @brief Writes bytes into this buffer from the CPU without requiring it to be mapped.
        * @details Equivalent to map()+memcpy()+flush() on Vulkan and Metal, whose buffers stay
        * persistently mapped and coherent regardless of concurrent GPU access. WebGPU implements it
        * via wgpuQueueWriteBuffer(): a WebGPU buffer must be unmapped for the GPU to touch it, so a
        * buffer a command stream also writes into cannot stay persistently mapped there.
        * @param offset Byte offset to write at.
        * @param data Source bytes.
        * @param size Number of bytes to write.
        */
        virtual void writeDirect(uint64_t offset, const void* data, uint64_t size) = 0;

        /*
        * @brief This buffer's address in the GPU's address space.
        * @return The address, or 0 on backends without one: WebGPU always, and Vulkan without
        * VkmDriverCapabilityFlags::BufferDeviceAddress.
        */
        virtual uint64_t getGPUVirtualAddress() const { return 0; }

        inline const VkmStagingBufferInfo& getStagingBufferInfo() const { return _stagingBufferInfo; }
        VkmResourceType getResourceType() const override { return VkmResourceType::StagingBuffer; }

    protected:
        bool initializeStagingBufferCommon(VkmResourceHandle handle, const VkmStagingBufferInfo& info);

    protected:
        VkmStagingBufferInfo _stagingBufferInfo;
        void* _mappedPointer{nullptr};
    };
} // namespace vkm
