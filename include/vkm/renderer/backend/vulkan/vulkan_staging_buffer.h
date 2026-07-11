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
        virtual void setDebugName(const char* name) override final;

        inline VkBuffer getBuffer() const { return _vkBuffer; }

    private:
        VkBuffer _vkBuffer{VK_NULL_HANDLE};
        VmaAllocation _vmaAllocation{nullptr};
    };
} // namespace vkm
