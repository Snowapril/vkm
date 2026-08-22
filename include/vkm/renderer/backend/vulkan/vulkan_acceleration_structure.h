// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/renderer/backend/common/acceleration_structure.h>
#include <volk.h>

// Forward-declared rather than including <vk_mem_alloc.h>, matching vulkan_buffer.h. That header
// pulls in <vulkan/vulkan.h> with the platform defines, which on Linux drags in X11, whose
// `#define None 0L` collides with VkmCullMode::None when this header precedes pipeline_state.h.
typedef struct VmaAllocation_T* VmaAllocation;

namespace vkm
{
    class VkmDriverVulkan;

    /*
    * @brief `VkAccelerationStructureKHR` plus the buffer its data lives in.
    * @details Vulkan splits an acceleration structure into a handle and the storage it is placed
    * in, and the caller allocates the storage. Both are owned here, along with the device address a
    * shader-side `RayQuery` traverses. A static structure's scratch buffer is not kept: it is only
    * needed while the build runs, and the build is synchronous.
    */
    class VkmAccelerationStructureVulkan : public VkmAccelerationStructure
    {
    public:
        explicit VkmAccelerationStructureVulkan(VkmDriverBase* driver);
        ~VkmAccelerationStructureVulkan() override;

        bool initialize(VkmResourceHandle handle, const VkmAccelerationStructureInfo& info) override final;
        bool updateInstances(const std::vector<VkmAccelerationStructureInstance>& instances) override final;

        /*
        * What a build needs to know about this structure, for the one caller that records one:
        * VkmCommandBufferVulkan::onBuildAccelerationStructure.
        * `_scratchBuffer` is VK_NULL_HANDLE on a structure built without `_allowUpdate`, whose
        * scratch is freed once the initial build completes, and a rebuild must refuse then rather
        * than read whatever now occupies that memory.
        */
        inline bool isRebuildable() const { return _scratchBuffer != VK_NULL_HANDLE; }
        inline VkDeviceAddress getScratchAddress() const { return _scratchAddress; }
        inline bool allowsUpdate() const { return _allowUpdate; }
        inline const std::vector<VkAccelerationStructureGeometryKHR>& getGeometries() const { return _geometries; }
        inline const std::vector<uint32_t>& getPrimitiveCounts() const { return _primitiveCounts; }

        inline VkAccelerationStructureKHR getAccelerationStructure() const { return _accelerationStructure; }
        // The value an instance descriptor or a shader-side ray query needs. Zero if the build
        // failed or the structure was destroyed.
        inline VkDeviceAddress getDeviceAddress() const { return _deviceAddress; }
        // The size the driver asked for the structure itself, which the memory report tags. Does
        // not count the scratch buffer.
        uint64_t getAllocatedSize() const override final { return _structureSize; }
        void setDebugName(const char* name) override final;

    private:
        // Fills `outGeometries` / `outPrimitiveCounts` from the info, resolving each buffer handle
        // to a device address. Returns false when a referenced buffer cannot report one: a buffer
        // carries VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT only on the committed allocation path.
        bool buildGeometryDescriptions(const VkmAccelerationStructureInfo& info,
                                       std::vector<VkAccelerationStructureGeometryKHR>* outGeometries,
                                       std::vector<uint32_t>* outPrimitiveCounts);
        // Uploads the instance descriptors a top-level build reads, into _instanceBuffer.
        bool createInstanceBuffer(const VkmAccelerationStructureInfo& info);
        bool createStorage(VkDeviceSize size);
        void releaseInstanceBuffer();
        // Not an override: VkmRenderResource has no virtual destroy(), so every backend resource
        // releases from its destructor. A named helper so initialize()'s failure paths can unwind
        // without duplicating it.
        void releaseAll();

        VkmDriverVulkan* _driverVulkan = nullptr;
        VkAccelerationStructureKHR _accelerationStructure = VK_NULL_HANDLE;
        VkDeviceAddress _deviceAddress = 0;
        uint64_t _structureSize = 0;

        VkBuffer _storageBuffer = VK_NULL_HANDLE;
        VmaAllocation _storageAllocation = nullptr;

        // Top-level only: the VkAccelerationStructureInstanceKHR array the build reads. Kept for the
        // structure's lifetime, updateInstances rewriting it in place for the next rebuild.
        VkBuffer _instanceBuffer = VK_NULL_HANDLE;
        VmaAllocation _instanceAllocation = nullptr;
        void* _instanceMapped = nullptr;
        uint32_t _instanceCapacity = 0;

        /*
        * Build-time working memory. Destroyed right after the initial build for a static structure;
        * kept for the lifetime of an updatable one, every rebuild needing it again and
        * reallocating per frame being the expensive part of a cheap operation.
        */
        VkBuffer _scratchBuffer = VK_NULL_HANDLE;
        VmaAllocation _scratchAllocation = nullptr;
        VkDeviceAddress _scratchAddress = 0;

        // Retained so a rebuild can be described without the caller passing anything: it reads the
        // same geometry the structure was created with, only the instance buffer's contents differ.
        std::vector<VkAccelerationStructureGeometryKHR> _geometries;
        std::vector<uint32_t> _primitiveCounts;
        bool _allowUpdate = false;
    };
} // namespace vkm
