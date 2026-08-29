// Copyright (c) 2026 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_acceleration_structure.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_buffer.h>
#include <vkm/renderer/backend/common/buffer_view.h>
#include <vkm/renderer/backend/vulkan/vulkan_command_buffer.h>
#include <vkm/renderer/backend/vulkan/vulkan_util.h>
#include <vkm/renderer/backend/common/command_queue.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>

// Last, deliberately. It pulls in <vulkan/vulkan.h> with the platform defines, which on Linux
// brings X11's `#define None 0L` with it -- so any engine header enumerating a `None` that follows
// it stops compiling. Every engine include above is therefore already resolved by this point.
#include <vk_mem_alloc.h>

#include <cstring>

namespace vkm
{
    namespace
    {
        /*
        * @brief The device address a build should read a geometry view from.
        * @details A build reads its vertex and index data in place, which is what the
        * `AllowAccelerationStructureInput` flag is for: without it the buffer may be sub-allocated
        * from a pool block carrying no device-address usage, making the address meaningless rather
        * than absent. The parent's own address already carries its sub-allocation offset, so the
        * view's RELATIVE offset is what gets added -- `VkmBufferViewVulkan::getOffset()` is
        * absolute and would count a pooled buffer's offset twice.
        * @param pool Pool the view is resolved through.
        * @param viewHandle View naming the range to read.
        * @return The address, or 0 when the view or its parent cannot report one.
        */
        VkDeviceAddress viewAddress(VkmRenderResourcePool* pool, VkmResourceHandle viewHandle)
        {
            VkmBufferView* view = pool->getResource<VkmBufferView>(viewHandle);
            if (view == nullptr)
            {
                return 0;
            }
            VkmBuffer* parent = view->tryGetParent();
            if (parent == nullptr || parent->getGPUVirtualAddress() == 0)
            {
                return 0;
            }
            return static_cast<VkDeviceAddress>(parent->getGPUVirtualAddress()) +
                   static_cast<VkDeviceAddress>(view->getBufferViewInfo()._offset);
        }
    } // namespace

    VkmAccelerationStructureVulkan::VkmAccelerationStructureVulkan(VkmDriverBase* driver)
        : VkmAccelerationStructure(driver), _driverVulkan(static_cast<VkmDriverVulkan*>(driver))
    {
    }

    VkmAccelerationStructureVulkan::~VkmAccelerationStructureVulkan()
    {
        releaseAll();
    }

    bool VkmAccelerationStructureVulkan::buildGeometryDescriptions(
        const VkmAccelerationStructureInfo& info,
        std::vector<VkAccelerationStructureGeometryKHR>* outGeometries,
        std::vector<uint32_t>* outPrimitiveCounts)
    {
        VkmRenderResourcePool* pool = _driverVulkan->getRenderResourcePool();

        if (info._type == VkmAccelerationStructureType::TopLevel)
        {
            if (!createInstanceBuffer(info))
            {
                return false;
            }
            VkAccelerationStructureGeometryKHR geometry{
                .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                .geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR,
                .flags        = VK_GEOMETRY_OPAQUE_BIT_KHR,
            };
            geometry.geometry.instances = VkAccelerationStructureGeometryInstancesDataKHR{
                .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR,
            };
            if (_instanceBuffer != VK_NULL_HANDLE)
            {
                const VkBufferDeviceAddressInfo addressInfo{
                    .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
                    .buffer = _instanceBuffer,
                };
                geometry.geometry.instances.data.deviceAddress =
                    vkGetBufferDeviceAddress(_driverVulkan->getDevice(), &addressInfo);
            }
            outGeometries->push_back(geometry);
            outPrimitiveCounts->push_back(static_cast<uint32_t>(info._instances.size()));
            return true;
        }

        outGeometries->reserve(info._geometries.size());
        outPrimitiveCounts->reserve(info._geometries.size());
        for (const VkmAccelerationStructureGeometry& source : info._geometries)
        {
            if (source._indexCount == 0 || (source._indexCount % 3) != 0)
            {
                VKM_DEBUG_ERROR("Acceleration structure geometry needs a non-zero index count that is a multiple of 3");
                return false;
            }
            const VkDeviceAddress vertexAddress = viewAddress(pool, source._vertexView);
            const VkDeviceAddress indexAddress = viewAddress(pool, source._indexView);
            if (vertexAddress == 0 || indexAddress == 0)
            {
                // Almost always a missing AllowAccelerationStructureInput on the buffer the view
                // was made over, so the message names that rather than the symptom.
                VKM_DEBUG_ERROR("Acceleration structure geometry references a buffer view whose buffer has "
                                "no device address; it needs VkmResourceCreateInfo::AllowAccelerationStructureInput");
                return false;
            }

            VkAccelerationStructureGeometryKHR geometry{
                .sType        = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR,
                .geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR,
                // Opaque so traversal never calls an any-hit shader: the engine has no alpha-tested
                // ray path, and marking geometry non-opaque without one silently accepts every hit.
                .flags        = VK_GEOMETRY_OPAQUE_BIT_KHR,
            };
            geometry.geometry.triangles = VkAccelerationStructureGeometryTrianglesDataKHR{
                .sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR,
                .vertexFormat  = VK_FORMAT_R32G32B32_SFLOAT,
                .vertexData    = { .deviceAddress = vertexAddress },
                .vertexStride  = source._vertexStride,
                .maxVertex     = source._vertexCount > 0 ? source._vertexCount - 1 : 0,
                .indexType     = VK_INDEX_TYPE_UINT32,
                .indexData     = { .deviceAddress = indexAddress },
                .transformData = { .deviceAddress = 0 },
            };
            outGeometries->push_back(geometry);
            outPrimitiveCounts->push_back(source._indexCount / 3);
        }
        return true;
    }

    bool VkmAccelerationStructureVulkan::createInstanceBuffer(const VkmAccelerationStructureInfo& info)
    {
        if (info._instances.empty())
        {
            return true; // an empty top-level structure is valid; there is nothing to upload
        }

        std::vector<VkAccelerationStructureInstanceKHR> instances;
        instances.reserve(info._instances.size());
        for (const VkmAccelerationStructureInstance& source : info._instances)
        {
            VkmAccelerationStructure* blas =
                _driverVulkan->getRenderResourcePool()->getResource<VkmAccelerationStructure>(source._blas);
            if (blas == nullptr)
            {
                VKM_DEBUG_ERROR("Top-level acceleration structure instance references an invalid bottom-level handle");
                return false;
            }

            VkAccelerationStructureInstanceKHR instance{};
            // glm is column-major and VkTransformMatrixKHR is row-major 3x4, so this transposes
            // while it copies rather than memcpy'ing a matrix that would come out rotated.
            for (uint32_t row = 0; row < 3; ++row)
            {
                for (uint32_t column = 0; column < 4; ++column)
                {
                    instance.transform.matrix[row][column] = source._transform[column][row];
                }
            }
            instance.instanceCustomIndex = source._instanceId & 0x00FFFFFFu;
            instance.mask = 0xFF;
            instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            instance.accelerationStructureReference =
                static_cast<VkmAccelerationStructureVulkan*>(blas)->getDeviceAddress();
            instances.push_back(instance);
        }

        const VkDeviceSize size = instances.size() * sizeof(VkAccelerationStructureInstanceKHR);
        const VkBufferCreateInfo bufferCreateInfo{
            .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size        = size,
            .usage       = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR |
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VmaAllocationCreateInfo allocCreateInfo{};
        allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocCreateInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT |
                                VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        VmaAllocationInfo allocationInfo{};
        if (!VKM_VK_CHECK_RESULT_MSG(vmaCreateBuffer(_driverVulkan->getVmaAllocator(), &bufferCreateInfo,
                                                     &allocCreateInfo, &_instanceBuffer,
                                                     &_instanceAllocation, &allocationInfo),
                                     "Failed to create the acceleration structure instance buffer"))
        {
            return false;
        }
        std::memcpy(allocationInfo.pMappedData, instances.data(), static_cast<size_t>(size));
        _instanceMapped = allocationInfo.pMappedData;
        _instanceCapacity = static_cast<uint32_t>(instances.size());
        return true;
    }

    bool VkmAccelerationStructureVulkan::createStorage(VkDeviceSize size)
    {
        const VkBufferCreateInfo bufferCreateInfo{
            .sType       = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size        = size,
            .usage       = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR |
                           VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        };
        VmaAllocationCreateInfo allocCreateInfo{};
        allocCreateInfo.usage = VMA_MEMORY_USAGE_AUTO;
        allocCreateInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        return VKM_VK_CHECK_RESULT_MSG(vmaCreateBuffer(_driverVulkan->getVmaAllocator(), &bufferCreateInfo,
                                                       &allocCreateInfo, &_storageBuffer,
                                                       &_storageAllocation, nullptr),
                                       "Failed to create the acceleration structure storage buffer");
    }

    bool VkmAccelerationStructureVulkan::initialize(VkmResourceHandle handle,
                                                    const VkmAccelerationStructureInfo& info)
    {
        if (!initializeAccelerationStructureCommon(handle, info))
        {
            return false;
        }

        _allowUpdate = info._allowUpdate;
        if (!buildGeometryDescriptions(info, &_geometries, &_primitiveCounts))
        {
            return false;
        }

        const VkAccelerationStructureTypeKHR type =
            info._type == VkmAccelerationStructureType::TopLevel
                ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
                : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;

        VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
            .sType         = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
            .type          = type,
            // PREFER_FAST_TRACE always: traversal cost dominates, and a top-level rebuild over a
            // few thousand instances is cheap even at the highest build quality. ALLOW_UPDATE only
            // when asked, because it constrains how the driver may lay the structure out.
            .flags         = static_cast<VkBuildAccelerationStructureFlagsKHR>(
                                 VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                                 (info._allowUpdate ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0)),
            .mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
            .geometryCount = static_cast<uint32_t>(_geometries.size()),
            .pGeometries   = _geometries.data(),
        };

        VkAccelerationStructureBuildSizesInfoKHR sizes{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
        };
        vkGetAccelerationStructureBuildSizesKHR(_driverVulkan->getDevice(),
                                                VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo,
                                                _primitiveCounts.data(), &sizes);

        if (!createStorage(sizes.accelerationStructureSize))
        {
            return false;
        }

        const VkAccelerationStructureCreateInfoKHR createInfo{
            .sType  = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR,
            .buffer = _storageBuffer,
            .offset = 0,
            .size   = sizes.accelerationStructureSize,
            .type   = type,
        };
        if (!VKM_VK_CHECK_RESULT_MSG(vkCreateAccelerationStructureKHR(_driverVulkan->getDevice(), &createInfo,
                                                                      nullptr, &_accelerationStructure),
                                     "Failed to create the acceleration structure"))
        {
            return false;
        }

        // Scratch is build-time only: created here, destroyed as soon as the submit completes.
        // Its offset alignment is a device property rather than a fixed number, and a build reads
        // past its own start when the driver wants more room.
        VkPhysicalDeviceAccelerationStructurePropertiesKHR accelerationProperties{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR,
        };
        VkPhysicalDeviceProperties2 properties2{
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &accelerationProperties,
        };
        vkGetPhysicalDeviceProperties2(_driverVulkan->getPhysicalDevice(), &properties2);

        {
            const VkBufferCreateInfo scratchCreateInfo{
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .size  = sizes.buildScratchSize +
                         accelerationProperties.minAccelerationStructureScratchOffsetAlignment,
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            };
            VmaAllocationCreateInfo scratchAllocInfo{};
            scratchAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
            scratchAllocInfo.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
            if (!VKM_VK_CHECK_RESULT_MSG(vmaCreateBuffer(_driverVulkan->getVmaAllocator(), &scratchCreateInfo,
                                                         &scratchAllocInfo, &_scratchBuffer,
                                                         &_scratchAllocation, nullptr),
                                         "Failed to create the acceleration structure scratch buffer"))
            {
                return false;
            }
        }

        const VkBufferDeviceAddressInfo scratchAddressInfo{
            .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .buffer = _scratchBuffer,
        };
        _scratchAddress = vkGetBufferDeviceAddress(_driverVulkan->getDevice(), &scratchAddressInfo);
        const VkDeviceSize scratchAlignment = accelerationProperties.minAccelerationStructureScratchOffsetAlignment;
        if (scratchAlignment > 0)
        {
            _scratchAddress = (_scratchAddress + scratchAlignment - 1) & ~(scratchAlignment - 1);
        }

        /*
        * Recorded into a command buffer from the engine's own pool and submitted through the
        * driver's setup path, with only the build call itself reaching for the raw handle.
        * Duplicating a command pool, a fence and a submit here to stay "pure" would be more Vulkan
        * code, not less. The shape matches VkmDriverBase::uploadToBuffer, and a scene builds one
        * structure per mesh, so the submission is the driver's to batch.
        */
        VkmCommandBufferBase* commandBuffer = _driverVulkan->acquireSetupCommandBuffer();
        if (commandBuffer == nullptr)
        {
            return false;
        }
        // Through the common entry point, like every other caller: it is what checks the recording
        // rules and records the usages the deferred reclaimer waits on. Reaching for the raw
        // VkCommandBuffer here skipped both.
        commandBuffer->buildAccelerationStructure(handle);

        const VkAccelerationStructureDeviceAddressInfoKHR deviceAddressInfo{
            .sType                 = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
            .accelerationStructure = _accelerationStructure,
        };
        _deviceAddress = vkGetAccelerationStructureDeviceAddressKHR(_driverVulkan->getDevice(), &deviceAddressInfo);
        _structureSize = sizes.accelerationStructureSize;

        // The scratch is what a batch has to budget: it stays live until the build retires.
        _driverVulkan->retireAfterSetupSubmit(this);
        return _driverVulkan->submitSetupCommandBuffer(sizes.buildScratchSize);
    }

    void VkmAccelerationStructureVulkan::onSetupBuildCompleted()
    {
        if (!_allowUpdate)
        {
            // A structure that will never be rebuilt has no further use for its scratch, and it is
            // the largest allocation here on a big scene. Freed only now: the build reads it, so
            // it has to outlive the submission that carries the build.
            vmaDestroyBuffer(_driverVulkan->getVmaAllocator(), _scratchBuffer, _scratchAllocation);
            _scratchBuffer = VK_NULL_HANDLE;
            _scratchAllocation = nullptr;
            _scratchAddress = 0;
        }
    }

    bool VkmAccelerationStructureVulkan::updateInstances(
        const std::vector<VkmAccelerationStructureInstance>& instances)
    {
        if (_info._type != VkmAccelerationStructureType::TopLevel)
        {
            VKM_DEBUG_ERROR("updateInstances is only valid on a top-level acceleration structure");
            return false;
        }
        if (_instanceMapped == nullptr)
        {
            VKM_DEBUG_ERROR("updateInstances on a structure created with no instances");
            return false;
        }
        if (instances.size() > _instanceCapacity)
        {
            // Growing would need a bigger buffer, and the structure was *sized* against the
            // original count -- a longer list would overrun what the build was told to expect.
            VKM_DEBUG_ERROR("updateInstances was given more instances than the structure was created with");
            return false;
        }

        VkmRenderResourcePool* pool = _driverVulkan->getRenderResourcePool();
        VkAccelerationStructureInstanceKHR* destination =
            static_cast<VkAccelerationStructureInstanceKHR*>(_instanceMapped);
        for (size_t i = 0; i < instances.size(); ++i)
        {
            const VkmAccelerationStructureInstance& source = instances[i];
            VkmAccelerationStructure* blas = pool->getResource<VkmAccelerationStructure>(source._blas);
            if (blas == nullptr)
            {
                VKM_DEBUG_ERROR("updateInstances references an invalid bottom-level handle");
                return false;
            }

            VkAccelerationStructureInstanceKHR instance{};
            // Same transpose as createInstanceBuffer: glm is column-major, VkTransformMatrixKHR is
            // row-major 3x4.
            for (uint32_t row = 0; row < 3; ++row)
            {
                for (uint32_t column = 0; column < 4; ++column)
                {
                    instance.transform.matrix[row][column] = source._transform[column][row];
                }
            }
            instance.instanceCustomIndex = source._instanceId & 0x00FFFFFFu;
            instance.mask = 0xFF;
            instance.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
            instance.accelerationStructureReference =
                static_cast<VkmAccelerationStructureVulkan*>(blas)->getDeviceAddress();
            destination[i] = instance;
        }

        // The primitive count is the instance count, so a shorter list has to shrink the build's
        // range too -- otherwise the build reads stale descriptors past the end of what was written.
        if (!_primitiveCounts.empty())
        {
            _primitiveCounts[0] = static_cast<uint32_t>(instances.size());
        }
        // Keep the retained info matching what the next build reads, so the command buffer's
        // GPU-usage walk and the debug inspector see the rebuilt structure, not the created one.
        _info._instances = instances;
        return true;
    }

    void VkmAccelerationStructureVulkan::setDebugName(const char* name)
    {
#ifdef VKM_DEBUG_NAME_ENABLED
        const VkDebugUtilsObjectNameInfoEXT nameInfo{
            .sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
            .objectType   = VK_OBJECT_TYPE_ACCELERATION_STRUCTURE_KHR,
            .objectHandle = reinterpret_cast<uint64_t>(_accelerationStructure),
            .pObjectName  = name,
        };
        VKM_VK_CHECK_RESULT_MSG(vkSetDebugUtilsObjectNameEXT(_driverVulkan->getDevice(), &nameInfo),
            "Failed to set debug name on acceleration structure");
#else
        (void)name;
#endif
    }

    void VkmAccelerationStructureVulkan::releaseInstanceBuffer()
    {
        if (_instanceBuffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(_driverVulkan->getVmaAllocator(), _instanceBuffer, _instanceAllocation);
            _instanceBuffer = VK_NULL_HANDLE;
            _instanceAllocation = nullptr;
            _instanceMapped = nullptr;
            _instanceCapacity = 0;
        }
    }

    void VkmAccelerationStructureVulkan::releaseAll()
    {
        if (_accelerationStructure != VK_NULL_HANDLE)
        {
            vkDestroyAccelerationStructureKHR(_driverVulkan->getDevice(), _accelerationStructure, nullptr);
            _accelerationStructure = VK_NULL_HANDLE;
        }
        if (_storageBuffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(_driverVulkan->getVmaAllocator(), _storageBuffer, _storageAllocation);
            _storageBuffer = VK_NULL_HANDLE;
            _storageAllocation = nullptr;
        }
        releaseInstanceBuffer();
        if (_scratchBuffer != VK_NULL_HANDLE)
        {
            vmaDestroyBuffer(_driverVulkan->getVmaAllocator(), _scratchBuffer, _scratchAllocation);
            _scratchBuffer = VK_NULL_HANDLE;
            _scratchAllocation = nullptr;
        }
        _scratchAddress = 0;
        _deviceAddress = 0;
    }
} // namespace vkm
