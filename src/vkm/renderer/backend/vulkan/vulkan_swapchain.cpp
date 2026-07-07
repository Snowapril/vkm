// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_swapchain.h>
#include <vkm/renderer/backend/vulkan/vulkan_util.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_texture.h>
#include <vkm/renderer/backend/vulkan/vulkan_command_queue.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/common/texture.h>
#include <vector>
#include <GLFW/glfw3.h>

namespace vkm
{
    // We choose the format that is the most common, and that is supported by* the physical device.
    static VkSurfaceFormat2KHR selectSwapSurfaceFormat(const std::vector<VkSurfaceFormat2KHR>& availableFormats)
    {
        // If there's only one available format and it's undefined, return a default format.
        if(availableFormats.size() == 1 && availableFormats[0].surfaceFormat.format == VK_FORMAT_UNDEFINED)
        {
            VkSurfaceFormat2KHR result{.sType         = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR,
                                        .surfaceFormat = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
            return result;
        }

        const std::vector<VkSurfaceFormat2KHR> preferredFormats = {
            VkSurfaceFormat2KHR{.sType         = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR,
                                .surfaceFormat = {VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}},
            VkSurfaceFormat2KHR{.sType         = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR,
                                .surfaceFormat = {VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}}};

        // Check available formats against the preferred formats.
        for(const auto& preferredFormat : preferredFormats)
        {
            for(const auto& availableFormat : availableFormats)
            {
                if(availableFormat.surfaceFormat.format == preferredFormat.surfaceFormat.format
                && availableFormat.surfaceFormat.colorSpace == preferredFormat.surfaceFormat.colorSpace)
                {
                    return availableFormat;  // Return the first matching preferred format.
                }
            }
        }

        // If none of the preferred formats are available, return the first available format.
        return availableFormats[0];
    }

    /*--
    * The present mode is chosen based on the vSync option
    * The FIFO mode is the most common, and is used when vSync is enabled.
    * The MAILBOX mode is used when vSync is disabled, and is the best mode for triple buffering.
    * The IMMEDIATE mode is used when vSync is disabled, and is the best mode for low latency.
    -*/
    static VkPresentModeKHR selectSwapPresentMode(const std::vector<VkPresentModeKHR>& availablePresentModes, bool vSync = true)
    {
        if(vSync)
        {
            return VK_PRESENT_MODE_FIFO_KHR;
        }

        bool mailboxSupported = false, immediateSupported = false;

        for(VkPresentModeKHR mode : availablePresentModes)
        {
            if(mode == VK_PRESENT_MODE_MAILBOX_KHR)
                mailboxSupported = true;
            if(mode == VK_PRESENT_MODE_IMMEDIATE_KHR)
                immediateSupported = true;
        }

        if(mailboxSupported)
        {
            return VK_PRESENT_MODE_MAILBOX_KHR;
        }

        if(immediateSupported)
        {
            return VK_PRESENT_MODE_IMMEDIATE_KHR;  // Best mode for low latency
        }

        return VK_PRESENT_MODE_FIFO_KHR;
    }

    VkmSwapChainVulkan::VkmSwapChainVulkan(VkmDriverBase* driver)
        : VkmSwapChainBase(driver)
    {

    }

    VkmSwapChainVulkan::~VkmSwapChainVulkan()
    {

    }

    void VkmSwapChainVulkan::setDebugName(const char* name)
    {
        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();
        for (const VkmResourceHandle& handle : _backBuffers)
        {
            if (handle.isValid())
            {
                VkmTexture* texture = renderResourcePool->getResource<VkmTexture>(handle);
                texture->setDebugName(name);
            }
        }
    }

    bool VkmSwapChainVulkan::createSwapChain(void* windowHandle)
    {
        // TODO(snowapril) : provide vsync as option
        const bool vSync = false;

        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        GLFWwindow* glfwWindow = reinterpret_cast<GLFWwindow*>(windowHandle);

        VkInstance instance = driverVulkan->getInstance();
        VkPhysicalDevice physicalDevice = driverVulkan->getPhysicalDevice();
        VkDevice device = driverVulkan->getDevice();

        glfwCreateWindowSurface(instance, glfwWindow, nullptr, reinterpret_cast<VkSurfaceKHR*>(&_surface));

        // Query the physical device's capabilities for the given surface.
        const VkPhysicalDeviceSurfaceInfo2KHR surfaceInfo2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR, .surface = _surface};
        VkSurfaceCapabilities2KHR             capabilities2{.sType = VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR};
        vkGetPhysicalDeviceSurfaceCapabilities2KHR(physicalDevice, &surfaceInfo2, &capabilities2);

        uint32_t formatCount;
        vkGetPhysicalDeviceSurfaceFormats2KHR(physicalDevice, &surfaceInfo2, &formatCount, nullptr);
        std::vector<VkSurfaceFormat2KHR> formats(formatCount, {.sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR});
        vkGetPhysicalDeviceSurfaceFormats2KHR(physicalDevice, &surfaceInfo2, &formatCount, formats.data());

        uint32_t presentModeCount;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, _surface, &presentModeCount, nullptr);
        std::vector<VkPresentModeKHR> presentModes(presentModeCount);
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, _surface, &presentModeCount, presentModes.data());

        // Choose the best available surface format and present mode
        const VkSurfaceFormat2KHR surfaceFormat2 = selectSwapSurfaceFormat(formats);
        const VkPresentModeKHR    presentMode    = selectSwapPresentMode(presentModes, vSync);
        // Set the window size according to the surface's current extent
        const VkExtent2D currentExtent = capabilities2.surfaceCapabilities.currentExtent;

        // Adjust the number of images in flight within GPU limitations
        uint32_t minImageCount       = capabilities2.surfaceCapabilities.minImageCount;  // Vulkan-defined minimum
        uint32_t preferredImageCount = std::max(3u, minImageCount);  // Prefer 3, but respect minImageCount

        // Handle the maxImageCount case where 0 means "no upper limit"
        uint32_t maxImageCount = (capabilities2.surfaceCapabilities.maxImageCount == 0) ? preferredImageCount :  // No upper limit, use preferred
                                    capabilities2.surfaceCapabilities.maxImageCount;

        // Store the chosen image format
        _imageFormat = surfaceFormat2.surfaceFormat.format;

        const uint32_t minImageCountClamped = std::clamp(preferredImageCount, minImageCount, maxImageCount);

        // Create the swapchain itself
        const VkSwapchainCreateInfoKHR swapchainCreateInfo{
            .sType            = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
            .surface          = _surface,
            .minImageCount    = minImageCountClamped,
            .imageFormat      = surfaceFormat2.surfaceFormat.format,
            .imageColorSpace  = surfaceFormat2.surfaceFormat.colorSpace,
            .imageExtent      = currentExtent,
            .imageArrayLayers = 1,
            .imageUsage       = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .preTransform     = capabilities2.surfaceCapabilities.currentTransform,
            .compositeAlpha   = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,
            .presentMode      = presentMode,
            .clipped          = VK_TRUE,
        };
        VkResult vkResult = vkCreateSwapchainKHR(device, &swapchainCreateInfo, nullptr, &_swapChain);
        VKM_VK_ASSERT(vkResult, "Failed to create swapchain");

        uint32_t imageCount;
        vkGetSwapchainImagesKHR(device, _swapChain, &imageCount, nullptr);
        VKM_ASSERT(minImageCountClamped == imageCount, "Wrong swapchain setup");
        std::vector<VkImage> swapImages(imageCount);
        vkGetSwapchainImagesKHR(device, _swapChain, &imageCount, swapImages.data());

        _backBufferCount = (uint8_t)imageCount;

        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();
        for (uint32_t i = 0; i < imageCount; ++i)
        {
            VkmTextureVulkan* newTextureVulkan = new VkmTextureVulkan(_driver);

            VkmTextureInfo textureInfo;
            textureInfo._flags = VkmResourceCreateInfo::ExternalHandleOwner | VkmResourceCreateInfo::AllowShaderRead |
                                  VkmResourceCreateInfo::AllowPresent | VkmResourceCreateInfo::AllowColorAttachment;
            textureInfo._extent = glm::uvec3(currentExtent.width, currentExtent.height, 1);
            textureInfo._format = fromVkFormat(_imageFormat);
            textureInfo._numMipLevels = 1;
            textureInfo._numArrayLayers = 1;

            VkmResourceHandle& handle = _backBuffers[i];
            handle = renderResourcePool->allocateTexture(newTextureVulkan);
            if (newTextureVulkan->initialize(handle, textureInfo) == false ||
                newTextureVulkan->overrideExternalHandle(static_cast<void*>(swapImages[i])) == false)
            {
                VKM_DEBUG_ERROR("Failed to create swap chain texture");
                if (handle.isValid())
                    renderResourcePool->releaseResource(handle);
                else
                    delete newTextureVulkan;

                destroySwapChain();
                return false;
            }
        }

        const VkFenceCreateInfo fenceCreateInfo{ .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        vkResult = vkCreateFence(device, &fenceCreateInfo, nullptr, &_acquireFence);
        VKM_VK_ASSERT(vkResult, "Failed to create swapchain acquire fence");

        return true;
    }

    void VkmSwapChainVulkan::destroySwapChain()
    {
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);

        // Release per-image textures (and their VkImageViews) before tearing down the
        // swapchain/surface those images and views were created from.
        destroySwapChainCommon();

        if (_acquireFence != VK_NULL_HANDLE)
        {
            vkDestroyFence(driverVulkan->getDevice(), _acquireFence, nullptr);
            _acquireFence = VK_NULL_HANDLE;
        }
        if (_swapChain != VK_NULL_HANDLE)
        {
            vkDestroySwapchainKHR(driverVulkan->getDevice(), _swapChain, nullptr);
            _swapChain = VK_NULL_HANDLE;
        }
        if (_surface != VK_NULL_HANDLE)
        {
            vkDestroySurfaceKHR(driverVulkan->getInstance(), _surface, nullptr);
            _surface = VK_NULL_HANDLE;
        }
    }

    VkmResourceHandle VkmSwapChainVulkan::acquireNextImageInner()
    {
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        VkDevice device = driverVulkan->getDevice();

        vkResetFences(device, 1, &_acquireFence);

        uint32_t imageIndex = 0;
        VkResult vkResult = vkAcquireNextImageKHR(device, _swapChain, UINT64_MAX, VK_NULL_HANDLE, _acquireFence, &imageIndex);
        if (vkResult != VK_SUCCESS && vkResult != VK_SUBOPTIMAL_KHR)
        {
            VKM_DEBUG_ERROR("Failed to acquire next swapchain image");
            return VKM_INVALID_RESOURCE_HANDLE;
        }

        // No semaphore is signaled by the render command buffer submission today (that would
        // require plumbing a binary semaphore through VkmCommandQueueVulkan::submit(), which is
        // out of scope here). Block on a fence instead so the image is guaranteed available
        // before we hand its handle back for rendering.
        vkWaitForFences(device, 1, &_acquireFence, VK_TRUE, UINT64_MAX);

        _currentBackBufferIndex = imageIndex;
        return _backBuffers[imageIndex];
    }

    void VkmSwapChainVulkan::presentInner()
    {
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        VkmCommandQueueVulkan* presentQueueVulkan = static_cast<VkmCommandQueueVulkan*>(_presentQueue);
        VkQueue vkQueue = presentQueueVulkan->getVkQueue();

        // Same rationale as acquireNextImageInner: without a render-complete semaphore threaded
        // through the submission path, block until all submitted GPU work has finished before
        // presenting, so we never present a partially-rendered image. Rendering may have been
        // submitted to a different queue than the present queue, so wait on the whole device
        // rather than assuming they're the same queue.
        vkDeviceWaitIdle(driverVulkan->getDevice());

        const VkPresentInfoKHR presentInfo{
            .sType          = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
            .swapchainCount = 1,
            .pSwapchains    = &_swapChain,
            .pImageIndices  = &_currentBackBufferIndex,
        };
        VkResult vkResult = vkQueuePresentKHR(vkQueue, &presentInfo);
        VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to present swapchain image");
    }
} // namespace vkm