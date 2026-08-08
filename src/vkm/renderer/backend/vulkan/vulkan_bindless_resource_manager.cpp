// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_bindless_resource_manager.h>
#include <vkm/renderer/backend/vulkan/vulkan_acceleration_structure.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_buffer.h>
#include <vkm/renderer/backend/vulkan/vulkan_texture.h>
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

        // Created here rather than through VkmDriverBase::newSampler(): the bindless manager
        // is initialized from initializeInner(), which runs before the render resource pool
        // is initialized, so no pooled resource can be created yet.
        const VkSamplerCreateInfo samplerCreateInfo{
            .sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter    = VK_FILTER_LINEAR,
            .minFilter    = VK_FILTER_LINEAR,
            .mipmapMode   = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            // REPEAT because that is glTF's default wrap, and material textures are what this
            // sampler mostly serves: DamagedHelmet's V runs 1.0..2.0 and relies on wrapping, which
            // under clamp collapsed every pixel onto the texture's bottom row. The cubemap is
            // unaffected -- a cube lookup derives in-range face coordinates and hardware seamless
            // filtering handles the edges, so the address mode never applies there.
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .maxLod       = VK_LOD_CLAMP_NONE,
        };
        VkResult vkResult = vkCreateSampler(device, &samplerCreateInfo, nullptr, &_defaultSampler);
        if (!VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to create the bindless default sampler"))
        {
            return false;
        }

        // Bindings 0-2 are the runtime-sized arrays, 3 the immutable sampler,
        // kVkmBindlessFirstSingletonBinding onwards the single-descriptor singleton buffers (one
        // per VkmBindlessSingletonBuffer in that enum's order), and -- only on a device that
        // reports ray tracing -- the scene acceleration structure after them.
        //
        // Conditional because VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR is not a legal
        // descriptor type without VK_KHR_acceleration_structure enabled, and MoltenVK enables
        // none of the RT extensions. Nothing is lost: the only shaders that declare the binding
        // are ray-query shaders, which cannot be created on such a device either way.
        _hasAccelerationStructureBinding =
            (_driver->getDriverCapabilityFlags() & VkmDriverCapabilityFlags::RayTracing) != 0;

        constexpr uint32_t kSingletonCount = static_cast<uint32_t>(VkmBindlessSingletonBuffer::Count);
        constexpr uint32_t kMaxBindingCount = kVkmBindlessAccelerationStructureBinding + 1;
        const uint32_t bindingCount =
            _hasAccelerationStructureBinding ? kMaxBindingCount : kVkmBindlessAccelerationStructureBinding;

        std::array<VkDescriptorSetLayoutBinding, kMaxBindingCount> bindings{
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
            // Immutable: baked into the layout, so it is never written and can never be the
            // "descriptor never updated" failure the other three bindings have to avoid.
            VkDescriptorSetLayoutBinding{
                .binding            = kVkmBindlessSamplerBinding,
                .descriptorType     = VK_DESCRIPTOR_TYPE_SAMPLER,
                .descriptorCount    = 1,
                .stageFlags         = VK_SHADER_STAGE_ALL,
                .pImmutableSamplers = &_defaultSampler,
            },
        };
        for (uint32_t i = 0; i < kSingletonCount; ++i)
        {
            bindings[kVkmBindlessFirstSingletonBinding + i] = VkDescriptorSetLayoutBinding{
                .binding         = kVkmBindlessFirstSingletonBinding + i,
                .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .descriptorCount = 1,
                .stageFlags      = VK_SHADER_STAGE_ALL,
            };
        }
        if (_hasAccelerationStructureBinding)
        {
            bindings[kVkmBindlessAccelerationStructureBinding] = VkDescriptorSetLayoutBinding{
                .binding         = kVkmBindlessAccelerationStructureBinding,
                .descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
                .descriptorCount = 1,
                .stageFlags      = VK_SHADER_STAGE_ALL,
            };
        }

        constexpr VkDescriptorBindingFlags kBindingFlags =
            VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
        std::array<VkDescriptorBindingFlags, kMaxBindingCount> bindingFlags{};
        bindingFlags.fill(kBindingFlags);
        // The immutable sampler needs no update-after-bind: it is never updated at all.
        bindingFlags[kVkmBindlessSamplerBinding] = 0;

        const VkDescriptorSetLayoutBindingFlagsCreateInfo bindingFlagsCreateInfo{
            .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO,
            .bindingCount  = bindingCount,
            .pBindingFlags = bindingFlags.data(),
        };

        const VkDescriptorSetLayoutCreateInfo layoutCreateInfo{
            .sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext        = &bindingFlagsCreateInfo,
            .flags        = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
            .bindingCount = bindingCount,
            .pBindings    = bindings.data(),
        };

        vkResult = vkCreateDescriptorSetLayout(device, &layoutCreateInfo, nullptr, &_setLayout);
        if (!VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to create bindless descriptor set layout"))
        {
            return false;
        }

        // The immutable sampler still consumes pool capacity, so it needs a pool size like
        // any other binding.
        const std::array<VkDescriptorPoolSize, 6> poolSizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, TEXTURE_CAPACITY},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, BUFFER_CAPACITY},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, INDEX_BUFFER_CAPACITY},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_SAMPLER, 1},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kSingletonCount},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1},
        };
        const VkDescriptorPoolCreateInfo poolCreateInfo{
            .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .flags         = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
            .maxSets       = 1,
            // The acceleration structure size is dropped along with its binding: a pool sized for
            // a descriptor type the device does not support is itself invalid.
            .poolSizeCount = _hasAccelerationStructureBinding ? static_cast<uint32_t>(poolSizes.size())
                                                              : static_cast<uint32_t>(poolSizes.size()) - 1,
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
        // After the layout that referenced it as an immutable sampler.
        if (_defaultSampler != VK_NULL_HANDLE)
        {
            vkDestroySampler(device, _defaultSampler, nullptr);
            _defaultSampler = VK_NULL_HANDLE;
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

    uint32_t VkmBindlessResourceManagerVulkan::registerTexture(VkmResourceHandle textureHandle)
    {
        VkmTextureVulkan* textureVulkan = static_cast<VkmTextureVulkan*>(
            _driver->getRenderResourcePool()->getResource<VkmTexture>(textureHandle));
        if (textureVulkan == nullptr)
        {
            VKM_DEBUG_ERROR("registerTexture: invalid texture handle");
            return UINT32_MAX;
        }

        const uint32_t slot = _textureSlots.allocate();
        if (slot == UINT32_MAX)
        {
            VKM_DEBUG_ERROR("Bindless texture array is exhausted");
            return UINT32_MAX;
        }

        // The texture's own default view: created from VkmTextureInfo::_type, so a cube
        // texture is already a VK_IMAGE_VIEW_TYPE_CUBE view here.
        const VkDescriptorImageInfo imageInfo{
            .imageView   = textureVulkan->getImageView(),
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        };

        const VkWriteDescriptorSet write{
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = _descriptorSet,
            .dstBinding      = 0,
            .dstArrayElement = slot,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
            .pImageInfo      = &imageInfo,
        };
        vkUpdateDescriptorSets(_driver->getDevice(), 1, &write, 0, nullptr);

        return slot;
    }

    void VkmBindlessResourceManagerVulkan::unregisterTexture(uint32_t slot)
    {
        _textureSlots.release(slot);
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

    bool VkmBindlessResourceManagerVulkan::setSingletonBuffer(VkmBindlessSingletonBuffer which, VkmResourceHandle bufferHandle)
    {
        VKM_ASSERT(which < VkmBindlessSingletonBuffer::Count, "Unknown VkmBindlessSingletonBuffer");

        // The binding is declared PARTIALLY_BOUND, so leaving it unwritten is legal as long as no
        // shader reads it -- there is nothing to undo on an unbind.
        if (bufferHandle == VKM_INVALID_RESOURCE_HANDLE)
        {
            return true;
        }

        VkmBufferVulkan* bufferVulkan = static_cast<VkmBufferVulkan*>(
            _driver->getRenderResourcePool()->getResource<VkmBuffer>(bufferHandle));
        if (bufferVulkan == nullptr)
        {
            VKM_DEBUG_ERROR("setSingletonBuffer was given a handle that is not a live buffer");
            return false;
        }

        // Same pooled-sub-allocation caveat as registerBuffer().
        const VkDescriptorBufferInfo bufferInfo{
            .buffer = bufferVulkan->getBuffer(),
            .offset = bufferVulkan->getBufferOffset(),
            .range  = bufferVulkan->getBufferInfo()._size,
        };

        const VkWriteDescriptorSet write{
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet          = _descriptorSet,
            .dstBinding      = kVkmBindlessFirstSingletonBinding + static_cast<uint32_t>(which),
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo     = &bufferInfo,
        };
        vkUpdateDescriptorSets(_driver->getDevice(), 1, &write, 0, nullptr);
        return true;
    }

    bool VkmBindlessResourceManagerVulkan::setAccelerationStructure(VkmResourceHandle accelerationStructureHandle)
    {
        if (!_hasAccelerationStructureBinding)
        {
            VKM_DEBUG_ERROR("setAccelerationStructure on a device that reports no ray tracing");
            return false;
        }

        // PARTIALLY_BOUND, like the singletons: unbinding leaves the descriptor unwritten, which
        // is legal as long as no shader reads it.
        if (accelerationStructureHandle == VKM_INVALID_RESOURCE_HANDLE)
        {
            return true;
        }

        VkmAccelerationStructureVulkan* structure = static_cast<VkmAccelerationStructureVulkan*>(
            _driver->getRenderResourcePool()->getResource<VkmAccelerationStructure>(accelerationStructureHandle));
        if (structure == nullptr)
        {
            VKM_DEBUG_ERROR("setAccelerationStructure was given a handle that is not a live acceleration structure");
            return false;
        }

        const VkAccelerationStructureKHR handle = structure->getAccelerationStructure();
        const VkWriteDescriptorSetAccelerationStructureKHR accelerationWrite{
            .sType                      = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR,
            .accelerationStructureCount = 1,
            .pAccelerationStructures    = &handle,
        };
        const VkWriteDescriptorSet write{
            .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            // The structure travels in the pNext chain, not in pBufferInfo/pImageInfo -- the only
            // descriptor type in this set that does.
            .pNext           = &accelerationWrite,
            .dstSet          = _descriptorSet,
            .dstBinding      = kVkmBindlessAccelerationStructureBinding,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType  = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR,
        };
        vkUpdateDescriptorSets(_driver->getDevice(), 1, &write, 0, nullptr);
        return true;
    }
} // namespace vkm
