// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/texture.h>

namespace vkm
{
    class VkmTextureVulkan : public VkmTexture
    {
    public:
        VkmTextureVulkan(VkmDriverBase* driver);
        ~VkmTextureVulkan();

        virtual bool initialize(const VkmTextureInfo& info, void* externalHandleOrNull = nullptr) override final;

    private:
        VkTexture _vkTexture;
    };
} // namespace vkm