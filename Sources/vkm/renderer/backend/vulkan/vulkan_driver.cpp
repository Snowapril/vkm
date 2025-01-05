

// Copyright (c) 2024 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_util.h>
#include <vkm/renderer/engine.h>

#include <volk.h>

#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#define VMA_IMPLEMENTATION
#define VMA_LEAK_LOG_FORMAT(format, ...)                                                                               \
  {                                                                                                                    \
    printf((format), __VA_ARGS__);                                                                                     \
    printf("\n");                                                                                                      \
  }
// Disable warnings in VMA
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnullability-completeness"
#pragma clang diagnostic ignored "-Wmissing-field-initializers"
#pragma clang diagnostic ignored "-Wunused-variable"
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#pragma warning(push)
#pragma warning(disable : 4100)  // Unreferenced formal parameter
#pragma warning(disable : 4189)  // Local variable is initialized but not referenced
#pragma warning(disable : 4127)  // Conditional expression is constant
#pragma warning(disable : 4324)  // Structure was padded due to alignment specifier
#pragma warning(disable : 4505)  // Unreferenced function with internal linkage has been removed
#include "vk_mem_alloc.h"
#pragma warning(pop)
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#include <vulkan/vk_enum_string_helper.h>

namespace vkm
{
    VkmDriverVulkan::VkmDriverVulkan()
        : VkmDriverBase()
    {
        
    }

    VkmDriverVulkan::~VkmDriverVulkan()
    {

    }

    bool VkmDriverVulkan::initializeInner(const VkmEngineLaunchOptions* options)
    {
        (void)options;
        VKM_VK_CHECK_RESULT_MSG_RETURN(volkInitialize(), "Failed to initialize volk");

        uint32_t instanceExtensionCount;
        VKM_VK_CHECK_RESULT_MSG_RETURN(vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, nullptr), "Failed to get instance extension count");

        std::vector<VkExtensionProperties> instanceExtensions(instanceExtensionCount);
        VKM_VK_CHECK_RESULT_MSG_RETURN(vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, instanceExtensions.data()), "Failed to get instance extension count");

        return true;
    }

    void VkmDriverVulkan::destroyInner()
    {

    }
}