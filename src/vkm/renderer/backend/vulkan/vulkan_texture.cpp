// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_texture.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_util.h>

namespace vkm
{
    VkmTextureVulkan::VkmTextureVulkan(VkmDriverBase* driver)
        : VkmTexture(driver), _vkTexture(VK_NULL_HANDLE)
    {
    }

    VkmTextureVulkan::~VkmTextureVulkan()
    {
        if (_vkImageView != VK_NULL_HANDLE)
        {
            VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
            vkDestroyImageView(driverVulkan->getDevice(), _vkImageView, nullptr);
            _vkImageView = VK_NULL_HANDLE;
        }
    }

    bool VkmTextureVulkan::initialize(VkmResourceHandle handle, const VkmTextureInfo& info)
    {
        if (!initializeTextureCommon(handle, info))
        {
            return false;
        }

        if ((info._flags & VkmResourceCreateInfo::DeferredCreation) == 0 &&
            (info._flags & VkmResourceCreateInfo::ExternalHandleOwner) == 0)
        {
            // TODO(snowapril) : create texture with info
        }
        return true;
    }

    bool VkmTextureVulkan::overrideExternalHandle(void* externalHandle)
    {
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);

        _vkTexture = static_cast<VkImage>(externalHandle);
        _currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (_vkImageView != VK_NULL_HANDLE)
        {
            vkDestroyImageView(driverVulkan->getDevice(), _vkImageView, nullptr);
            _vkImageView = VK_NULL_HANDLE;
        }

        const VkImageViewCreateInfo viewCreateInfo{
            .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image            = _vkTexture,
            .viewType         = VK_IMAGE_VIEW_TYPE_2D,
            .format           = toVkFormat(_textureInfo._format),
            .subresourceRange = {
                .aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel   = 0,
                .levelCount     = 1,
                .baseArrayLayer = 0,
                .layerCount     = 1,
            },
        };
        VkResult vkResult = vkCreateImageView(driverVulkan->getDevice(), &viewCreateInfo, nullptr, &_vkImageView);
        return VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to create swapchain image view");
    }

    void VkmTextureVulkan::setDebugName(const char* name)
    {
#ifdef VKM_DEBUG_NAME_ENABLED
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        const VkDebugUtilsObjectNameInfoEXT nameInfo{
            .sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
            .objectType   = VK_OBJECT_TYPE_IMAGE,
            .objectHandle = reinterpret_cast<uint64_t>(_vkTexture),
            .pObjectName  = name,
        };
        vkSetDebugUtilsObjectNameEXT(driverVulkan->getDevice(), &nameInfo);
#else
        (void)name;
#endif
    }
} // namespace vkm