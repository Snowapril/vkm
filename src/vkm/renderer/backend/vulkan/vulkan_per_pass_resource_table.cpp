// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_per_pass_resource_table.h>

#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/vulkan/vulkan_buffer.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_pipeline_state.h>
#include <vkm/renderer/backend/vulkan/vulkan_sampler.h>
#include <vkm/renderer/backend/vulkan/vulkan_texture.h>
#include <vkm/renderer/backend/vulkan/vulkan_util.h>

#include <array>
#include <vector>

namespace vkm
{
    namespace
    {
        void setError(std::string* outError, const std::string& message)
        {
            VKM_DEBUG_ERROR(message.c_str());
            if (outError != nullptr && outError->empty())
            {
                *outError = message;
            }
        }

        VkDescriptorType toVkDescriptorType(VkmPerPassResourceType type)
        {
            switch (type)
            {
                case VkmPerPassResourceType::SampledTexture: return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                case VkmPerPassResourceType::Sampler:        return VK_DESCRIPTOR_TYPE_SAMPLER;
                case VkmPerPassResourceType::StorageBuffer:  return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                case VkmPerPassResourceType::UniformBuffer:  return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            }
            return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        }
    }

    VkmPerPassResourceTableVulkan::VkmPerPassResourceTableVulkan(VkmDriverBase* driver)
        : VkmPerPassResourceTableBase(driver)
    {
    }

    VkmPerPassResourceTableVulkan::~VkmPerPassResourceTableVulkan()
    {
        destroyInner();
    }

    bool VkmPerPassResourceTableVulkan::createInner(const std::vector<VkmPerPassResourceEntry>& entries,
                                                    std::string* outError)
    {
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        VkDevice device = driverVulkan->getDevice();

        const VkmPipelineStateVulkan* pipelineStateVulkan =
            static_cast<const VkmPipelineStateVulkan*>(_pipelineState);
        VkDescriptorSetLayout setLayout = pipelineStateVulkan->getPerPassSetLayout();
        if (setLayout == VK_NULL_HANDLE)
        {
            setError(outError, "Pipeline '" + _pipelineState->getName() + "' has no set-2 layout");
            return false;
        }

        const std::vector<VkmPerPassResourceBinding>& declaration =
            _pipelineState->getDescriptor().perPassResources;

        // Exactly what this table needs and nothing more -- the pool is its own, so it is sized
        // from the declaration rather than from a guess about the engine's peak usage.
        std::vector<VkDescriptorPoolSize> poolSizes;
        for (const VkmPerPassResourceBinding& declared : declaration)
        {
            const VkDescriptorType descriptorType = toVkDescriptorType(declared.type);
            const auto existing = std::find_if(poolSizes.begin(), poolSizes.end(),
                [descriptorType](const VkDescriptorPoolSize& size) { return size.type == descriptorType; });
            if (existing != poolSizes.end())
            {
                ++existing->descriptorCount;
            }
            else
            {
                poolSizes.push_back(VkDescriptorPoolSize{descriptorType, 1});
            }
        }

        const VkDescriptorPoolCreateInfo poolCreateInfo{
            .sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets       = 1,
            .poolSizeCount = static_cast<uint32_t>(poolSizes.size()),
            .pPoolSizes    = poolSizes.data(),
        };
        VkResult vkResult = vkCreateDescriptorPool(device, &poolCreateInfo, nullptr, &_descriptorPool);
        if (!VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to create per-pass descriptor pool"))
        {
            setError(outError, "Failed to create the set-2 descriptor pool for " + _pipelineState->getName());
            return false;
        }

        const VkDescriptorSetAllocateInfo allocateInfo{
            .sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool     = _descriptorPool,
            .descriptorSetCount = 1,
            .pSetLayouts        = &setLayout,
        };
        vkResult = vkAllocateDescriptorSets(device, &allocateInfo, &_descriptorSet);
        if (!VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to allocate per-pass descriptor set"))
        {
            setError(outError, "Failed to allocate the set-2 descriptor set for " + _pipelineState->getName());
            destroyInner();
            return false;
        }

        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();

        // The info structs must outlive the vkUpdateDescriptorSets call, so they are collected up
        // front rather than built inside the loop that references them. Reserved so no push_back
        // reallocates while a VkWriteDescriptorSet already points into them.
        std::vector<VkDescriptorImageInfo> imageInfos;
        std::vector<VkDescriptorBufferInfo> bufferInfos;
        imageInfos.reserve(entries.size());
        bufferInfos.reserve(entries.size());
        std::vector<VkWriteDescriptorSet> writes;
        writes.reserve(entries.size());

        // `entries` arrives in declaration order (the base class reorders it), so the two walk in
        // lockstep.
        for (size_t i = 0; i < entries.size(); ++i)
        {
            const VkmPerPassResourceEntry& entry = entries[i];
            const VkmPerPassResourceBinding& declared = declaration[i];

            VkWriteDescriptorSet write{
                .sType           = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet          = _descriptorSet,
                .dstBinding      = declared.binding,
                .descriptorCount = 1,
                .descriptorType  = toVkDescriptorType(declared.type),
            };

            switch (declared.type)
            {
                case VkmPerPassResourceType::SampledTexture:
                {
                    VkmTextureVulkan* textureVulkan = static_cast<VkmTextureVulkan*>(
                        renderResourcePool->getResource<VkmTexture>(entry.resource));
                    if (textureVulkan == nullptr)
                    {
                        setError(outError, "Binding " + std::to_string(declared.binding) +
                                               " was given a handle that is not a live texture");
                        destroyInner();
                        return false;
                    }
                    // The layout the descriptor promises. barrierTextureForShaderRead() is what
                    // gets a render target here; an uploaded texture already is.
                    imageInfos.push_back(VkDescriptorImageInfo{
                        .imageView   = textureVulkan->getImageView(),
                        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    });
                    write.pImageInfo = &imageInfos.back();
                    break;
                }
                case VkmPerPassResourceType::Sampler:
                {
                    VkmSamplerVulkan* samplerVulkan = static_cast<VkmSamplerVulkan*>(
                        renderResourcePool->getResource<VkmSampler>(entry.resource));
                    if (samplerVulkan == nullptr)
                    {
                        setError(outError, "Binding " + std::to_string(declared.binding) +
                                               " was given a handle that is not a live sampler");
                        destroyInner();
                        return false;
                    }
                    imageInfos.push_back(VkDescriptorImageInfo{ .sampler = samplerVulkan->getSampler() });
                    write.pImageInfo = &imageInfos.back();
                    break;
                }
                case VkmPerPassResourceType::StorageBuffer:
                case VkmPerPassResourceType::UniformBuffer:
                {
                    VkmBufferVulkan* bufferVulkan = static_cast<VkmBufferVulkan*>(
                        renderResourcePool->getResource<VkmBuffer>(entry.resource));
                    if (bufferVulkan == nullptr)
                    {
                        setError(outError, "Binding " + std::to_string(declared.binding) +
                                               " was given a handle that is not a live buffer");
                        destroyInner();
                        return false;
                    }
                    // Sub-allocated buffers carry a non-zero offset within their pooled block.
                    bufferInfos.push_back(VkDescriptorBufferInfo{
                        .buffer = bufferVulkan->getBuffer(),
                        .offset = bufferVulkan->getBufferOffset(),
                        .range  = bufferVulkan->getBufferInfo()._size,
                    });
                    write.pBufferInfo = &bufferInfos.back();
                    break;
                }
            }
            writes.push_back(write);
        }

        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
        return true;
    }

    void VkmPerPassResourceTableVulkan::destroyInner()
    {
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        VkDevice device = driverVulkan->getDevice();

        // The set is freed with the pool it came from; the pool is this table's own.
        _descriptorSet = VK_NULL_HANDLE;
        if (_descriptorPool != VK_NULL_HANDLE)
        {
            vkDestroyDescriptorPool(device, _descriptorPool, nullptr);
            _descriptorPool = VK_NULL_HANDLE;
        }
    }
} // namespace vkm
