// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/texture.h>
#include <volk.h>

namespace vkm
{
    class VkmTextureVulkan : public VkmTexture
    {
    public:
        VkmTextureVulkan(VkmDriverBase* driver);
        ~VkmTextureVulkan();

        virtual bool initialize(VkmResourceHandle handle, const VkmTextureInfo& info) override final;
        virtual bool overrideExternalHandle(void* externalHandle) override final;
        virtual void setDebugName(const char* name) override final;

        inline VkImage getImage() const { return _vkTexture; }
        inline VkImageView getImageView() const { return _vkImageView; }
        inline VkImageLayout getCurrentLayout() const { return _currentLayout; }
        inline void setCurrentLayout(VkImageLayout layout) { _currentLayout = layout; }

    private:
        VkImage _vkTexture;
        VkImageView _vkImageView{VK_NULL_HANDLE};
        VkImageLayout _currentLayout{VK_IMAGE_LAYOUT_UNDEFINED};
    };
} // namespace vkm