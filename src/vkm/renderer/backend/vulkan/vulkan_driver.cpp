

// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_resource_table.h>
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
#include <vkm/renderer/backend/vulkan/vulkan_buffer.h>
#include <vkm/renderer/backend/vulkan/vulkan_staging_buffer.h>
#include <vkm/renderer/backend/vulkan/vulkan_sampler.h>
#include <vkm/renderer/backend/vulkan/vulkan_acceleration_structure.h>
#include <vkm/renderer/backend/vulkan/vulkan_texture_view.h>
#include <vkm/renderer/backend/vulkan/vulkan_buffer_view.h>
#include <vkm/renderer/backend/vulkan/vulkan_gpu_buffer_pool.h>
#include <vkm/renderer/backend/vulkan/vulkan_command_queue.h>
#include <vkm/renderer/backend/vulkan/vulkan_bindless_resource_manager.h>
#include <vkm/renderer/backend/vulkan/vulkan_command_buffer.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>

// X11/Xlib.h (see above) also #defines None and Always as bare integers, clobbering
// VkmCullMode::None and VkmCompareOp::Always in pipeline_state.h, transitively included
// below via vulkan_pipeline_state.h.
#ifdef None
#undef None
#endif
#ifdef Always
#undef Always
#endif

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
        return new VkmTextureVulkan(this);
    }

    VkmBuffer* VkmDriverVulkan::newBufferInner()
    {
        return new VkmBufferVulkan(this);
    }

    VkmStagingBuffer* VkmDriverVulkan::newStagingBufferInner()
    {
        return new VkmStagingBufferVulkan(this);
    }

    VkmSampler* VkmDriverVulkan::newSamplerInner()
    {
        return new VkmSamplerVulkan(this);
    }

    VkmTextureView* VkmDriverVulkan::newTextureViewInner()
    {
        return new VkmTextureViewVulkan(this);
    }

    VkmBufferView* VkmDriverVulkan::newBufferViewInner()
    {
        return new VkmBufferViewVulkan(this);
    }

    VkmRenderResourcePool* VkmDriverVulkan::newRenderResourcePoolInner()
    {
        return new VkmRenderResourcePool(this);
    }

    VkmPipelineStateBase* VkmDriverVulkan::newPipelineStateInner()
    {
        return new VkmPipelineStateVulkan(this);
    }

    VkmFormat VkmDriverVulkan::selectSwapChainColorFormat(bool enableHdr) const
    {
        // TODO(hdr): Vulkan HDR needs an HDR color space (VK_EXT_swapchain_colorspace) and
        // surface-format negotiation; not implemented yet. Always use the non-HDR format, which
        // matches selectSwapSurfaceFormat()'s BGRA8_UNORM/SRGB_NONLINEAR preference.
        (void)enableHdr;
        return VkmFormat::BGRA8_UNORM;
    }

    VkmAccelerationStructure* VkmDriverVulkan::newAccelerationStructureInner()
    {
        return new VkmAccelerationStructureVulkan(this);
    }

    void VkmDriverVulkan::waitIdle(const uint64_t timeoutMs)
    {
        VkmDriverBase::waitIdle(timeoutMs);
        if (_device != VK_NULL_HANDLE)
        {
            vkDeviceWaitIdle(_device);
        }
    }

    VkmResourceTableBase* VkmDriverVulkan::newResourceTableInner()
    {
        return new VkmResourceTableVulkan(this);
    }

    VkmSwapChainBase* VkmDriverVulkan::newSwapChainInner()
    {
        return new VkmSwapChainVulkan(this);
    }

    VkmGpuMemoryStats VkmDriverVulkan::getGpuMemoryStats() const
    {
        VkmGpuMemoryStats stats{};
        if (_vmaAllocator == VK_NULL_HANDLE)
        {
            return stats;
        }

        // Budgets are the cheap query (no allocation walk) and are what the driver itself
        // reports for the heaps VMA allocates from -- i.e. the "actual" side.
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(_physicalDevice, &memoryProperties);

        std::vector<VmaBudget> budgets(memoryProperties.memoryHeapCount);
        vmaGetHeapBudgets(_vmaAllocator, budgets.data());
        for (uint32_t heapIndex = 0; heapIndex < memoryProperties.memoryHeapCount; ++heapIndex)
        {
            // Only device-local heaps are "VRAM" in the sense the UI reports; host-visible
            // system-memory heaps would otherwise double-count against process RSS.
            if ((memoryProperties.memoryHeaps[heapIndex].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) == 0)
            {
                continue;
            }
            stats._deviceAllocatedBytes += budgets[heapIndex].usage;
            stats._deviceBudgetBytes += budgets[heapIndex].budget;
        }
        stats._hasDeviceStats = true;

        // blockBytes = what VMA reserved in VkDeviceMemory blocks, allocationBytes = what it
        // handed out of them; the difference is suballocator slack.
        VmaTotalStatistics totalStatistics{};
        vmaCalculateStatistics(_vmaAllocator, &totalStatistics);
        stats._poolReservedBytes = totalStatistics.total.statistics.blockBytes;
        stats._poolUsedBytes = totalStatistics.total.statistics.allocationBytes;
        stats._hasPoolStats = true;

        return stats;
    }

    bool VkmDriverVulkan::initializeGpuTimestampPool(const uint32_t slotCount)
    {
        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(_physicalDevice, &properties);
        if (properties.limits.timestampComputeAndGraphics == VK_FALSE)
        {
            VKM_DEBUG_INFO("GPU timestamp queries are not supported on this device; GPU profiling is disabled");
            return false;
        }
        _timestampPeriodNs = static_cast<double>(properties.limits.timestampPeriod);

        // Only the low timestampValidBits of a written timestamp carry data; the remaining high
        // bits are undefined and would turn a delta into nonsense.
        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(_physicalDevice, &queueFamilyCount, nullptr);
        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(_physicalDevice, &queueFamilyCount, queueFamilies.data());
        if (_graphicsQueueFamilyIndex < queueFamilyCount)
        {
            const uint32_t validBits = queueFamilies[_graphicsQueueFamilyIndex].timestampValidBits;
            if (validBits == 0)
            {
                VKM_DEBUG_INFO("Graphics queue family reports no valid timestamp bits; GPU profiling is disabled");
                return false;
            }
            _timestampValidMask = (validBits >= 64) ? UINT64_MAX : ((uint64_t{1} << validBits) - 1);
        }

        const VkQueryPoolCreateInfo queryPoolCreateInfo{
            .sType      = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .queryType  = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = slotCount,
        };
        const VkResult vkResult =
            vkCreateQueryPool(_device, &queryPoolCreateInfo, nullptr, &_timestampQueryPool);
        if (!VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to create GPU profiler timestamp query pool"))
        {
            return false;
        }

        _driverCapabilityFlags = _driverCapabilityFlags | VkmDriverCapabilityFlags::TimestampQuery;
        return true;
    }

    void VkmDriverVulkan::destroyGpuTimestampPool()
    {
        if (_timestampQueryPool != VK_NULL_HANDLE)
        {
            vkDestroyQueryPool(_device, _timestampQueryPool, nullptr);
            _timestampQueryPool = VK_NULL_HANDLE;
        }
    }

    void VkmDriverVulkan::resetGpuTimestampSlots(VkmCommandBufferBase* commandBuffer, const uint32_t firstSlot,
                                                 const uint32_t count)
    {
        if (_timestampQueryPool == VK_NULL_HANDLE || count == 0)
        {
            return;
        }
        vkCmdResetQueryPool(static_cast<VkmCommandBufferVulkan*>(commandBuffer)->getVkCommandBuffer(),
                            _timestampQueryPool, firstSlot, count);
    }

    bool VkmDriverVulkan::resolveGpuTimestamps(const uint32_t firstSlot, const uint32_t count, uint64_t* outTicks)
    {
        if (_timestampQueryPool == VK_NULL_HANDLE || count == 0 || outTicks == nullptr)
        {
            return false;
        }

        // Callers only ask once the writing submission's timeline has completed, so every query
        // in the range is available and this cannot stall.
        const VkResult vkResult = vkGetQueryPoolResults(_device, _timestampQueryPool, firstSlot, count,
                                                        sizeof(uint64_t) * count, outTicks, sizeof(uint64_t),
                                                        VK_QUERY_RESULT_64_BIT);
        if (vkResult != VK_SUCCESS)
        {
            return false;
        }

        for (uint32_t index = 0; index < count; ++index)
        {
            outTicks[index] &= _timestampValidMask;
        }
        return true;
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
        // Registers this driver so vulkan_util.cpp's vkCheckResult() can route a detected
        // VK_ERROR_DEVICE_LOST (from this call or any later VkResult-returning call in this
        // backend) to the shared VkmGpuCrashHandler.
        setActiveVulkanDriver(this);

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
            // No Vulkan-capable ICD is registered on this system at all -- the same underlying
            // condition as the "zero physical devices" check below, one call earlier.
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
        // Lets the CPU write an OPTIMAL-tiled image directly (vkCopyMemoryToImage), which a plain
        // memcpy cannot do because the layout is swizzled. Without it, host-visible texture memory
        // is useless and every upload goes through a staging buffer -- see
        // shouldUseHostWritableTexture in vulkan_texture.cpp.
        //
        // Not requested on MoltenVK: it advertises the extension but emulates the feature on top of
        // Metal, and enabling it hangs initialization indefinitely. Nothing is lost, since macOS
        // has the engine's own Metal backend for this and Vulkan-on-Metal falls back to staging.
        VkPhysicalDeviceDriverProperties driverProperties{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
        };
        VkPhysicalDeviceProperties2 driverProps2{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &driverProperties,
        };
        vkGetPhysicalDeviceProperties2(_physicalDevice, &driverProps2);

        const bool requestHostImageCopy =
            isExtensionSupported(VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME, availableDeviceExtensions) &&
            driverProperties.driverID != VK_DRIVER_ID_MOLTENVK;
        if(requestHostImageCopy)
        {
            pNextChainPushFront(&_features11, &_hostImageCopyFeatures);
            deviceExtensions.push_back(VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME);
        }
        // Opportunistic: only requested when the crash handler's breadcrumb recording was
        // opted into, and only enabled if the GPU/driver actually supports it (not every
        // vendor implements VK_EXT_device_fault). See isDeviceFaultExtensionEnabled().
        const bool requestDeviceFaultExtension = options != nullptr && options->enableGpuCrashDump
            && isExtensionSupported(VK_EXT_DEVICE_FAULT_EXTENSION_NAME, availableDeviceExtensions);
        if(requestDeviceFaultExtension)
        {
            pNextChainPushFront(&_features11, &_deviceFaultFeatures);
            deviceExtensions.push_back(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
        }
        /*
        * Ray query. Requested as a set of three because they only mean anything together:
        * VK_KHR_acceleration_structure depends on VK_KHR_deferred_host_operations, and
        * VK_KHR_ray_query needs a structure to traverse, so a partial set would leave the
        * capability flag ambiguous. VK_KHR_ray_tracing_pipeline is absent because the engine casts
        * rays from compute shaders and needs neither RT pipelines nor shader binding tables.
        * Opportunistic: MoltenVK exposes none of these and lavapipe only does from Mesa 24.1, so a
        * device without them reports no RayTracing capability rather than failing.
        */
        const bool requestRayTracing =
            isExtensionSupported(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, availableDeviceExtensions) &&
            isExtensionSupported(VK_KHR_RAY_QUERY_EXTENSION_NAME, availableDeviceExtensions) &&
            isExtensionSupported(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME, availableDeviceExtensions);
        if (requestRayTracing)
        {
            pNextChainPushFront(&_features11, &_accelerationStructureFeatures);
            pNextChainPushFront(&_features11, &_rayQueryFeatures);
            deviceExtensions.push_back(VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME);
            deviceExtensions.push_back(VK_KHR_RAY_QUERY_EXTENSION_NAME);
            deviceExtensions.push_back(VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
        }

        // Required by the Vulkan spec (VUID-VkDeviceCreateInfo-pProperties-04451) whenever the
        // physical device exposes it, e.g. MoltenVK on macOS.
        if(isExtensionSupported("VK_KHR_portability_subset", availableDeviceExtensions))
        {
            deviceExtensions.push_back("VK_KHR_portability_subset");
        }

        // Requesting all supported features, which will then be activated in the device
        // By requesting, it turns on all feature that it is supported, but the user could request specific features instead
        _deviceFeatures.pNext = &_features11;
        vkGetPhysicalDeviceFeatures2(_physicalDevice, &_deviceFeatures);

        // The extension name may be present while the specific feature bit isn't (some
        // drivers expose VK_EXT_device_fault without the base deviceFault feature) --
        // vkGetDeviceFaultInfoEXT is only valid to call once this feature is actually enabled.
        _deviceFaultExtensionEnabled = requestDeviceFaultExtension && _deviceFaultFeatures.deviceFault == VK_TRUE;

        // Same caveat as device fault: the extension name can be present while the feature bit is
        // not, and both bits have to be on -- an acceleration structure nothing can traverse is
        // not a ray-tracing capability.
        _rayTracingEnabled = requestRayTracing &&
                             _accelerationStructureFeatures.accelerationStructure == VK_TRUE &&
                             _rayQueryFeatures.rayQuery == VK_TRUE;

        // Same "name present but feature bit off" caveat as device fault above. When it is
        // on, ask which layouts vkCopyMemoryToImage accepts as a destination: copying
        // straight into SHADER_READ_ONLY_OPTIMAL means an upload needs no layout transition
        // at all, so prefer it and fall back to GENERAL (which the spec guarantees).
        _hostImageCopyEnabled = requestHostImageCopy && _hostImageCopyFeatures.hostImageCopy == VK_TRUE;
        if (_hostImageCopyEnabled)
        {
            VkPhysicalDeviceHostImageCopyProperties hostImageCopyProperties{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_PROPERTIES,
            };
            VkPhysicalDeviceProperties2 hostCopyProps2{
                .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
                .pNext = &hostImageCopyProperties,
            };
            vkGetPhysicalDeviceProperties2(_physicalDevice, &hostCopyProps2);

            // Both arrays are sized and pointed at real storage before the second call. The
            // src list is not used here, but leaving its pointer null while its count stays
            // non-zero is the kind of half-filled enumerate struct drivers handle
            // inconsistently -- cheaper to allocate it than to rely on that.
            std::vector<VkImageLayout> copySrcLayouts(hostImageCopyProperties.copySrcLayoutCount);
            std::vector<VkImageLayout> copyDstLayouts(hostImageCopyProperties.copyDstLayoutCount);
            hostImageCopyProperties.pCopySrcLayouts = copySrcLayouts.data();
            hostImageCopyProperties.pCopyDstLayouts = copyDstLayouts.data();
            vkGetPhysicalDeviceProperties2(_physicalDevice, &hostCopyProps2);

            _hostImageCopyDstLayout = VK_IMAGE_LAYOUT_GENERAL;
            for (const VkImageLayout layout : copyDstLayouts)
            {
                if (layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
                {
                    _hostImageCopyDstLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    break;
                }
            }
        }

        // maintenance5/6 are requested and activated opportunistically above when the driver
        // supports them, but are not required: MoltenVK does not expose either extension yet.
        if ( _features13.dynamicRendering == false || _features13.maintenance4 == false )
        {
            VKM_DEBUG_ERROR("Required Vulkan 1.3 features are not supported");
            return VkmInitResult{VkmInitResultCode::HardwareUnsupported, "This GPU/driver does not support required Vulkan 1.3 features (dynamicRendering, maintenance4)."};
        }

        // The engine-global bindless resource-binding set (VkmBindlessResourceManagerVulkan)
        // relies on descriptor indexing with update-after-bind semantics -- core Vulkan 1.2
        // fields, so no extension string is needed, but the driver must actually support them.
        // The set has both a sampled-image binding (texture array) and storage-buffer
        // bindings (vertex/index arrays), so both per-type update-after-bind bits are required.
        if ( _features12.descriptorIndexing == false ||
             _features12.shaderSampledImageArrayNonUniformIndexing == false ||
             _features12.shaderStorageBufferArrayNonUniformIndexing == false ||
             _features12.descriptorBindingSampledImageUpdateAfterBind == false ||
             _features12.descriptorBindingStorageBufferUpdateAfterBind == false ||
             _features12.descriptorBindingPartiallyBound == false ||
             _features12.runtimeDescriptorArray == false )
        {
            VKM_DEBUG_ERROR("Required Vulkan 1.2 descriptor-indexing features are not supported");
            return VkmInitResult{VkmInitResultCode::HardwareUnsupported, "This GPU/driver does not support required descriptor-indexing features (see VkPhysicalDeviceVulkan12Features)."};
        }

        // Optional, not required: the GPU-driven scene path prefers vkCmdDrawIndirectCount (core in
        // 1.2) so the draw count comes from GPU memory, but falls back to issuing the whole
        // maxDrawCount range where it is unavailable (MoltenVK). That fallback is correct because
        // the culling pass compacts survivors to the front and zeroes the tail.
        _drawIndirectCountSupported = (_features12.drawIndirectCount == VK_TRUE);
        if (!_drawIndirectCountSupported)
        {
            VKM_DEBUG_INFO("vkCmdDrawIndirectCount is unavailable; GPU-driven draws will issue the full argument range");
        }

        // Also optional: without it getGPUVirtualAddress() reports 0 everywhere and nothing else
        // changes -- no buffer carries VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT and VMA is not
        // told to expect one. The feature itself is already enabled by the "request everything
        // supported" chain above; this only records whether the GPU offered it, and it has to be
        // known before the VMA allocator is created below.
        _bufferDeviceAddressEnabled = (_features12.bufferDeviceAddress == VK_TRUE);
        if (!_bufferDeviceAddressEnabled)
        {
            VKM_DEBUG_INFO("VkPhysicalDeviceVulkan12Features::bufferDeviceAddress is unavailable; buffer GPU addresses will report 0");
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

        VmaVulkanFunctions vmaVulkanFunctions{};
        vmaVulkanFunctions.vkGetInstanceProcAddr = vkGetInstanceProcAddr;
        vmaVulkanFunctions.vkGetDeviceProcAddr = vkGetDeviceProcAddr;
        // VMA has to be told when buffers will carry VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        // because their memory needs VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT at allocation time.
        const VmaAllocatorCreateInfo allocatorCreateInfo{
            .flags            = _bufferDeviceAddressEnabled ? VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT : VmaAllocatorCreateFlags{0},
            .physicalDevice   = _physicalDevice,
            .device           = _device,
            .pVulkanFunctions = &vmaVulkanFunctions,
            .instance         = _instance,
            .vulkanApiVersion = VK_API_VERSION_1_3,
        };
        VKM_VK_CHECK_RESULT_MSG_RETURN(vmaCreateAllocator(&allocatorCreateInfo, &_vmaAllocator), "Failed to create VMA allocator");

        // Only now, after volkLoadDevice, do the device entry points exist. The core-1.4
        // names (vkCopyMemoryToImage) are loaded only on a 1.4+ device, so on 1.3 +
        // VK_EXT_host_image_copy only the EXT names are non-null -- checking the exact
        // pointers writeRegion calls turns a loader/driver mismatch into a clean fall back to
        // staging instead of a null call.
        if (_hostImageCopyEnabled && (vkCopyMemoryToImageEXT == nullptr || vkTransitionImageLayoutEXT == nullptr))
        {
            VKM_DEBUG_WARN("VK_EXT_host_image_copy is enabled but its entry points did not load; host-copy texture upload is disabled");
            _hostImageCopyEnabled = false;
        }

        // A memory type that is both DEVICE_LOCAL and HOST_VISIBLE means the CPU can reach
        // GPU memory without a copy through a separate host heap. That property -- not
        // VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU -- is what makes a host-side texture write
        // worthwhile, and it also catches ReBAR-style discrete configurations correctly.
        {
            VkPhysicalDeviceMemoryProperties memoryProperties{};
            vkGetPhysicalDeviceMemoryProperties(_physicalDevice, &memoryProperties);
            constexpr VkMemoryPropertyFlags kUnifiedFlags =
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
            for (uint32_t typeIndex = 0; typeIndex < memoryProperties.memoryTypeCount; ++typeIndex)
            {
                if ((memoryProperties.memoryTypes[typeIndex].propertyFlags & kUnifiedFlags) == kUnifiedFlags)
                {
                    _hasUnifiedMemory = true;
                    break;
                }
            }
        }

        _driverCapabilityFlags = VkmDriverCapabilityFlags::CommandBufferReusable | VkmDriverCapabilityFlags::TextureUpload |
                                 VkmDriverCapabilityFlags::BindlessTextures;
        // Both halves are required: the extension makes a host write to an OPTIMAL-tiled
        // image correct, unified memory makes it worth doing.
        if (_hostImageCopyEnabled && _hasUnifiedMemory)
        {
            _driverCapabilityFlags = _driverCapabilityFlags | VkmDriverCapabilityFlags::TextureHostCopy;
        }
        if (_bufferDeviceAddressEnabled)
        {
            _driverCapabilityFlags = _driverCapabilityFlags | VkmDriverCapabilityFlags::BufferDeviceAddress;
        }
        if (_rayTracingEnabled)
        {
            _driverCapabilityFlags = _driverCapabilityFlags | VkmDriverCapabilityFlags::RayTracing;
        }

        // Both must exist before VkmEngine::initializeBackendDriver() loads engine PSOs, since
        // pipeline-layout creation (VkmPipelineStateVulkan::createInner) needs the bindless
        // set 0 layout and the per-frame set 1 layout.
        auto bindlessResourceManager = std::make_unique<VkmBindlessResourceManagerVulkan>(this);
        if (!bindlessResourceManager->initialize())
        {
            return VkmInitResult{VkmInitResultCode::Failed, "Failed to initialize bindless resource manager"};
        }
        _bindlessResourceManager = std::move(bindlessResourceManager);

        auto frameConstantManager = std::make_unique<VkmFrameConstantManagerVulkan>(this);
        if (!frameConstantManager->initialize())
        {
            return VkmInitResult{VkmInitResultCode::Failed, "Failed to initialize frame constant manager"};
        }
        _frameConstantManager = std::move(frameConstantManager);

        return VkmInitResult{VkmInitResultCode::Success, ""};
    }

    void VkmDriverVulkan::destroyInner()
    {
        setActiveVulkanDriver(nullptr);

        if (_frameConstantManager)
        {
            _frameConstantManager->destroy();
            _frameConstantManager.reset();
        }
        if (_bindlessResourceManager)
        {
            _bindlessResourceManager->destroy();
            _bindlessResourceManager.reset();
        }

        // _bufferPools must be torn down explicitly here (not left to the class destructor)
        // since each pool's VMA-backed VkBuffer must be destroyed while _vmaAllocator is
        // still valid; destroyInner() runs before ~VkmDriverVulkan()'s automatic member
        // destruction, so this ordering is required, not incidental.
        _bufferPools.clear();

        // VMA allocations must be freed before the VmaAllocator is destroyed; this must
        // remain the last step here as other resource teardown is added.
        if (_vmaAllocator != VK_NULL_HANDLE)
        {
            vmaDestroyAllocator(_vmaAllocator);
            _vmaAllocator = VK_NULL_HANDLE;
        }
    }

    bool VkmDriverVulkan::allocateFromBufferPool(uint64_t sizeBytes, uint32_t alignment, PooledBufferAllocation* outResult)
    {
        for (auto& pool : _bufferPools)
        {
            VkmGpuMemoryAllocation allocation{};
            if (pool->tryAllocate(sizeBytes, alignment, &allocation))
            {
                outResult->buffer = pool->getBuffer();
                outResult->allocation = allocation;
                outResult->ownerPool = pool.get();
                return true;
            }
        }

        if (sizeBytes > VkmGpuBufferPoolVulkan::POOL_BLOCK_SIZE_BYTES)
        {
            VKM_DEBUG_ERROR("Buffer allocation exceeds pool block size; use a committed allocation instead");
            return false;
        }

        auto newPool = std::make_unique<VkmGpuBufferPoolVulkan>(this);
        if (!newPool->initialize())
        {
            return false;
        }

        VkmGpuMemoryAllocation allocation{};
        if (!newPool->tryAllocate(sizeBytes, alignment, &allocation))
        {
            return false;
        }

        outResult->buffer = newPool->getBuffer();
        outResult->allocation = allocation;
        outResult->ownerPool = newPool.get();
        _bufferPools.push_back(std::move(newPool));
        return true;
    }
}