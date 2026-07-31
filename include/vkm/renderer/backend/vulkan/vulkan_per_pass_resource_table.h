// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/per_pass_resource_table.h>

#include <volk.h>

namespace vkm
{
    /*
    * @brief Vulkan set 2: one descriptor set allocated from this table's own pool and written once.
    *
    * @details A pool per table rather than one shared driver-wide pool. Tables are few (one per
    * pass, not per draw) and immutable, so there is nothing to amortize across them, and a
    * dedicated pool means destroy() reclaims everything without needing
    * VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT or a free-list. The bindless and
    * frame-constant managers own their pools for the same reason.
    */
    class VkmPerPassResourceTableVulkan : public VkmPerPassResourceTableBase
    {
    public:
        explicit VkmPerPassResourceTableVulkan(VkmDriverBase* driver);
        ~VkmPerPassResourceTableVulkan() override;

        inline VkDescriptorSet getDescriptorSet() const { return _descriptorSet; }

    protected:
        bool createInner(const std::vector<VkmPerPassResourceEntry>& entries, std::string* outError) override final;
        void destroyInner() override final;

    private:
        VkDescriptorPool _descriptorPool{VK_NULL_HANDLE};
        VkDescriptorSet _descriptorSet{VK_NULL_HANDLE};
    };
} // namespace vkm
