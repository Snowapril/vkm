// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_swapchain.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>

namespace vkm
{
    VulkanSwapChain::VulkanSwapChain(VkmDriverBase* driver)
        : SwapChain(driver)
    {

    }

    VulkanSwapChain::~VulkanSwapChain()
    {

    }

    bool VulkanSwapChain::createSwapChain(const char* title)
    {
        (void)title;
        // VkmDriverVulkan* vulkanDriver = static_cast<VkmDriverVulkan*>(_driver);
        // VkDevice device = vulkanDriver->getDevice();
        // VkPhysicalDevice physicalDevice = vulkanDriver->getPhysicalDevice();

        return true;
    }

    void VulkanSwapChain::destroySwapChain()
    {

    }

    VkmResourceHandle VulkanSwapChain::acquireNextImageInner()
    {
        return VKM_INVALID_RESOURCE_HANDLE;
    }

    void VulkanSwapChain::presentInner()
    {

    }
} // namespace vkm