// Copyright (c) 2026 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_frame_constant_manager.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_util.h>

#include <vk_mem_alloc.h>

#include <array>
#include <cstring>

namespace vkm
{
    VkmFrameConstantManagerVulkan::VkmFrameConstantManagerVulkan(VkmDriverVulkan* driver)
        : _driver(driver)
    {
    }

    VkmFrameConstantManagerVulkan::~VkmFrameConstantManagerVulkan()
    {
    }

    bool VkmFrameConstantManagerVulkan::initialize()
    {
        VkDevice device = _driver->getDevice();

        VkPhysicalDeviceProperties deviceProperties{};
        vkGetPhysicalDeviceProperties(_driver->getPhysicalDevice(), &deviceProperties);
        const VkDeviceSize minOffsetAlignment = deviceProperties.limits.minUniformBufferOffsetAlignment;
        if (minOffsetAlignment > kVkmFrameConstantAlignment)
        {
            // kVkmFrameConstantAlignment is a shared constant, not a per-device query, so a
            // device demanding more would silently point every descriptor at a misaligned
            // region. No known device does; fail loudly rather than render garbage.
            VKM_DEBUG_ERROR("minUniformBufferOffsetAlignment exceeds kVkmFrameConstantAlignment");
            return false;
        }

        const VkBufferCreateInfo bufferCreateInfo{
            .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size        = kVkmFrameConstantBufferSize,
            .usage       = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };

        // Host-visible and persistently mapped, the same allocation shape staging buffers use:
        // the CPU writes one region per frame and there is no GPU copy to schedule. Sequential
        // write is the right access pattern -- nothing ever reads this back.
        VmaAllocationCreateInfo allocCreateInfo{};
        allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST;
        allocCreateInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;

        VmaAllocation allocation = VK_NULL_HANDLE;
        VmaAllocationInfo vmaAllocationInfo{};
        VkResult vkResult = vmaCreateBuffer(_driver->getVmaAllocator(), &bufferCreateInfo,
                                            &allocCreateInfo, &_buffer, &allocation, &vmaAllocationInfo);
        if (!VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to create frame-constant buffer via VMA"))
        {
            return false;
        }
        _allocation = allocation;
        _mappedPointer = vmaAllocationInfo.pMappedData;

        // Identity rather than zero, so a shader that reads set 1 before the first update()
        // still produces something defined (see VkmFrameConstants' default initializers).
        const VkmFrameConstants identity{};
        for (uint32_t frameIndex = 0; frameIndex < FRAME_COUNT; ++frameIndex)
        {
            std::memcpy(static_cast<uint8_t*>(_mappedPointer) + frameIndex * kVkmFrameConstantStride,
                        &identity, sizeof(VkmFrameConstants));
        }

        const VkDescriptorSetLayoutBinding binding{
            .binding         = kVkmFrameConstantBinding,
            .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            .descriptorCount = 1,
            .stageFlags      = VK_SHADER_STAGE_ALL,
        };
        const VkDescriptorSetLayoutCreateInfo layoutCreateInfo{
            .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1,
            .pBindings    = &binding,
        };
        vkResult = vkCreateDescriptorSetLayout(device, &layoutCreateInfo, nullptr, &_setLayout);
        if (!VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to create frame-constant descriptor set layout"))
        {
            destroy();
            return false;
        }

        const VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, FRAME_COUNT};
        const VkDescriptorPoolCreateInfo poolCreateInfo{
            .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets       = FRAME_COUNT,
            .poolSizeCount = 1,
            .pPoolSizes    = &poolSize,
        };
        vkResult = vkCreateDescriptorPool(device, &poolCreateInfo, nullptr, &_descriptorPool);
        if (!VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to create frame-constant descriptor pool"))
        {
            destroy();
            return false;
        }

        const std::array<VkDescriptorSetLayout, FRAME_COUNT> setLayouts{_setLayout, _setLayout, _setLayout};
        static_assert(FRAME_COUNT == 3, "setLayouts above must have one entry per frame slot");
        const VkDescriptorSetAllocateInfo allocateInfo{
            .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool     = _descriptorPool,
            .descriptorSetCount = FRAME_COUNT,
            .pSetLayouts        = setLayouts.data(),
        };
        vkResult = vkAllocateDescriptorSets(device, &allocateInfo, _descriptorSets.data());
        if (!VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to allocate frame-constant descriptor sets"))
        {
            destroy();
            return false;
        }

        // Each set covers its own region, written once here and never rewritten.
        std::array<VkDescriptorBufferInfo, FRAME_COUNT> bufferInfos{};
        std::array<VkWriteDescriptorSet, FRAME_COUNT> writes{};
        for (uint32_t frameIndex = 0; frameIndex < FRAME_COUNT; ++frameIndex)
        {
            bufferInfos[frameIndex] = VkDescriptorBufferInfo{
                .buffer = _buffer,
                .offset = static_cast<VkDeviceSize>(frameIndex) * kVkmFrameConstantStride,
                .range  = sizeof(VkmFrameConstants),
            };
            writes[frameIndex] = VkWriteDescriptorSet{
                .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet          = _descriptorSets[frameIndex],
                .dstBinding      = kVkmFrameConstantBinding,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .pBufferInfo     = &bufferInfos[frameIndex],
            };
        }
        vkUpdateDescriptorSets(device, FRAME_COUNT, writes.data(), 0, nullptr);

        return true;
    }

    void VkmFrameConstantManagerVulkan::destroy()
    {
        VkDevice device = _driver->getDevice();

        if (_descriptorPool != VK_NULL_HANDLE)
        {
            // Frees its sets with it.
            vkDestroyDescriptorPool(device, _descriptorPool, nullptr);
            _descriptorPool = VK_NULL_HANDLE;
            _descriptorSets.fill(VK_NULL_HANDLE);
        }
        if (_setLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device, _setLayout, nullptr);
            _setLayout = VK_NULL_HANDLE;
        }
        if (_buffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(_driver->getVmaAllocator(), _buffer, static_cast<VmaAllocation>(_allocation));
            _buffer = VK_NULL_HANDLE;
            _allocation = nullptr;
            _mappedPointer = nullptr;
        }
    }

    void VkmFrameConstantManagerVulkan::update(uint32_t frameIndex, const VkmFrameConstants& constants)
    {
        VKM_ASSERT(frameIndex < FRAME_COUNT, "Frame slot index out of range");
        if (_mappedPointer == nullptr)
        {
            return;
        }
        _activeFrameIndex = frameIndex;

        const VkDeviceSize offset = static_cast<VkDeviceSize>(frameIndex) * kVkmFrameConstantStride;
        std::memcpy(static_cast<uint8_t*>(_mappedPointer) + offset, &constants, sizeof(VkmFrameConstants));
        // No-op on coherent memory, required on the rest.
        VKM_VK_CHECK_RESULT_MSG(vmaFlushAllocation(_driver->getVmaAllocator(), static_cast<VmaAllocation>(_allocation),
                           offset, sizeof(VkmFrameConstants)),
            "Failed to flush frame-constant buffer");
    }
} // namespace vkm
