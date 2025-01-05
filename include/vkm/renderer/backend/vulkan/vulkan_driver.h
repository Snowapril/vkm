// Copyright (c) 2024 Snowapril

#pragma once

#include <vkm/renderer/backend/common/driver.h>
#include <volk.h>

namespace vkm
{
    /*
    * @brief renderer backend driver base class
    * @details 
    */
    class VkmDriverVulkan : public VkmDriverBase
    {
    public:
        VkmDriverVulkan();
        ~VkmDriverVulkan();

        inline VkDevice getDevice() const { return _device; }
        inline VkPhysicalDevice getPhysicalDevice() const { return _physicalDevice; }
        inline VkInstance getInstance() const { return _instance; }

    protected:
        virtual bool initializeInner(const VkmEngineLaunchOptions* options) override final;
        virtual void destroyInner() override final;

    private:
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
    };
}