// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_texture.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_util.h>

#include <vk_mem_alloc.h>

namespace vkm
{
    namespace
    {
        // Decision policy: explicit hint always wins; Auto forces a dedicated allocation for
        // large or attachment/external textures (few, long-lived, resize-churny -- VMA's own
        // guidance recommends dedicated for render targets) and otherwise leaves the dedicated
        // bit unset, letting VMA's internal suballocator place it (VMA still respects
        // driver-reported dedicated-allocation requirements even when we don't force the bit).
        constexpr uint64_t POOLING_SIZE_THRESHOLD_BYTES = 16ull * 1024 * 1024;

        bool shouldUseDedicatedTexture(const VkmTextureInfo& info)
        {
            if (info._placementHint == VkmMemoryPlacementHint::ForceCommitted)
            {
                return true;
            }
            if (info._placementHint == VkmMemoryPlacementHint::ForcePooled)
            {
                return false;
            }

            const uint64_t approxBytes = (uint64_t)info._extent.x * info._extent.y * info._extent.z *
                info._numArrayLayers * vkFormatBytesPerTexel(info._format);
            if (approxBytes >= POOLING_SIZE_THRESHOLD_BYTES)
            {
                return true;
            }
            if ((info._flags & VkmResourceCreateInfo::AllowColorAttachment) != 0 ||
                (info._flags & VkmResourceCreateInfo::AllowDepthStencilAttachment) != 0)
            {
                return true;
            }
            if ((info._flags & VkmResourceCreateInfo::ExternalHandleOwner) != 0)
            {
                return true;
            }
            return false;
        }
    }

    VkmTextureVulkan::VkmTextureVulkan(VkmDriverBase* driver)
        : VkmTexture(driver), _vkTexture(VK_NULL_HANDLE)
    {
    }

    VkmTextureVulkan::~VkmTextureVulkan()
    {
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        if (_vkImageView != VK_NULL_HANDLE)
        {
            vkDestroyImageView(driverVulkan->getDevice(), _vkImageView, nullptr);
            _vkImageView = VK_NULL_HANDLE;
        }
        if (_vmaAllocation != nullptr)
        {
            vmaDestroyImage(driverVulkan->getVmaAllocator(), _vkTexture, _vmaAllocation);
            _vkTexture = VK_NULL_HANDLE;
            _vmaAllocation = nullptr;
        }
    }

    bool VkmTextureVulkan::initialize(VkmResourceHandle handle, const VkmTextureInfo& info)
    {
        if (!initializeTextureCommon(handle, info))
        {
            return false;
        }

        if ((info._flags & VkmResourceCreateInfo::DeferredCreation) != 0 ||
            (info._flags & VkmResourceCreateInfo::ExternalHandleOwner) != 0)
        {
            return true;
        }

        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);

        const VkImageCreateInfo imageCreateInfo{
            .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType   = VK_IMAGE_TYPE_2D,
            .format      = toVkFormat(info._format),
            .extent      = {info._extent.x, info._extent.y, info._extent.z},
            .mipLevels   = info._numMipLevels,
            .arrayLayers = info._numArrayLayers,
            .samples     = VK_SAMPLE_COUNT_1_BIT,
            .tiling      = VK_IMAGE_TILING_OPTIMAL,
            .usage       = toVkImageUsageFlags(info._flags),
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        VmaAllocationCreateInfo allocCreateInfo{};
        allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocCreateInfo.flags = shouldUseDedicatedTexture(info) ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0;

        const VkResult vkResult = vmaCreateImage(driverVulkan->getVmaAllocator(), &imageCreateInfo, &allocCreateInfo, &_vkTexture, &_vmaAllocation, nullptr);
        if (!VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to create image via VMA"))
        {
            return false;
        }

        _currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        return createDefaultView();
    }

    bool VkmTextureVulkan::overrideExternalHandle(void* externalHandle)
    {
        _vkTexture = static_cast<VkImage>(externalHandle);
        _currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        return createDefaultView();
    }

    bool VkmTextureVulkan::createDefaultView()
    {
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);

        if (_vkImageView != VK_NULL_HANDLE)
        {
            vkDestroyImageView(driverVulkan->getDevice(), _vkImageView, nullptr);
            _vkImageView = VK_NULL_HANDLE;
        }

        VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        if (hasDepth(_textureInfo._format))
        {
            aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
            if (hasStencil(_textureInfo._format))
            {
                aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
            }
        }

        const VkImageViewCreateInfo viewCreateInfo{
            .sType            = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image            = _vkTexture,
            .viewType         = (_textureInfo._numArrayLayers > 1) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D,
            .format           = toVkFormat(_textureInfo._format),
            .subresourceRange = {
                .aspectMask     = aspectMask,
                .baseMipLevel   = 0,
                .levelCount     = _textureInfo._numMipLevels,
                .baseArrayLayer = 0,
                .layerCount     = _textureInfo._numArrayLayers,
            },
        };
        VkResult vkResult = vkCreateImageView(driverVulkan->getDevice(), &viewCreateInfo, nullptr, &_vkImageView);
        return VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to create image view");
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