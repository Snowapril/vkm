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
        /*
        * @brief Whether this image gets VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT.
        * @details An explicit hint wins. Auto forces the bit only for attachments -- an engine
        * policy about how the texture is used, which VMA cannot infer from the image alone.
        * Everything else is left to VMA, which queries VkMemoryDedicatedRequirements per image
        * (the allocator's vulkanApiVersion 1.3 enables that path), honours both
        * requiresDedicatedAllocation and prefersDedicatedAllocation, and weighs them against
        * block occupancy and maxMemoryAllocationCount. Setting the bit here overrides all of it.
        */
        bool shouldUseDedicatedTexture(const VkmTextureInfo& info)
        {
            if (info._placementHint == VkmMemoryPlacementHint::Committed)
            {
                return true;
            }
            if (info._placementHint == VkmMemoryPlacementHint::Heap)
            {
                return false;
            }
            return (info._flags & VkmResourceCreateInfo::AllowColorAttachment) != 0 ||
                   (info._flags & VkmResourceCreateInfo::AllowDepthStencilAttachment) != 0;
        }

        /*
        * @brief Decision policy for CPU-writable texture memory.
        * @details An OPTIMAL-tiled image cannot be memcpy'd into, its layout being swizzled, so
        * host-visible memory alone buys nothing; VK_EXT_host_image_copy is what makes a host-side
        * write correct. Requested only where it also pays off, on a unified-memory device where
        * the GPU reads the same memory the CPU wrote at no extra cost.
        * Restricted to plain upload destinations: render targets and presentables are GPU-written
        * and never CPU-written, so steering them towards host-visible memory would trade
        * attachment bandwidth for a fast path they never take.
        * @param driverVulkan Driver whose capabilities decide availability.
        * @param info Texture being created.
        * @return True when the texture should be placed in CPU-writable memory.
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
        else if (_isAliasable && _vkTexture != VK_NULL_HANDLE)
        {
            // No VmaAllocation of its own: the memory belongs to a heap block shared with other
            // textures, so only the image is destroyed. The block outlives every image in it and
            // is freed at driver teardown.
            vkDestroyImage(driverVulkan->getDevice(), _vkTexture, nullptr);
            _vkTexture = VK_NULL_HANDLE;
            if (VkmAliasedMemoryHeap* heap = driverVulkan->getAliasedMemoryHeap())
            {
                heap->unregisterResource(getHandle());
            }
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

        /*
        * An aliasable texture stops here with an image but no memory. The image has to exist --
        * vkGetImageMemoryRequirements is what tells the packer how many bytes at what alignment
        * to reserve, and out of which memory types -- but who else shares those bytes is not
        * known until VkmRenderGraph::compile() has seen the whole frame. finalizeAliasPlacement()
        * binds and creates the view; until then isAliasPlaced() reports false.
        */
        _isAliasable = (info._flags & VkmResourceCreateInfo::Aliasable) != 0;
        if (_isAliasable)
        {
            const VkResult imageResult =
                vkCreateImage(driverVulkan->getDevice(), &imageCreateInfo, nullptr, &_vkTexture);
            if (!VKM_VK_CHECK_RESULT_MSG(imageResult, "Failed to create an aliasable image"))
            {
                return false;
            }

            VkMemoryRequirements aliasMemReqs{};
            vkGetImageMemoryRequirements(driverVulkan->getDevice(), _vkTexture, &aliasMemReqs);
            _alignment = (uint32_t)aliasMemReqs.alignment;
            // Reported as zero for the same reason a transient texture is: the bytes are counted
            // once as a heap block, so charging each sharer would inflate the report.
            _allocatedSize = 0;
            _uniformLayout = VK_IMAGE_LAYOUT_UNDEFINED;

            VkmAliasedMemoryHeap* heap = driverVulkan->getAliasedMemoryHeap();
            return heap != nullptr && heap->registerResource(handle, aliasMemReqs.size, aliasMemReqs.alignment,
                                                             aliasMemReqs.memoryTypeBits);
        }

        VmaAllocationCreateInfo allocCreateInfo{};
        allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocCreateInfo.flags = shouldUseDedicatedTexture(info) ? VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT : 0;

        // Requested, not granted -- like the host-writable path above. preferredFlags rather
        // than requiredFlags: a device offering no lazily-allocated memory type would make
        // requiredFlags fail vmaCreateImage outright, while an ordinary device-local
        // attachment is still correct, just not lazy. TRANSIENT_ATTACHMENT usage is what puts
        // such a type into the image's memoryTypeBits at all, so this is only ever scored
        // where the driver offers one. shouldUseDedicatedTexture already returns true for
        // every attachment, which matters here: suballocating into a shared VMA block would
        // defeat the lazy commitment.
        const bool requestTransient = (info._flags & VkmResourceCreateInfo::Transient) != 0;
        if (requestTransient)
        {
            allocCreateInfo.preferredFlags = VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
        }

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

        if (requestTransient)
        {
            VkMemoryPropertyFlags memoryProperties = 0;
            vmaGetAllocationMemoryProperties(driverVulkan->getVmaAllocator(), _vmaAllocation, &memoryProperties);
            _isTransient = (memoryProperties & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) != 0;
            if (_isTransient)
            {
                // VmaAllocationInfo::size is the *virtual* size; lazily-allocated memory commits
                // pages on demand and normally commits none, so reporting it would inflate the
                // memory report by the attachment's whole footprint. Metal reports 0 natively.
                _allocatedSize = 0;
            }
        }

        _uniformLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        _subresourceLayouts.clear();
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

        // Host-side layout transition -- no command buffer, no queue, nothing to wait on, which
        // is the whole point of this path. Only the subresource being written: a cubemap arrives
        // one face at a time, and transitioning the whole image per face would move five layers
        // that this call is not touching.
        const VkmSubresourceRange writtenRange{ mipLevel, 1, arrayLayer, 1 };
        if (getSubresourceLayout(mipLevel, arrayLayer) != copyLayout)
        {
            const VkHostImageLayoutTransitionInfo transitionInfo{
                .sType            = VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO,
                .image            = _vkTexture,
                .oldLayout        = getSubresourceLayout(mipLevel, arrayLayer),
                .newLayout        = copyLayout,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mipLevel, 1, arrayLayer, 1 },
            };
            const VkResult transitionResult = vkTransitionImageLayoutEXT(driverVulkan->getDevice(), 1, &transitionInfo);
            if (!VKM_VK_CHECK_RESULT_MSG(transitionResult, "Failed to transition image layout on the host"))
            {
                return false;
            }
            setSubresourceLayout(writtenRange, copyLayout);
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
        if (getSubresourceLayout(mipLevel, arrayLayer) != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        {
            const VkHostImageLayoutTransitionInfo shaderReadTransition{
                .sType            = VK_STRUCTURE_TYPE_HOST_IMAGE_LAYOUT_TRANSITION_INFO,
                .image            = _vkTexture,
                .oldLayout        = getSubresourceLayout(mipLevel, arrayLayer),
                .newLayout        = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, mipLevel, 1, arrayLayer, 1 },
            };
            const VkResult transitionResult = vkTransitionImageLayoutEXT(driverVulkan->getDevice(), 1, &shaderReadTransition);
            if (!VKM_VK_CHECK_RESULT_MSG(transitionResult, "Failed to transition image to shader-read on the host"))
            {
                return false;
            }
            setSubresourceLayout(writtenRange, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
        return true;
    }

    bool VkmTextureVulkan::finalizeAliasPlacement(const VkmAliasPlacement& placement)
    {
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        if (!driverVulkan->bindImageToAliasBlock(placement._blockIndex, _vkTexture, placement._offset))
        {
            return false;
        }

        // Only now: vkCreateImageView requires the image to have memory bound
        // (VUID-VkImageViewCreateInfo-image-01020), which is the whole reason a resource table
        // naming an aliasable texture cannot be built before this point.
        if (!createDefaultView())
        {
            return false;
        }

        _isAliasPlaced = true;
        return true;
    }

    bool VkmTextureVulkan::overrideExternalHandle(void* externalHandle)
    {
        _vkTexture = static_cast<VkImage>(externalHandle);
        _uniformLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        _subresourceLayouts.clear();
        return createDefaultView();
    }

    void VkmTextureVulkan::makeLayoutsPerSubresource()
    {
        if (!_subresourceLayouts.empty())
        {
            return;
        }
        const uint32_t count = std::max(1u, _textureInfo._numMipLevels) *
                               std::max(1u, _textureInfo._numArrayLayers);
        _subresourceLayouts.assign(count, _uniformLayout);
    }

    VkImageLayout VkmTextureVulkan::getSubresourceLayout(uint32_t mipLevel, uint32_t arrayLayer) const
    {
        if (_subresourceLayouts.empty())
        {
            return _uniformLayout;
        }
        const uint32_t index = subresourceIndex(mipLevel, arrayLayer);
        VKM_ASSERT(index < _subresourceLayouts.size(), "Subresource index is out of range");
        return _subresourceLayouts[index];
    }

    VkImageLayout VkmTextureVulkan::getUniformLayout(const VkmSubresourceRange& range) const
    {
        if (_subresourceLayouts.empty())
        {
            return _uniformLayout;
        }

        const uint32_t mipCount = std::max(1u, _textureInfo._numMipLevels);
        const uint32_t layerCount = std::max(1u, _textureInfo._numArrayLayers);
        const uint32_t lastMip = (range._mipLevelCount == VKM_ALL_REMAINING_SUBRESOURCES)
                                     ? mipCount - 1u
                                     : std::min(range._baseMipLevel + range._mipLevelCount - 1u, mipCount - 1u);
        const uint32_t lastLayer =
            (range._arrayLayerCount == VKM_ALL_REMAINING_SUBRESOURCES)
                ? layerCount - 1u
                : std::min(range._baseArrayLayer + range._arrayLayerCount - 1u, layerCount - 1u);

        VkImageLayout shared = VK_IMAGE_LAYOUT_MAX_ENUM;
        for (uint32_t layer = range._baseArrayLayer; layer <= lastLayer; ++layer)
        {
            for (uint32_t mip = range._baseMipLevel; mip <= lastMip; ++mip)
            {
                const VkImageLayout layout = _subresourceLayouts[subresourceIndex(mip, layer)];
                if (shared == VK_IMAGE_LAYOUT_MAX_ENUM)
                {
                    shared = layout;
                }
                else if (shared != layout)
                {
                    return VK_IMAGE_LAYOUT_MAX_ENUM;
                }
            }
        }
        return shared;
    }

    void VkmTextureVulkan::setSubresourceLayout(const VkmSubresourceRange& range, VkImageLayout layout)
    {
        const uint32_t mipCount = std::max(1u, _textureInfo._numMipLevels);
        const uint32_t layerCount = std::max(1u, _textureInfo._numArrayLayers);

        // The whole image: collapse back to the uniform form so the common case keeps costing one
        // VkImageLayout and one whole-image barrier, and the vector is freed rather than kept.
        if (range.coversAll(mipCount, layerCount))
        {
            _uniformLayout = layout;
            _subresourceLayouts.clear();
            _subresourceLayouts.shrink_to_fit();
            return;
        }

        makeLayoutsPerSubresource();
        const uint32_t lastMip = (range._mipLevelCount == VKM_ALL_REMAINING_SUBRESOURCES)
                                     ? mipCount - 1u
                                     : std::min(range._baseMipLevel + range._mipLevelCount - 1u, mipCount - 1u);
        const uint32_t lastLayer =
            (range._arrayLayerCount == VKM_ALL_REMAINING_SUBRESOURCES)
                ? layerCount - 1u
                : std::min(range._baseArrayLayer + range._arrayLayerCount - 1u, layerCount - 1u);

        for (uint32_t layer = range._baseArrayLayer; layer <= lastLayer; ++layer)
        {
            for (uint32_t mip = range._baseMipLevel; mip <= lastMip; ++mip)
            {
                _subresourceLayouts[subresourceIndex(mip, layer)] = layout;
            }
        }

        // A partial write that happens to leave every subresource agreeing collapses too -- the
        // last face of a cubemap upload is exactly that, and it must not leave the texture stuck
        // in the per-subresource form for the rest of its life.
        const VkImageLayout first = _subresourceLayouts[0];
        if (std::all_of(_subresourceLayouts.begin(), _subresourceLayouts.end(),
                        [first](VkImageLayout candidate) { return candidate == first; }))
        {
            _uniformLayout = first;
            _subresourceLayouts.clear();
            _subresourceLayouts.shrink_to_fit();
        }
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
        // An externally-owned or aliasable texture has no image yet at the point newTexture()
        // names it -- the swapchain binds one later, and an aliasable one gets its memory at
        // graph-compile time. Naming VK_NULL_HANDLE is a validation error
        // (VUID-vkSetDebugUtilsObjectNameEXT-pNameInfo-02588), so skip until there is something
        // to name.
        if (_vkTexture == VK_NULL_HANDLE)
        {
            return;
        }

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