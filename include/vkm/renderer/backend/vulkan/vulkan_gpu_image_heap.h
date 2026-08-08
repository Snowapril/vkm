// Copyright (c) 2025 Snowapril

#pragma once

#include <volk.h>

typedef struct VmaAllocator_T* VmaAllocator;
typedef struct VmaAllocation_T* VmaAllocation;

namespace vkm
{
    class VkmDriverVulkan;

    /*
    * @brief One VkDeviceMemory block that aliasable images are bound into at chosen offsets.
    *
    * @details The counterpart to VkmGpuBufferPoolVulkan, and deliberately not the same class:
    * that pool's block is a shared *VkBuffer* carved up with VkmOffsetAllocator, whose memory
    * type was chosen for buffer usage and whose offsets are uint32_t. An image needs a raw
    * VkDeviceMemory whose memory type satisfies its own memoryTypeBits, so this asks VMA for a
    * whole allocation (vmaAllocateMemory with VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT) and
    * lets VkmAliasedMemoryHeap decide who sits where inside it.
    *
    * Which images may share a block is not this class's business -- it only owns the memory.
    * The interval arithmetic that makes sharing safe lives in VkmAliasedMemoryHeap, where it can
    * be unit-tested without a device.
    */
    class VkmGpuImageHeapVulkan
    {
    public:
        VkmGpuImageHeapVulkan(VkmDriverVulkan* driver);
        ~VkmGpuImageHeapVulkan();

        /*
        * @param memoryTypeBits The intersection of every image this block may host, as reported
        * by vkGetImageMemoryRequirements. VMA picks a concrete type inside that mask.
        */
        bool initialize(uint64_t sizeBytes, uint32_t memoryTypeBits);
        void destroy();

        // Binds an already-created VkImage into this block at `offset`. One-shot per image:
        // Vulkan allows vkBindImageMemory exactly once, which is why placements are final.
        bool bindImage(VkImage image, uint64_t offset);

        inline uint64_t getSizeBytes() const { return _sizeBytes; }

    private:
        VkmDriverVulkan* _driver;
        VmaAllocation _vmaAllocation{nullptr};
        uint64_t _sizeBytes = 0;
    };
} // namespace vkm
