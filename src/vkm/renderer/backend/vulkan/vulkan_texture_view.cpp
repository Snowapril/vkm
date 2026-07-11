// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_texture_view.h>
#include <vkm/renderer/backend/vulkan/vulkan_texture.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_util.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>

namespace vkm
{
    VkmTextureViewVulkan::VkmTextureViewVulkan(VkmDriverBase* driver)
        : VkmTextureView(driver)
    {
    }

    VkmTextureViewVulkan::~VkmTextureViewVulkan()
    {
        if (_vkImageView != VK_NULL_HANDLE)
        {
            VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
            vkDestroyImageView(driverVulkan->getDevice(), _vkImageView, nullptr);
            _vkImageView = VK_NULL_HANDLE;
        }
    }

    bool VkmTextureViewVulkan::initialize(VkmResourceHandle handle, const VkmTextureViewInfo& info)
    {
        if (!initializeTextureViewCommon(handle, info))
        {
            return false;
        }

        if (info._texture.type != VkmResourceType::Texture)
        {
            VKM_DEBUG_ERROR("VkmTextureViewInfo::_texture must reference a Texture handle");
            return false;
        }

        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        VkmTextureVulkan* parentTexture = driverVulkan->getRenderResourcePool()->getResource<VkmTextureVulkan>(info._texture);
        if (parentTexture == nullptr)
        {
            VKM_DEBUG_ERROR("VkmTextureViewInfo::_texture does not resolve to a live texture");
            return false;
        }

        const VkmFormat resolvedFormat = (info._format != VkmFormat::Undefined) ? info._format : parentTexture->getTextureInfo()._format;
        const VkFormat format = toVkFormat(resolvedFormat);

        VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        if (hasDepth(resolvedFormat))
        {
            aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            if (hasStencil(resolvedFormat))
            {
                aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
            }
        }

        const uint32_t effectiveArrayLayers = (info._numArrayLayers != UINT32_MAX)
            ? info._numArrayLayers
            : (parentTexture->getTextureInfo()._numArrayLayers - info._baseArrayLayer);

        // UINT32_MAX numerically matches Vulkan's own VK_REMAINING_MIP_LEVELS /
        // VK_REMAINING_ARRAY_LAYERS sentinels, so these pass through unchanged.
        const VkImageViewCreateInfo viewCreateInfo{
            .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image            = parentTexture->getImage(),
            .viewType         = (effectiveArrayLayers > 1) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
            .format           = format,
            .subresourceRange = {
                .aspectMask     = aspectMask,
                .baseMipLevel   = info._baseMipLevel,
                .levelCount     = info._numMipLevels,
                .baseArrayLayer = info._baseArrayLayer,
                .layerCount     = info._numArrayLayers,
            },
        };
        const VkResult vkResult = vkCreateImageView(driverVulkan->getDevice(), &viewCreateInfo, nullptr, &_vkImageView);
        return VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to create texture view");
    }

    void VkmTextureViewVulkan::setDebugName(const char* name)
    {
#ifdef VKM_DEBUG_NAME_ENABLED
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        const VkDebugUtilsObjectNameInfoEXT nameInfo{
            .sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
            .objectType   = VK_OBJECT_TYPE_IMAGE_VIEW,
            .objectHandle = reinterpret_cast<uint64_t>(_vkImageView),
            .pObjectName  = name,
        };
        vkSetDebugUtilsObjectNameEXT(driverVulkan->getDevice(), &nameInfo);
#else
        (void)name;
#endif
    }
} // namespace vkm
