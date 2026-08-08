// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_texture.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_util.h>

#include <vk_mem_alloc.h>

#include <algorithm>

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

        /*
        * Decision policy for CPU-writable texture memory. An OPTIMAL-tiled image cannot be
        * memcpy'd into -- its layout is swizzled -- so host-visible memory alone buys
        * nothing; VK_EXT_host_image_copy is what makes a host-side write correct. Requested
        * only where it also pays off: a unified-memory device, where the GPU reads the same
        * memory the CPU wrote at no extra cost.
        *
        * Restricted to plain upload destinations: render targets and presentables are
        * GPU-written and never CPU-written, so steering them towards host-visible memory
        * would trade attachment bandwidth for a fast path they never take.
        */
        bool shouldUseHostWritableTexture(const VkmDriverVulkan* driverVulkan, const VkmTextureInfo& info)
        {
            if (!driverVulkan->isHostImageCopyEnabled() || !driverVulkan->hasUnifiedMemory())
            {
                return false;
            }
            if ((info._flags & VkmResourceCreateInfo::AllowTransferDst) == 0)
            {
                return false;
            }
            return (info._flags & VkmResourceCreateInfo::AllowColorAttachment) == 0 &&
                   (info._flags & VkmResourceCreateInfo::AllowDepthStencilAttachment) == 0 &&
                   (info._flags & VkmResourceCreateInfo::AllowPresent) == 0;
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

        // Requested, not granted: the image may still land in device-local-only memory, which
        // is why _isHostWritable is decided from the allocation's real properties below.
        const bool requestHostWritable = shouldUseHostWritableTexture(driverVulkan, info);
        VkImageUsageFlags imageUsage = toVkImageUsageFlags(info._flags);
        if (requestHostWritable)
        {
            imageUsage |= VK_IMAGE_USAGE_HOST_TRANSFER_BIT;
        }

        const VkImageCreateInfo imageCreateInfo{
            .sType       = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            // A cubemap is an ordinary 6-layer 2D image plus this flag; without it the
            // cube-typed image view below is invalid.
            .flags       = (info._type == VkmTextureType::Cube) ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0u,
            .imageType   = VK_IMAGE_TYPE_2D,
            .format      = toVkFormat(info._format),
            .extent      = {info._extent.x, info._extent.y, info._extent.z},
            .mipLevels   = info._numMipLevels,
            .arrayLayers = info._numArrayLayers,
            .samples     = VK_SAMPLE_COUNT_1_BIT,
            .tiling      = VK_IMAGE_TILING_OPTIMAL,
            .usage       = imageUsage,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        };

        VmaAllocationCreateInfo allocCreateInfo{};
        allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocCreateInfo.flags = shouldUseDedicatedTexture(info) ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0;

        VmaAllocationInfo vmaAllocationInfo{};
        const VkResult vkResult = vmaCreateImage(driverVulkan->getVmaAllocator(), &imageCreateInfo, &allocCreateInfo, &_vkTexture, &_vmaAllocation, &vmaAllocationInfo);
        if (!VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to create image via VMA"))
        {
            return false;
        }

        _allocatedSize = vmaAllocationInfo.size;
        VkMemoryRequirements memReqs{};
        vkGetImageMemoryRequirements(driverVulkan->getDevice(), _vkTexture, &memReqs);
        _alignment = (uint32_t)memReqs.alignment;

        // The decision the upload path actually reads: HOST_TRANSFER usage makes a host copy
        // legal, but only memory the CPU can reach makes it worth taking. Adding the usage
        // bit can change the image's memory-type requirements, so ask what VMA really picked
        // rather than assuming the request was honored.
        if (requestHostWritable)
        {
            VkMemoryPropertyFlags memoryProperties = 0;
            vmaGetAllocationMemoryProperties(driverVulkan->getVmaAllocator(), _vmaAllocation, &memoryProperties);
            _isHostWritable = (memoryProperties & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) != 0;
        }

        _currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        return createDefaultView();
    }

    bool VkmTextureVulkan::writeRegion(const void* data, uint64_t size, uint32_t mipLevel, uint32_t arrayLayer)
    {
        if (!_isHostWritable || _vkTexture == VK_NULL_HANDLE)
        {
            VKM_DEBUG_ERROR("writeRegion requires a host-writable texture");
            return false;
        }

        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        const VkImageLayout copyLayout = driverVulkan->getHostImageCopyDstLayout();

        const uint32_t mipWidth = std::max(1u, _textureInfo._extent.x >> mipLevel);
        const uint32_t mipHeight = std::max(1u, _textureInfo._extent.y >> mipLevel);
        const uint32_t mipDepth = std::max(1u, _textureInfo._extent.z >> mipLevel);
        const uint64_t expectedSize =
            static_cast<uint64_t>(mipWidth) * mipHeight * mipDepth * vkmBytesPerTexel(_textureInfo._format);
        if (size < expectedSize)
        {
            VKM_DEBUG_ERROR("writeRegion: the source is smaller than the destination mip level");
            return false;
        }

        // Host-side layout transition -- no command buffer, no queue, nothing to wait on,
        // which is the whole point of this path. Whole-image range for the same reason
        // transitionImageLayout uses one: the layout tracker is per-texture, not per-layer.
        if (_currentLayout != copyLayout)
        {
            const VkHostImageLayoutTransitionInfo transitionInfo{
                .sType            = VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO,
                .image            = _vkTexture,
                .oldLayout        = _currentLayout,
                .newLayout        = copyLayout,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS },
            };
            const VkResult transitionResult = vkTransitionImageLayoutEXT(driverVulkan->getDevice(), 1, &transitionInfo);
            if (!VKM_VK_CHECK_RESULT_MSG(transitionResult, "Failed to transition image layout on the host"))
            {
                return false;
            }
            _currentLayout = copyLayout;
        }

        const VkMemoryToImageCopy region{
            .sType             = VK_STRUCTURE_TYPE_MEMORY_TO_IMAGE_COPY,
            .pHostPointer      = data,
            .memoryRowLength   = 0, // tightly packed
            .memoryImageHeight = 0,
            .imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, mipLevel, arrayLayer, 1 },
            .imageOffset       = { 0, 0, 0 },
            .imageExtent       = { mipWidth, mipHeight, mipDepth },
        };
        const VkCopyMemoryToImageInfo copyInfo{
            .sType          = VK_STRUCTURE_TYPE_COPY_MEMORY_TO_IMAGE_INFO,
            .dstImage       = _vkTexture,
            .dstImageLayout = copyLayout,
            .regionCount    = 1,
            .pRegions       = &region,
        };
        // EXT entry point, not the core vkCopyMemoryToImage: that one is only loaded on a
        // Vulkan 1.4+ device, and this runs against 1.3 + VK_EXT_host_image_copy, where the
        // core pointer is null. Same reason vkTransitionImageLayoutEXT is used above.
        const VkResult copyResult = vkCopyMemoryToImageEXT(driverVulkan->getDevice(), &copyInfo);
        if (!VKM_VK_CHECK_RESULT_MSG(copyResult, "Failed to copy host memory into image"))
        {
            return false;
        }

        // Same end state copyBufferToTexture guarantees, so callers need not care which path
        // ran. Usually already satisfied -- the driver prefers SHADER_READ_ONLY_OPTIMAL as
        // the copy layout precisely so this is a no-op.
        if (_currentLayout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            const VkHostImageLayoutTransitionInfo shaderReadTransition{
                .sType            = VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO,
                .image            = _vkTexture,
                .oldLayout        = _currentLayout,
                .newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS },
            };
            const VkResult transitionResult = vkTransitionImageLayoutEXT(driverVulkan->getDevice(), 1, &shaderReadTransition);
            if (!VKM_VK_CHECK_RESULT_MSG(transitionResult, "Failed to transition image to shader-read on the host"))
            {
                return false;
            }
            _currentLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        return true;
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
            .viewType         = toVkImageViewType(_textureInfo._type, _textureInfo._numArrayLayers),
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
        VKM_VK_CHECK_RESULT_MSG(vkSetDebugUtilsObjectNameEXT(driverVulkan->getDevice(), &nameInfo),
            "Failed to set debug name on texture");
#else
        (void)name;
#endif
    }
} // namespace vkm