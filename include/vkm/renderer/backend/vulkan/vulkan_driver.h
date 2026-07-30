// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/gpu_offset_allocator.h>
#include <vkm/renderer/backend/vulkan/vulkan_bindless_resource_manager.h>
#include <vkm/renderer/backend/vulkan/vulkan_frame_constant_manager.h>
#include <volk.h>

// Mirrors VMA's own VK_DEFINE_HANDLE(VmaAllocator) expansion (vk_mem_alloc.h), so this
// lightweight header doesn't need to include the vendored VMA header.
typedef struct VmaAllocator_T* VmaAllocator;

namespace vkm
{
    class VkmGpuBufferPoolVulkan;

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

        // Shadows VkmDriverBase::getFrameConstantManager() with the Vulkan-typed manager, for
        // the same reason as above.
        inline VkmFrameConstantManagerVulkan* getFrameConstantManager() const
        {
            return static_cast<VkmFrameConstantManagerVulkan*>(_frameConstantManager.get());
        }
        // VMA heap budgets (device allocated/budget) plus its block-vs-allocation split.
        virtual VkmGpuMemoryStats getGpuMemoryStats() const override final;

        // GPU timestamp pool backing VkmGpuProfiler: one VkQueryPool of `slotCount`
        // VK_QUERY_TYPE_TIMESTAMP queries. See driver.h for the contract.
        virtual bool initializeGpuTimestampPool(uint32_t slotCount) override final;
        virtual void destroyGpuTimestampPool() override final;
        virtual double getGpuTimestampPeriodNs() const override final { return _timestampPeriodNs; }
        virtual void resetGpuTimestampSlots(VkmCommandBufferBase* commandBuffer, uint32_t firstSlot,
                                            uint32_t count) override final;
        virtual bool resolveGpuTimestamps(uint32_t firstSlot, uint32_t count, uint64_t* outTicks) override final;

        inline VkQueryPool getGpuTimestampQueryPool() const { return _timestampQueryPool; }

        /*
        * @brief true if VK_EXT_device_fault was requested (enableGpuCrashDump) and the
        * GPU/driver actually supports it. Guards whether vkGetDeviceFaultInfoEXT() may be
        * called from vulkan_util.cpp's device-lost handling.
        */
        inline bool isDeviceFaultExtensionEnabled() const { return _deviceFaultExtensionEnabled; }
        // False on drivers without VkPhysicalDeviceVulkan12Features::drawIndirectCount (MoltenVK):
        // GPU-driven draws then issue the whole argument range instead of a GPU-supplied count.
        inline bool isDrawIndirectCountSupported() const { return _drawIndirectCountSupported; }

        /*
        * @brief Whether VkPhysicalDeviceVulkan12Features::bufferDeviceAddress is on, so buffers
        * may carry VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT and vkGetBufferDeviceAddress is
        * valid to call. Mirrors VkmDriverCapabilityFlags::BufferDeviceAddress, but is readable
        * before that flag is assembled -- the VMA allocator needs it at creation time.
        */
        inline bool isBufferDeviceAddressEnabled() const { return _bufferDeviceAddressEnabled; }

        /*
        * @brief Whether VK_EXT_host_image_copy is enabled *and* its feature bit is on, so
        * vkCopyMemoryToImage / vkTransitionImageLayoutEXT are valid to call.
        */
        inline bool isHostImageCopyEnabled() const { return _hostImageCopyEnabled; }

        // Layout to hand vkCopyMemoryToImage as the destination (see initializeInner).
        inline VkImageLayout getHostImageCopyDstLayout() const { return _hostImageCopyDstLayout; }


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

        VkQueryPool _timestampQueryPool{VK_NULL_HANDLE};
        double _timestampPeriodNs{1.0};
        // Timestamps are only valid in this many low bits on the graphics queue family; the rest
        // are undefined and must be masked off before the value means anything.
        uint64_t _timestampValidMask{UINT64_MAX};

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
        bool _drawIndirectCountSupported{false};
        bool _bufferDeviceAddressEnabled{false};

        VkPhysicalDeviceHostImageCopyFeatures _hostImageCopyFeatures{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_HOST_IMAGE_COPY_FEATURES};
        bool _hostImageCopyEnabled{false};
        // Destination layout vkCopyMemoryToImage is called with; resolved at init from the
        // device's supported list (see initializeInner).
        VkImageLayout _hostImageCopyDstLayout{VK_IMAGE_LAYOUT_GENERAL};
    };
}