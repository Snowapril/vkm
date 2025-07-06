// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_texture.h>

namespace vkm
{
    VkmTextureVulkan::VkmTextureVulkan(VkmDriverBase* driver)
        : VkmTexture(driver), _vkTexture(VK_NULL_HANDLE)
    {
    }

    VkmTextureVulkan::~VkmTextureVulkan()
    {
    }

    bool VkmTextureVulkan::initialize(VkmResourceHandle handle, const VkmTextureInfo& info, void* externalHandleOrNull)
    {
        if (!initializeTextureCommon(handle, info))
        {
            return false;
        }

        if ((info._flags & VkmResourceCreateInfo::DeferredCreation) == 0)
        {
            // TODO(snowapril) : create texture with info
        }
        return true;
    }

    bool VkmTextureVulkan::overrideExternalHandle(void* externalHandle)
    {
        _vkTexture = static_cast<VkImage>(externalHandle);
        // TODO(snowapril) : validate external handle with current texture info
        return true;
    }
} // namespace vkm