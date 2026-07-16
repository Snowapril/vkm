// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/bindless_resource_manager.h>
#include <volk.h>

#include <cstdint>

namespace vkm
{
    class VkmDriverVulkan;

    // Owns the engine-wide "set 0" bindless descriptor set: a fixed-capacity, update-after-bind
    // set of sampled-image and storage-buffer arrays that every Vulkan pipeline shares (see
    // VkmPipelineStateVulkan::createInner). Implements the engine-global bindless convention
    // declared in common/bindless_resource_manager.h; separate from VkmRenderResourcePool,
    // which has no descriptor-set concept -- this class only tracks *slot* allocation
    // (VkmBindlessSlotAllocator) and the descriptor writes that publish a resource at a slot.
    class VkmBindlessResourceManagerVulkan : public VkmBindlessResourceManagerBase
    {
    public:
        static constexpr uint32_t TEXTURE_CAPACITY      = kVkmBindlessTextureCapacity;
        static constexpr uint32_t BUFFER_CAPACITY        = kVkmBindlessBufferCapacity;
        static constexpr uint32_t INDEX_BUFFER_CAPACITY   = kVkmBindlessIndexBufferCapacity;

        explicit VkmBindlessResourceManagerVulkan(VkmDriverVulkan* driver);
        ~VkmBindlessResourceManagerVulkan();

        bool initialize();
        void destroy() override final;

        // Registers an existing storage buffer resource into the given bindless array and
        // writes an update-after-bind descriptor at the returned slot. Returns UINT32_MAX if
        // the array is exhausted.
        uint32_t registerBuffer(VkmResourceHandle bufferHandle, VkmBindlessArrayType arrayType) override final;
        void unregisterBuffer(uint32_t slot, VkmBindlessArrayType arrayType) override final;

        inline VkDescriptorSetLayout getSetLayout() const { return _setLayout; }
        inline VkDescriptorSet getDescriptorSet() const { return _descriptorSet; }

    private:
        // Picks the slot allocator matching `arrayType` and allocates from it. Returns
        // UINT32_MAX if capacity is exhausted.
        uint32_t allocateSlot(VkmBindlessArrayType arrayType);

        VkmDriverVulkan* _driver;

        VkDescriptorSetLayout _setLayout{VK_NULL_HANDLE};
        VkDescriptorPool _descriptorPool{VK_NULL_HANDLE};
        VkDescriptorSet _descriptorSet{VK_NULL_HANDLE};

        VkmBindlessSlotAllocator _textureSlots{TEXTURE_CAPACITY};
        VkmBindlessSlotAllocator _bufferSlots{BUFFER_CAPACITY};
        VkmBindlessSlotAllocator _indexBufferSlots{INDEX_BUFFER_CAPACITY};
    };
} // namespace vkm
