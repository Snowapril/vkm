// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_swapchain.h>
#include <vkm/renderer/backend/vulkan/vulkan_util.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
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
        : VkmSwapChain(driver)
    {

    }

    VkmSwapChainVulkan::~VkmSwapChainVulkan()
    {

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
            .imageExtent      = capabilities2.surfaceCapabilities.currentExtent,
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

        // TODO(snowapril) : create VkmTexture with swapImages and create image view for each image

        return true;
    }

    void VkmSwapChainVulkan::destroySwapChain()
    {

    }

    VkmResourceHandle VkmSwapChainVulkan::acquireNextImageInner()
    {
        return VKM_INVALID_RESOURCE_HANDLE;
    }

    void VkmSwapChainVulkan::presentInner()
    {

    }
} // namespace vkm