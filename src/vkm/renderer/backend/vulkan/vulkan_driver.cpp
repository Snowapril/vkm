

// Copyright (c) 2025 Snowapril

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
#pragma clang diagnostic ignored "-Wunused-private-field"
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4100)  // Unreferenced formal parameter
#pragma warning(disable : 4189)  // Local variable is initialized but not referenced
#pragma warning(disable : 4127)  // Conditional expression is constant
#pragma warning(disable : 4324)  // Structure was padded due to alignment specifier
#pragma warning(disable : 4505)  // Unreferenced function with internal linkage has been removed
#endif
#include <vk_mem_alloc.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#ifdef __clang__
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// vk_mem_alloc.h pulls in vulkan.h, which on Linux/X11 pulls in X11/Xlib.h; Xlib.h
// #defines Success as a plain integer macro, colliding with VkmInitResultCode::Success below.
#ifdef Success
#undef Success
#endif

#include <GLFW/glfw3.h>

// On Linux, GLFW pulls in <vulkan/vulkan.h>'s VK_USE_PLATFORM_XLIB_KHR path, which
// includes <X11/Xlib.h>. Xlib.h #defines Success as a bare 0, clobbering every later
// use of VkmInitResultCode::Success as a plain-text substitution (a syntax error).
#ifdef Success
#undef Success
#endif

#include <vkm/renderer/backend/vulkan/vulkan_swapchain.h>
#include <vkm/renderer/backend/vulkan/vulkan_texture.h>
#include <vkm/renderer/backend/vulkan/vulkan_command_queue.h>
#include <vkm/renderer/backend/vulkan/vulkan_pipeline_state.h>

static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                                    VkDebugUtilsMessageTypeFlagsEXT,
                                                    const VkDebugUtilsMessengerCallbackDataEXT* callbackData,
                                                    void*)
{
    using namespace vkm;
    if ( (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0 )
    {
        VKM_DEBUG_ERROR(fmt::format("{}", callbackData->pMessage).c_str());
    }
    else if ( (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) != 0 )
    {
        VKM_DEBUG_WARN(fmt::format("{}", callbackData->pMessage).c_str());
    }
    else
    {
        VKM_DEBUG_INFO(fmt::format("{}", callbackData->pMessage).c_str());
    }
    
    if((severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) != 0)
    {
  #if defined(_MSVC_LANG)
        __debugbreak();
  #elif defined(LINUX)
        raise(SIGTRAP);
  #endif
    }
    return VK_FALSE;
}

template <typename MainT, typename NewT>
static void pNextChainPushFront(MainT* mainStruct, NewT* newStruct)
{
  newStruct->pNext  = mainStruct->pNext;
  mainStruct->pNext = newStruct;
}

namespace vkm
{
    struct ValidationSettings
    {
        VkBool32 fine_grained_locking{VK_TRUE};
        VkBool32 validate_core{VK_TRUE};
        VkBool32 check_image_layout{VK_TRUE};
        VkBool32 check_command_buffer{VK_TRUE};
        VkBool32 check_object_in_use{VK_TRUE};
        VkBool32 check_query{VK_TRUE};
        VkBool32 check_shaders{VK_TRUE};
        VkBool32 check_shaders_caching{VK_TRUE};
        VkBool32 unique_handles{VK_TRUE};
        VkBool32 object_lifetime{VK_TRUE};
        VkBool32 stateless_param{VK_TRUE};
        std::vector<const char*> debug_action{"VK_DBG_LAYER_ACTION_LOG_MSG"};  // "VK_DBG_LAYER_ACTION_DEBUG_OUTPUT", "VK_DBG_LAYER_ACTION_BREAK"
        std::vector<const char*> report_flags{"error"};

        VkBaseInStructure* buildPNextChain()
        {
            layerSettings = std::vector<VkLayerSettingEXT>{
                {layerName, "fine_grained_locking", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &fine_grained_locking},
                {layerName, "validate_core", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &validate_core},
                {layerName, "check_image_layout", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &check_image_layout},
                {layerName, "check_command_buffer", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &check_command_buffer},
                {layerName, "check_object_in_use", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &check_object_in_use},
                {layerName, "check_query", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &check_query},
                {layerName, "check_shaders", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &check_shaders},
                {layerName, "check_shaders_caching", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &check_shaders_caching},
                {layerName, "unique_handles", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &unique_handles},
                {layerName, "object_lifetime", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &object_lifetime},
                {layerName, "stateless_param", VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &stateless_param},
                {layerName, "debug_action", VK_LAYER_SETTING_TYPE_STRING_EXT, uint32_t(debug_action.size()), debug_action.data()},
                {layerName, "report_flags", VK_LAYER_SETTING_TYPE_STRING_EXT, uint32_t(report_flags.size()), report_flags.data()},
            
            };
            layerSettingsCreateInfo = {
                .sType        = VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,
                .settingCount = uint32_t(layerSettings.size()),
                .pSettings    = layerSettings.data(),
            };
            
            return reinterpret_cast<VkBaseInStructure*>(&layerSettingsCreateInfo);
        }

        static constexpr const char*   layerName{"VK_LAYER_KHRONOS_validation"};
        std::vector<VkLayerSettingEXT> layerSettings{};
        VkLayerSettingsCreateInfoEXT   layerSettingsCreateInfo{};
    };

    VkmDriverVulkan::VkmDriverVulkan()
        : VkmDriverBase()
    {
        
    }

    VkmDriverVulkan::~VkmDriverVulkan()
    {

    }

    VkmTexture* VkmDriverVulkan::newTextureInner()
    {
        // TODO(snowapril) : create texture via resource pool backend
        return new VkmTextureVulkan(this);
    }

    VkmPipelineStateBase* VkmDriverVulkan::newPipelineStateInner()
    {
        return new VkmPipelineStateVulkan(this);
    }

    VkmSwapChainBase* VkmDriverVulkan::newSwapChainInner()
    {
        return new VkmSwapChainVulkan(this);
    }

    uint32_t VkmDriverVulkan::getQueueFamilyIndex(VkmCommandQueueType queueType) const
    {
        switch (queueType)
        {
            case VkmCommandQueueType::Graphics: return _graphicsQueueFamilyIndex;
            case VkmCommandQueueType::Compute:  return _computeQueueFamilyIndex;
            case VkmCommandQueueType::Transfer: return _transferQueueFamilyIndex;
            default: VKM_ASSERT(false, "Invalid command queue type"); return UINT32_MAX;
        }
    }

    VkmCommandQueueBase* VkmDriverVulkan::newCommandQueueInner()
    {
        return new VkmCommandQueueVulkan(this);
    }

    static bool isExtensionSupported(const char* extensionName, const std::vector<VkExtensionProperties>& availableExtensions)
    {
        for (const auto& extension : availableExtensions)
        {
            if (strcmp(extension.extensionName, extensionName) == 0)
            {
                return true;
            }
        }

        return false;
    }

    VkmInitResult VkmDriverVulkan::initializeInner(const VkmEngineLaunchOptions* options)
    {
        if (volkInitialize() != VK_SUCCESS)
        {
            return VkmInitResult{VkmInitResultCode::HardwareUnsupported, "Failed to initialize the Vulkan loader (no Vulkan runtime installed on this system)."};
        }

        uint32_t instanceExtensionCount = 0;
        VKM_VK_CHECK_RESULT_MSG_RETURN(vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, nullptr), "Failed to get instance extension count");

        std::vector<VkExtensionProperties> availableInstanceExtensions(instanceExtensionCount);
        VKM_VK_CHECK_RESULT_MSG_RETURN(vkEnumerateInstanceExtensionProperties(nullptr, &instanceExtensionCount, availableInstanceExtensions.data()), "Failed to get instance extension count");

        uint32_t glfwExtensionCount = 0;
        const char** glfwExtensions = glfwGetRequiredInstanceExtensions(&glfwExtensionCount);

        std::vector<const char*> instanceExtensions { VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME };
        instanceExtensions.insert(instanceExtensions.end(), glfwExtensions, glfwExtensions + glfwExtensionCount);
        if (isExtensionSupported(VK_EXT_DEBUG_UTILS_EXTENSION_NAME, availableInstanceExtensions))
        {
            instanceExtensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        }
        if (isExtensionSupported(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME, availableInstanceExtensions))
        {
            instanceExtensions.push_back(VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME);
        }

        // Portability-only ICDs (e.g. MoltenVK) are excluded from enumeration by default on
        // newer loaders; without this, vkCreateInstance fails with VK_ERROR_INCOMPATIBLE_DRIVER.
        VkInstanceCreateFlags instanceCreateFlags = 0;
        if (isExtensionSupported(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME, availableInstanceExtensions))
        {
            instanceExtensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
            instanceCreateFlags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        }

        std::vector<const char*> instanceLayers;
        if (options->enableValidationLayer)
        {
            instanceLayers.push_back("VK_LAYER_KHRONOS_validation");
        }

        ValidationSettings validationSettings{.validate_core = VK_TRUE};

        // TODO : get application name from shared application class
        const VkApplicationInfo applicationInfo{
            .pApplicationName   = "Vkm",
            .applicationVersion = 1,
            .pEngineName        = "Vkm",
            .engineVersion      = 1,
            .apiVersion         = VK_API_VERSION_1_3,
        };
        
        const VkInstanceCreateInfo instanceCreateInfo{
            .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
            .pNext                   = validationSettings.buildPNextChain(),
            .flags                   = instanceCreateFlags,
            .pApplicationInfo        = &applicationInfo,
            .enabledLayerCount       = uint32_t(instanceLayers.size()),
            .ppEnabledLayerNames     = instanceLayers.data(),
            .enabledExtensionCount   = uint32_t(instanceExtensions.size()),
            .ppEnabledExtensionNames = instanceExtensions.data(),
        };

        const VkResult createInstanceResult = vkCreateInstance(&instanceCreateInfo, nullptr, &_instance);
        if (createInstanceResult == VK_ERROR_INCOMPATIBLE_DRIVER)
        {
            // No Vulkan-capable ICD is registered on this system at all (e.g. a CI runner
            // with no GPU and no software rasterizer) -- same underlying condition as the
            // "zero physical devices" check below, just surfacing one call earlier.
            VKM_DEBUG_ERROR("No compatible Vulkan driver/ICD found");
            return VkmInitResult{VkmInitResultCode::HardwareUnsupported, "No compatible Vulkan driver/ICD found on this system."};
        }
        VKM_VK_CHECK_RESULT_MSG_RETURN(createInstanceResult, "Failed to create instance");

        VKM_DEBUG_INFO("Vulkan instance created");
        VKM_DEBUG_INFO("Instance extension used : ");
        for (const auto& extension : instanceExtensions)
        {
            VKM_DEBUG_INFO(fmt::format("\t{}", extension).c_str());
        }
        VKM_DEBUG_INFO("Instance layers used : ");
        for (const auto& layer : instanceLayers)
        {
            VKM_DEBUG_INFO(fmt::format("\t{}", layer).c_str());
        }

        volkLoadInstance(_instance);
        if (options->enableValidationLayer)
        {
            const VkDebugUtilsMessengerCreateInfoEXT dbgMessengerCreateInfo{
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                .messageType     = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
                .pfnUserCallback = debugCallback
            };
            VKM_VK_CHECK_RESULT_MSG_RETURN(vkCreateDebugUtilsMessengerEXT(_instance, &dbgMessengerCreateInfo, nullptr, &_callback), "Failed to create debug messenger");
            VKM_DEBUG_INFO("Vulkan validation layer enabled");
        }

        size_t chosenDevice = 0;

        uint32_t deviceCount = 0;
        vkEnumeratePhysicalDevices(_instance, &deviceCount, nullptr);
        if (deviceCount == 0)
        {
            VKM_DEBUG_ERROR("No Vulkan GPU found");
            return VkmInitResult{VkmInitResultCode::HardwareUnsupported, "No Vulkan-compatible GPU found on this system."};
        }
        
        std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
        vkEnumeratePhysicalDevices(_instance, &deviceCount, physicalDevices.data());

        VkPhysicalDeviceProperties2 properties2{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        for(size_t i = 0; i < physicalDevices.size(); i++)
        {
            vkGetPhysicalDeviceProperties2(physicalDevices[i], &properties2);
            if(properties2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            {
                chosenDevice = i;
                break;
            }
        }

        _physicalDevice = physicalDevices[chosenDevice];
        vkGetPhysicalDeviceProperties2(_physicalDevice, &properties2);

        VKM_DEBUG_INFO(fmt::format("Selected GPU: {}", properties2.properties.deviceName).c_str());
        VKM_DEBUG_INFO(fmt::format("Driver: {}.{}.{}", VK_VERSION_MAJOR(properties2.properties.driverVersion),
            VK_VERSION_MINOR(properties2.properties.driverVersion), VK_VERSION_PATCH(properties2.properties.driverVersion)).c_str());
        VKM_DEBUG_INFO(fmt::format("Vulkan API: {}.{}.{}", VK_VERSION_MAJOR(properties2.properties.apiVersion),
            VK_VERSION_MINOR(properties2.properties.apiVersion), VK_VERSION_PATCH(properties2.properties.apiVersion)).c_str());

        // Chaining all features up to Vulkan 1.3
        pNextChainPushFront(&_features11, &_features12);
        pNextChainPushFront(&_features11, &_features13);

        /*-- 
        * Check if the device supports the required extensions 
        * Because we cannot request a device with extension it is not supporting
        -*/
        uint32_t deviceExtensionCount = 0;
        VKM_VK_CHECK_RESULT_MSG_RETURN(vkEnumerateDeviceExtensionProperties(_physicalDevice, nullptr, &deviceExtensionCount, nullptr), "Failed to get device extension count");

        std::vector<VkExtensionProperties> availableDeviceExtensions(deviceExtensionCount);
        VKM_VK_CHECK_RESULT_MSG_RETURN(vkEnumerateDeviceExtensionProperties(_physicalDevice, nullptr, &deviceExtensionCount, availableDeviceExtensions.data()), "Failed to get device extension count");

        std::vector<const char*> deviceExtensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME,  /* Needed for display on the screen */ };

        if(isExtensionSupported(VK_KHR_MAINTENANCE_5_EXTENSION_NAME, availableDeviceExtensions))
        {
            pNextChainPushFront(&_features11, &_maintenance5Features);
            deviceExtensions.push_back(VK_KHR_MAINTENANCE_5_EXTENSION_NAME);
        }
        if(isExtensionSupported(VK_KHR_MAINTENANCE_6_EXTENSION_NAME, availableDeviceExtensions))
        {
            pNextChainPushFront(&_features11, &_maintenance6Features);
            deviceExtensions.push_back(VK_KHR_MAINTENANCE_6_EXTENSION_NAME);
        }
        if(isExtensionSupported(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, availableDeviceExtensions))
        {
            deviceExtensions.push_back(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
        }
        if(isExtensionSupported(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME, availableDeviceExtensions))
        {
            pNextChainPushFront(&_features11, &_dynamicStateFeatures);
            deviceExtensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_EXTENSION_NAME);
        }
        if(isExtensionSupported(VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME, availableDeviceExtensions))
        {
            pNextChainPushFront(&_features11, &_dynamicState2Features);
            deviceExtensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_2_EXTENSION_NAME);
        }
        if(isExtensionSupported(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, availableDeviceExtensions))
        {
            pNextChainPushFront(&_features11, &_dynamicState3Features);
            deviceExtensions.push_back(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
        }
        if(isExtensionSupported(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME, availableDeviceExtensions))
        {
            pNextChainPushFront(&_features11, &_swapchainFeatures);
            deviceExtensions.push_back(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME);
        }

        // Requesting all supported features, which will then be activated in the device
        // By requesting, it turns on all feature that it is supported, but the user could request specific features instead
        _deviceFeatures.pNext = &_features11;
        vkGetPhysicalDeviceFeatures2(_physicalDevice, &_deviceFeatures);

        // maintenance5/6 are requested and activated opportunistically above when the driver
        // supports them, but are not required: MoltenVK does not expose either extension yet.
        if ( _features13.dynamicRendering == false || _features13.maintenance4 == false )
        {
            VKM_DEBUG_ERROR("Required Vulkan 1.3 features are not supported");
            return VkmInitResult{VkmInitResultCode::HardwareUnsupported, "This GPU/driver does not support required Vulkan 1.3 features (dynamicRendering, maintenance4)."};
        }

        // Query queue families
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(_physicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilyProperties(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(_physicalDevice, &queueFamilyCount, queueFamilyProperties.data());

        _graphicsQueueFamilyIndex = UINT32_MAX;
        [[maybe_unused]] uint32_t presentQueueFamilyIndex = UINT32_MAX;
        _computeQueueFamilyIndex = UINT32_MAX;
        bool dedicatedComputeQueueFound = false;
        for (uint32_t i = 0; i < queueFamilyCount; i++)
        {
            // VkBool32 presentSupport = false;
            // vkGetPhysicalDeviceSurfaceSupportKHR(_physicalDevice, i, nullptr, &presentSupport);
            if (queueFamilyProperties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            {
                _graphicsQueueFamilyIndex = i;
            }
            // if (presentSupport)
            // {
            //     presentQueueFamilyIndex = i;
            // }

            if (queueFamilyProperties[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
            {
                if ( dedicatedComputeQueueFound == false )
                {
                    _computeQueueFamilyIndex = i;
                    if ( ( queueFamilyProperties[i].queueFlags & VK_QUEUE_GRAPHICS_BIT ) == 0 )
                    {
                        dedicatedComputeQueueFound = true;
                    }
                }
            }
        }

        // TODO : Create queue for each purpose. One queue for each graphics/present/compute queue. Each queue can be same or different.
        // For transfer queue, prepare all we have.
        std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
        std::vector<float> queuePriorities { 1.0f };

        queueCreateInfos.push_back( VkDeviceQueueCreateInfo {
            .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
            .queueFamilyIndex = _graphicsQueueFamilyIndex,
            .queueCount       = 1,
            .pQueuePriorities = queuePriorities.data(),
        } );

        // Get information about what the device can do
        VkPhysicalDeviceProperties2 deviceProperties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        deviceProperties.pNext = &_pushDescriptorProperties;
        vkGetPhysicalDeviceProperties2(_physicalDevice, &deviceProperties);

        // Create the logical device
        const VkDeviceCreateInfo deviceCreateInfo{
            .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
            .pNext                   = &_deviceFeatures,
            .queueCreateInfoCount    = (uint32_t)queueCreateInfos.size(),
            .pQueueCreateInfos       = queueCreateInfos.data(),
            .enabledExtensionCount   = uint32_t(deviceExtensions.size()),
            .ppEnabledExtensionNames = deviceExtensions.data(),
        };
        
        VKM_VK_CHECK_RESULT_MSG_RETURN(vkCreateDevice(_physicalDevice, &deviceCreateInfo, nullptr, &_device), "Failed to create logical device");
        VKM_DEBUG_INFO("Logical Device created");
        VKM_DEBUG_INFO("Device extension used : ");
        for (const auto& extension : deviceExtensions)
        {
            VKM_DEBUG_INFO(fmt::format("\t{}", extension).c_str());
        }

        volkLoadDevice(_device);

        _driverCapabilityFlags = VkmDriverCapabilityFlags::CommandBufferReusable;

        return VkmInitResult{VkmInitResultCode::Success, ""};
    }

    void VkmDriverVulkan::destroyInner()
    {

    }
}