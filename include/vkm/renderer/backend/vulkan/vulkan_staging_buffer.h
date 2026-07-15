// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/staging_buffer.h>
#include <volk.h>

typedef struct VmaAllocator_T* VmaAllocator;
typedef struct VmaAllocation_T* VmaAllocation;

namespace vkm
{
    class VkmStagingBufferVulkan : public VkmStagingBuffer
    {
    public:
        VkmStagingBufferVulkan(VkmDriverBase* driver);
        ~VkmStagingBufferVulkan();

        virtual bool initialize(VkmResourceHandle handle, const VkmStagingBufferInfo& info) override final;
        virtual void* map() override final;
        virtual void unmap() override final;
        virtual void flush(uint64_t offset, uint64_t size) override final;
        virtual void invalidate(uint64_t offset, uint64_t size) override final;
        virtual void writeDirect(uint64_t offset, const void* data, uint64_t size) override final;
        virtual void setDebugName(const char* name) override final;

        uint64_t getAllocatedSize() const override { return _allocatedSize; }
        uint32_t getMemoryAlignment() const override { return _alignment; }

        inline VkBuffer getBuffer() const { return _vkBuffer; }

    private:
        VkBuffer _vkBuffer{VK_NULL_HANDLE};
        VmaAllocation _vmaAllocation{nullptr};
        uint64_t _allocatedSize{0};
        uint32_t _alignment{0};
    };
} // namespace vkm
