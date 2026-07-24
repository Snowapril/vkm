// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/gpu_offset_allocator.h>
#include <vkm/renderer/backend/vulkan/vulkan_bindless_resource_manager.h>
#include <volk.h>

// Mirrors VMA's own VK_DEFINE_HANDLE(VmaAllocator) expansion (vk_mem_alloc.h), so this
// lightweight header doesn't need to include the vendored VMA header.
typedef struct VmaAllocator_T* VmaAllocator;

namespace vkm
{
    class VkmGpuBufferPoolVulkan;
    class VkmGpuTimerVulkan;

    /*
    * @brief renderer backend driver base class
    * @details
    */
    class VkmDriverVulkan : public VkmDriverBase
    {
    public:
        VkmDriverVulkan();
        ~VkmDriverVulkan();

        /*
        * @brief Create swapchain with window info
        */
        virtual VkmSwapChainBase* newSwapChainInner() override final;

        inline VkDevice getDevice() const { return _device; }
        inline VkPhysicalDevice getPhysicalDevice() const { return _physicalDevice; }
        inline VkInstance getInstance() const { return _instance; }
        inline VmaAllocator getVmaAllocator() const { return _vmaAllocator; }
        // Shadows VkmDriverBase::getBindlessResourceManager() with the Vulkan-typed manager
        // (the base member always holds a VkmBindlessResourceManagerVulkan for this driver).
        inline VkmBindlessResourceManagerVulkan* getBindlessResourceManager() const
        {
            return static_cast<VkmBindlessResourceManagerVulkan*>(_bindlessResourceManager.get());
        }
        inline VkmGpuTimerVulkan* getGpuTimer() const { return _gpuTimer.get(); }

        /*
        * @brief true if VK_EXT_device_fault was requested (enableGpuCrashDump) and the
        * GPU/driver actually supports it. Guards whether vkGetDeviceFaultInfoEXT() may be
        * called from vulkan_util.cpp's device-lost handling.
        */
        inline bool isDeviceFaultExtensionEnabled() const { return _deviceFaultExtensionEnabled; }

        uint32_t getQueueFamilyIndex(VkmCommandQueueType queueType) const;

        struct PooledBufferAllocation
        {
            VkBuffer buffer{VK_NULL_HANDLE};
            VkmGpuMemoryAllocation allocation{};
            VkmGpuBufferPoolVulkan* ownerPool{nullptr};
        };

        /*
        * @brief Suballocate a sub-range from an existing (or newly grown) buffer pool block.
        */
        bool allocateFromBufferPool(uint64_t sizeBytes, uint32_t alignment, PooledBufferAllocation* outResult);

    protected:
        virtual VkmInitResult initializeInner(const VkmEngineLaunchOptions* options) override final;
        virtual void destroyInner() override final;
        virtual VkmTexture* newTextureInner() override final;
        virtual VkmBuffer* newBufferInner() override final;
        virtual VkmStagingBuffer* newStagingBufferInner() override final;
        virtual VkmSampler* newSamplerInner() override final;
        virtual VkmTextureView* newTextureViewInner() override final;
        virtual VkmBufferView* newBufferViewInner() override final;
        virtual VkmCommandQueueBase* newCommandQueueInner() override final;
        virtual VkmPipelineStateBase* newPipelineStateInner() override final;
        virtual VkmRenderResourcePool* newRenderResourcePoolInner() override final;
        virtual VkmFormat selectSwapChainColorFormat(bool enableHdr) const override final;

    private:
        VmaAllocator _vmaAllocator{VK_NULL_HANDLE};
        std::vector<std::unique_ptr<VkmGpuBufferPoolVulkan>> _bufferPools;
        std::unique_ptr<VkmGpuTimerVulkan> _gpuTimer;

        uint32_t _graphicsQueueFamilyIndex{UINT32_MAX};
        uint32_t _computeQueueFamilyIndex{UINT32_MAX};
        uint32_t _transferQueueFamilyIndex{UINT32_MAX};

        VkInstance                  _instance{VK_NULL_HANDLE};
        VkPhysicalDevice            _physicalDevice{VK_NULL_HANDLE};
        VkDevice                    _device{VK_NULL_HANDLE};
        VkDebugUtilsMessengerEXT    _callback{VK_NULL_HANDLE};

        VkPhysicalDeviceFeatures2        _deviceFeatures{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
        VkPhysicalDeviceVulkan11Features _features11{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES};
        VkPhysicalDeviceVulkan12Features _features12{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        VkPhysicalDeviceVulkan13Features _features13{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT _swapchainFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
        VkPhysicalDeviceMaintenance5FeaturesKHR _maintenance5Features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR};
        VkPhysicalDeviceMaintenance6FeaturesKHR _maintenance6Features{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_6_FEATURES_KHR};
        VkPhysicalDeviceExtendedDynamicStateFeaturesEXT _dynamicStateFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_FEATURES_EXT};
        VkPhysicalDeviceExtendedDynamicState2FeaturesEXT _dynamicState2Features{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_2_FEATURES_EXT};
        VkPhysicalDeviceExtendedDynamicState3FeaturesEXT _dynamicState3Features{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT};
        VkPhysicalDevicePushDescriptorPropertiesKHR _pushDescriptorProperties{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PUSH_DESCRIPTOR_PROPERTIES_KHR};
        std::vector<VkBaseOutStructure*> _linkedDeviceProperties{reinterpret_cast<VkBaseOutStructure*>(&_pushDescriptorProperties)};

        VkPhysicalDeviceFaultFeaturesEXT _deviceFaultFeatures{.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT};
        bool _deviceFaultExtensionEnabled{false};
    };
}