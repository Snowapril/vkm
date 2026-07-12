// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/renderer_common.h>
#include <volk.h>

#include <vector>
#include <cstdint>

namespace vkm
{
    class VkmDriverVulkan;

    // Which bindless array a registered buffer belongs to. Each maps to a fixed binding
    // within the engine-global bindless descriptor set (see VkmBindlessResourceManagerVulkan).
    enum class VkmBindlessArrayType : uint8_t
    {
        Texture     = 0, // set 0, binding 0 (sampled image array) -- reserved, unused so far
        Buffer      = 1, // set 0, binding 1 (storage buffer array) -- vertex-pulling data
        IndexBuffer = 2, // set 0, binding 2 (storage buffer array) -- index-pulling data
    };

    // Owns the engine-wide "set 0" bindless descriptor set: a fixed-capacity, update-after-bind
    // set of sampled-image and storage-buffer arrays that every Vulkan pipeline shares (see
    // VkmPipelineStateVulkan::createInner). Deliberately Vulkan-only and separate from
    // VkmRenderResourcePool, which has no descriptor-set concept -- this class only tracks
    // *slot* allocation (a plain free-list, not VkmOffsetAllocator's byte-range coalescing,
    // which is the wrong shape for uniform-size array-element allocation) and the descriptor
    // writes that publish a resource at a given slot.
    class VkmBindlessResourceManagerVulkan
    {
    public:
        static constexpr uint32_t TEXTURE_CAPACITY      = 4096;
        static constexpr uint32_t BUFFER_CAPACITY        = 4096;
        static constexpr uint32_t INDEX_BUFFER_CAPACITY   = 4096;

        explicit VkmBindlessResourceManagerVulkan(VkmDriverVulkan* driver);
        ~VkmBindlessResourceManagerVulkan();

        bool initialize();
        void destroy();

        // Registers an existing storage buffer resource into the given bindless array and
        // writes an update-after-bind descriptor at the returned slot. Returns UINT32_MAX if
        // the array is exhausted.
        uint32_t registerBuffer(VkmResourceHandle bufferHandle, VkmBindlessArrayType arrayType);
        void unregisterBuffer(uint32_t slot, VkmBindlessArrayType arrayType);

        inline VkDescriptorSetLayout getSetLayout() const { return _setLayout; }
        inline VkDescriptorSet getDescriptorSet() const { return _descriptorSet; }

    private:
        // Pops a free slot for `arrayType` if one was previously released, otherwise bumps
        // the monotonic high-water mark. Returns UINT32_MAX if capacity is exhausted.
        uint32_t allocateSlot(VkmBindlessArrayType arrayType);

        VkmDriverVulkan* _driver;

        VkDescriptorSetLayout _setLayout{VK_NULL_HANDLE};
        VkDescriptorPool _descriptorPool{VK_NULL_HANDLE};
        VkDescriptorSet _descriptorSet{VK_NULL_HANDLE};

        std::vector<uint32_t> _freeTextureSlots;
        uint32_t _nextTextureSlot = 0;
        std::vector<uint32_t> _freeBufferSlots;
        uint32_t _nextBufferSlot = 0;
        std::vector<uint32_t> _freeIndexBufferSlots;
        uint32_t _nextIndexBufferSlot = 0;
    };
} // namespace vkm
