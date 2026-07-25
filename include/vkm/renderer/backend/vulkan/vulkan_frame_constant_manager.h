// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/frame_constants.h>
#include <volk.h>

#include <array>
#include <cstdint>

namespace vkm
{
    class VkmDriverVulkan;

    // Owns the engine-wide "set 1" per-frame constant set (see common/frame_constants.h): one
    // host-visible, persistently mapped uniform buffer carved into FRAME_COUNT regions, plus one
    // descriptor set per region. Every Vulkan pipeline declares this layout as set 1 alongside
    // the bindless set 0 (see VkmPipelineStateVulkan::createInner).
    //
    // Deliberately much simpler than VkmBindlessResourceManagerVulkan: the descriptors are
    // written once at initialization and never touched again -- only the buffer *bytes* change,
    // and only in a region no in-flight frame is reading -- so none of the update-after-bind
    // machinery applies. One static set per frame slot rather than a single
    // UNIFORM_BUFFER_DYNAMIC set keeps every bind site free of dynamic-offset bookkeeping.
    class VkmFrameConstantManagerVulkan : public VkmFrameConstantManagerBase
    {
    public:
        explicit VkmFrameConstantManagerVulkan(VkmDriverVulkan* driver);
        ~VkmFrameConstantManagerVulkan();

        bool initialize();
        void destroy() override final;

        void update(uint32_t frameIndex, const VkmFrameConstants& constants) override final;

        inline VkDescriptorSetLayout getSetLayout() const { return _setLayout; }

        // Descriptor set covering the region the most recent update() wrote.
        inline VkDescriptorSet getActiveDescriptorSet() const { return _descriptorSets[_activeFrameIndex]; }

    private:
        VkmDriverVulkan* _driver;

        VkDescriptorSetLayout _setLayout{VK_NULL_HANDLE};
        VkDescriptorPool _descriptorPool{VK_NULL_HANDLE};
        std::array<VkDescriptorSet, FRAME_COUNT> _descriptorSets{};

        VkBuffer _buffer{VK_NULL_HANDLE};
        void* _mappedPointer{nullptr};
        // VmaAllocation, kept opaque so this header does not pull in vk_mem_alloc.h.
        void* _allocation{nullptr};
    };
} // namespace vkm
