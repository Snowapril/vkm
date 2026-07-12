// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/texture_view.h>
#include <volk.h>

namespace vkm
{
    class VkmTextureViewVulkan : public VkmTextureView
    {
    public:
        VkmTextureViewVulkan(VkmDriverBase* driver);
        ~VkmTextureViewVulkan();

        virtual bool initialize(VkmResourceHandle handle, const VkmTextureViewInfo& info) override final;
        virtual void setDebugName(const char* name) override final;

        inline VkImageView getImageView() const { return _vkImageView; }

    private:
        VkImageView _vkImageView{VK_NULL_HANDLE};
    };
} // namespace vkm
