// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_bindless_resource_manager.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_buffer.h>
#include <vkm/renderer/backend/vulkan/vulkan_util.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>

#include <array>

namespace vkm
{
    VkmBindlessResourceManagerVulkan::VkmBindlessResourceManagerVulkan(VkmDriverVulkan* driver)
        : _driver(driver)
    {
    }

    VkmBindlessResourceManagerVulkan::~VkmBindlessResourceManagerVulkan()
    {
    }

    bool VkmBindlessResourceManagerVulkan::initialize()
    {
        VkDevice device = _driver->getDevice();

        const std::array<VkDescriptorSetLayoutBinding, 3> bindings{
            VkDescriptorSetLayoutBinding{
                .binding         = 0,
                .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                .descriptorCount = TEXTURE_CAPACITY,
                .stageFlags      = VK_SHADER_STAGE_ALL,
            },
            VkDescriptorSetLayoutBinding{
                .binding         = 1,
                .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = BUFFER_CAPACITY,
                .stageFlags      = VK_SHADER_STAGE_ALL,
            },
            VkDescriptorSetLayoutBinding{
                .binding         = 2,
                .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = INDEX_BUFFER_CAPACITY,
                .stageFlags      = VK_SHADER_STAGE_ALL,
            },
        };

        constexpr VkDescriptorBindingFlags kBindingFlags =
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        const std::array<VkDescriptorBindingFlags, 3> bindingFlags{kBindingFlags, kBindingFlags, kBindingFlags};

        const VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCreateInfo{
            .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .bindingCount  = static_cast<uint32_t>(bindingFlags.size()),
            .pBindingFlags = bindingFlags.data(),
        };

        const VkDescriptorSetLayoutCreateInfo layoutCreateInfo{
            .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext        = &bindingFlagsCreateInfo,
            .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
            .bindingCount = static_cast<uint32_t>(bindings.size()),
            .pBindings    = bindings.data(),
        };

        VkResult vkResult = vkCreateDescriptorSetLayout(device, &layoutCreateInfo, nullptr, &_setLayout);
        if (!VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to create bindless descriptor set layout"))
        {
            return false;
        }

        const std::array<VkDescriptorPoolSize, 3> poolSizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, TEXTURE_CAPACITY},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, BUFFER_CAPACITY},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, INDEX_BUFFER_CAPACITY},
        };
        const VkDescriptorPoolCreateInfo poolCreateInfo{
            .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
            .maxSets       = 1,
            .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
            .pPoolSizes    = poolSizes.data(),
        };
        vkResult = vkCreateDescriptorPool(device, &poolCreateInfo, nullptr, &_descriptorPool);
        if (!VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to create bindless descriptor pool"))
        {
            vkDestroyDescriptorSetLayout(device, _setLayout, nullptr);
            _setLayout = VK_NULL_HANDLE;
            return false;
        }

        const VkDescriptorSetAllocateInfo allocateInfo{
            .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool     = _descriptorPool,
            .descriptorSetCount = 1,
            .pSetLayouts        = &_setLayout,
        };
        vkResult = vkAllocateDescriptorSets(device, &allocateInfo, &_descriptorSet);
        if (!VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to allocate bindless descriptor set"))
        {
            vkDestroyDescriptorPool(device, _descriptorPool, nullptr);
            _descriptorPool = VK_NULL_HANDLE;
            vkDestroyDescriptorSetLayout(device, _setLayout, nullptr);
            _setLayout = VK_NULL_HANDLE;
            return false;
        }

        return true;
    }

    void VkmBindlessResourceManagerVulkan::destroy()
    {
        VkDevice device = _driver->getDevice();

        if (_descriptorPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(device, _descriptorPool, nullptr);
            _descriptorPool = VK_NULL_HANDLE;
            _descriptorSet = VK_NULL_HANDLE; // implicitly freed with the pool
        }
        if (_setLayout != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorSetLayout(device, _setLayout, nullptr);
            _setLayout = VK_NULL_HANDLE;
        }
    }

    uint32_t VkmBindlessResourceManagerVulkan::allocateSlot(VkmBindlessArrayType arrayType)
    {
        switch (arrayType)
        {
            case VkmBindlessArrayType::Texture:     return _textureSlots.allocate();
            case VkmBindlessArrayType::Buffer:      return _bufferSlots.allocate();
            case VkmBindlessArrayType::IndexBuffer: return _indexBufferSlots.allocate();
        }
        return UINT32_MAX;
    }

    uint32_t VkmBindlessResourceManagerVulkan::registerBuffer(VkmResourceHandle bufferHandle, VkmBindlessArrayType arrayType)
    {
        VKM_ASSERT(arrayType == VkmBindlessArrayType::Buffer || arrayType == VkmBindlessArrayType::IndexBuffer,
            "registerBuffer only accepts Buffer or IndexBuffer array types");

        const uint32_t slot = allocateSlot(arrayType);
        if (slot == UINT32_MAX)
        {
            VKM_DEBUG_ERROR("Bindless buffer array is exhausted");
            return UINT32_MAX;
        }

        VkmBufferVulkan* bufferVulkan = static_cast<VkmBufferVulkan*>(_driver->getRenderResourcePool()->getResource<VkmBuffer>(bufferHandle));

        // Use the buffer's real pool offset/size rather than assuming 0/VK_WHOLE_SIZE:
        // small buffers may be sub-allocated into a shared pooled VkBuffer (see
        // VkmBufferVulkan::getBufferOffset()), so offset 0 would silently read the wrong bytes.
        const VkDescriptorBufferInfo bufferInfo{
            .buffer = bufferVulkan->getBuffer(),
            .offset = bufferVulkan->getBufferOffset(),
            .range  = bufferVulkan->getBufferInfo()._size,
        };

        const uint32_t binding = (arrayType == VkmBindlessArrayType::Buffer) ? 1u : 2u;
        const VkWriteDescriptorSet write{
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = _descriptorSet,
            .dstBinding      = binding,
            .dstArrayElement = slot,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo     = &bufferInfo,
        };
        // Update-after-bind means this write is safe even while a previously-recorded
        // command buffer referencing this set is in flight.
        vkUpdateDescriptorSets(_driver->getDevice(), 1, &write, 0, nullptr);

        return slot;
    }

    void VkmBindlessResourceManagerVulkan::unregisterBuffer(uint32_t slot, VkmBindlessArrayType arrayType)
    {
        switch (arrayType)
        {
            case VkmBindlessArrayType::Buffer:      _bufferSlots.release(slot); break;
            case VkmBindlessArrayType::IndexBuffer: _indexBufferSlots.release(slot); break;
            case VkmBindlessArrayType::Texture:     _textureSlots.release(slot); break;
        }
    }
} // namespace vkm
