// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_texture.h>

namespace vkm
{
    VkmTextureVulkan::VkmTextureVulkan(VkmDriverBase* driver)
        : VkmTexture(driver)
    {
    }

    VkmTextureVulkan::~VkmTextureVulkan()
    {
    }

    bool VkmTextureVulkan::initialize(const VkmTextureInfo& info, void* externalHandleOrNull)
    {
        if (!initializeCommon(info))
        {
            return false;
        }

        if (externalHandleOrNull != nullptr)
        {
            _vkTexture = static_cast<VkImage>(externalHandleOrNull);
        }
        else if ((info._flags & VkmResourceCreateInfo::DeferredCreation) == 0)
        {
            // TODO(snowapril) : create texture with info
        }

        return true;
    }
} // namespace vkm