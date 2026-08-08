// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/texture.h>
#include <volk.h>

#include <algorithm>
#include <vector>

typedef struct VmaAllocator_T* VmaAllocator;
typedef struct VmaAllocation_T* VmaAllocation;

namespace vkm
{
    class VkmTextureVulkan : public VkmTexture
    {
    public:
        VkmTextureVulkan(VkmDriverBase* driver);
        ~VkmTextureVulkan();

        virtual bool initialize(VkmResourceHandle handle, const VkmTextureInfo& info) override final;
        virtual bool overrideExternalHandle(void* externalHandle) override final;
        virtual bool writeRegion(const void* data, uint64_t size, uint32_t mipLevel, uint32_t arrayLayer) override final;
        virtual void setDebugName(const char* name) override final;

        uint64_t getAllocatedSize() const override { return _allocatedSize; }
        uint32_t getMemoryAlignment() const override { return _alignment; }

        inline VkImage getImage() const { return _vkTexture; }
        inline VkImageView getImageView() const { return _vkImageView; }

        /*
        * @brief The layout of one subresource.
        *
        * @details Layout is tracked per (mip, layer) rather than per texture because several
        * things here touch one subresource at a time and must leave the rest alone: a cubemap
        * uploads face by face, and the probe updater rewrites one atlas cell while every other
        * cell has to keep its contents. A whole-image barrier in those cases either transitions
        * layers it must not, or records a state the tracker cannot honour.
        */
        VkImageLayout getSubresourceLayout(uint32_t mipLevel, uint32_t arrayLayer) const;

        /*
        * @brief The layout shared by every subresource in `range`, or VK_IMAGE_LAYOUT_MAX_ENUM
        * when they disagree -- which is the signal to emit one barrier per run rather than one
        * for the whole range.
        */
        VkImageLayout getUniformLayout(const VkmSubresourceRange& range) const;

        // Records `layout` over `range`. A range covering the whole image collapses the tracker
        // back to its uniform form and frees the per-subresource storage.
        void setSubresourceLayout(const VkmSubresourceRange& range, VkImageLayout layout);

        // True while every subresource shares one layout, which is the overwhelmingly common case
        // and the one that costs a single VkImageLayout and one whole-image barrier.
        inline bool isLayoutUniform() const { return _subresourceLayouts.empty(); }

    private:
        bool createDefaultView();
        // Mip-major index into _subresourceLayouts, matching how a Vulkan subresource range walks.
        inline uint32_t subresourceIndex(uint32_t mipLevel, uint32_t arrayLayer) const
        {
            return arrayLayer * std::max(1u, _textureInfo._numMipLevels) + mipLevel;
        }
        // Materializes _subresourceLayouts from _uniformLayout, on the first partial transition.
        void makeLayoutsPerSubresource();

    private:
        VkImage _vkTexture;
        VkImageView _vkImageView{VK_NULL_HANDLE};
        // Valid while _subresourceLayouts is empty; otherwise that vector is the authority.
        VkImageLayout _uniformLayout{VK_IMAGE_LAYOUT_UNDEFINED};
        std::vector<VkImageLayout> _subresourceLayouts;
        VmaAllocation _vmaAllocation{nullptr};
        uint64_t _allocatedSize{0};
        uint32_t _alignment{0};
    };
} // namespace vkm