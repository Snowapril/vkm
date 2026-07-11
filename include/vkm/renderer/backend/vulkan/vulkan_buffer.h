// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/gpu_offset_allocator.h>
#include <volk.h>

typedef struct VmaAllocator_T* VmaAllocator;
typedef struct VmaAllocation_T* VmaAllocation;

namespace vkm
{
    class VkmGpuBufferPoolVulkan;

    class VkmBufferVulkan : public VkmBuffer
    {
    public:
        VkmBufferVulkan(VkmDriverBase* driver);
        ~VkmBufferVulkan();

        virtual bool initialize(VkmResourceHandle handle, const VkmBufferInfo& info) override final;
        virtual bool overrideExternalHandle(void* externalHandle) override final;
        virtual void setDebugName(const char* name) override final;

        uint64_t getAllocatedSize() const override { return _allocatedSize; }
        uint32_t getMemoryAlignment() const override { return _alignment; }

        inline VkBuffer getBuffer() const { return _vkBuffer; }
        inline uint64_t getBufferOffset() const { return _ownerPool != nullptr ? _poolAllocation._offset : 0; }

    private:
        VkBuffer _vkBuffer{VK_NULL_HANDLE};
        VmaAllocation _vmaAllocation{nullptr}; // valid only for the committed path
        uint64_t _allocatedSize{0};
        uint32_t _alignment{0};

        VkmGpuBufferPoolVulkan* _ownerPool{nullptr}; // non-owning; valid only for the pooled path
        VkmGpuMemoryAllocation _poolAllocation{};
    };
} // namespace vkm
