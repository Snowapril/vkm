// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/texture.h>
#include <volk/volk.h>

namespace vkm
{
    class VkmTextureVulkan : public VkmTexture
    {
    public:
        VkmTextureVulkan(VkmDriverBase* driver);
        ~VkmTextureVulkan();

        virtual bool initialize(const VkmTextureInfo& info) override final;
        virtual bool overrideExternalHandle(void* externalHandle) override final;

    private:
        VkImage _vkTexture;
    };
} // namespace vkm