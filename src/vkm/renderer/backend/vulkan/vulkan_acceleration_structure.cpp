// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_acceleration_structure.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_buffer.h>
#include <vkm/renderer/backend/vulkan/vulkan_command_buffer.h>
#include <vkm/renderer/backend/vulkan/vulkan_util.h>
#include <vkm/renderer/backend/common/command_queue.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>

#include <cstring>

namespace vkm
{
    namespace
    {
        /*
        * @brief Device address of a buffer the caller handed us, or 0.
        *
        * A build reads its vertex and index data in place, so this is where the
        * `AllowAccelerationStructureInput` flag pays off: without it the buffer may have been
        * sub-allocated from a pool block that carries no device-address usage, and the address
        * would be meaningless rather than absent.
        */
        VkDeviceAddress bufferAddress(VkmRenderResourcePool* pool, VkmResourceHandle handle)
        {
            VkmBuffer* buffer = pool->getResource<VkmBuffer>(handle);
            return buffer != nullptr ? static_cast<VkDeviceAddress>(buffer->getGPUVirtualAddress()) : 0;
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
            const VkDeviceAddress vertexAddress = bufferAddress(pool, source._vertexBuffer);
            const VkDeviceAddress indexAddress = bufferAddress(pool, source._indexBuffer);
            if (vertexAddress == 0 || indexAddress == 0)
            {
                // Almost always a missing AllowAccelerationStructureInput on the source buffer, so
                // the message names that rather than the symptom.
                VKM_DEBUG_ERROR("Acceleration structure geometry references a buffer with no device "
                                "address; it needs VkmResourceCreateInfo::AllowAccelerationStructureInput");
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
                .vertexData    = { .deviceAddress = vertexAddress + source._vertexByteOffset },
                .vertexStride  = source._vertexStride,
                .maxVertex     = source._vertexCount > 0 ? source._vertexCount - 1 : 0,
                .indexType     = VK_INDEX_TYPE_UINT32,
                .indexData     = { .deviceAddress = indexAddress + source._indexByteOffset },
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

        std::vector<VkAccelerationStructureGeometryKHR> geometries;
        std::vector<uint32_t> primitiveCounts;
        if (!buildGeometryDescriptions(info, &geometries, &primitiveCounts))
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
            .flags         = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR,
            .mode          = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
            .geometryCount = static_cast<uint32_t>(geometries.size()),
            .pGeometries   = geometries.data(),
        };

        VkAccelerationStructureBuildSizesInfoKHR sizes{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR,
        };
        vkGetAccelerationStructureBuildSizesKHR(_driverVulkan->getDevice(),
                                                VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &buildInfo,
                                                primitiveCounts.data(), &sizes);

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

        VkBuffer scratchBuffer = VK_NULL_HANDLE;
        VmaAllocation scratchAllocation = nullptr;
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
                                                         &scratchAllocInfo, &scratchBuffer,
                                                         &scratchAllocation, nullptr),
                                         "Failed to create the acceleration structure scratch buffer"))
            {
                return false;
            }
        }

        const VkBufferDeviceAddressInfo scratchAddressInfo{
            .sType  = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO,
            .buffer = scratchBuffer,
        };
        VkDeviceAddress scratchAddress = vkGetBufferDeviceAddress(_driverVulkan->getDevice(), &scratchAddressInfo);
        const VkDeviceSize scratchAlignment = accelerationProperties.minAccelerationStructureScratchOffsetAlignment;
        if (scratchAlignment > 0)
        {
            scratchAddress = (scratchAddress + scratchAlignment - 1) & ~(scratchAlignment - 1);
        }

        buildInfo.dstAccelerationStructure = _accelerationStructure;
        buildInfo.scratchData.deviceAddress = scratchAddress;

        std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
        ranges.reserve(primitiveCounts.size());
        for (uint32_t count : primitiveCounts)
        {
            ranges.push_back(VkAccelerationStructureBuildRangeInfoKHR{ .primitiveCount = count });
        }
        const VkAccelerationStructureBuildRangeInfoKHR* rangePointer = ranges.data();

        /*
        * Recorded into a command buffer from the engine's own pool and submitted through the
        * engine's queue, with only the build call itself reaching for the raw handle. Duplicating
        * a command pool, a fence and a submit here to stay "pure" would be more Vulkan code, not
        * less. The shape matches VkmDriverBase::uploadToBuffer: allocate, record, submit, wait.
        */
        VkmCommandQueueBase* commandQueue = _driverVulkan->getCommandQueue(VkmCommandQueueType::Graphics, 0);
        VkmCommandBufferBase* commandBuffer = commandQueue->getCommandBufferPool()->allocate();
        commandBuffer->beginCommandBuffer();
        vkCmdBuildAccelerationStructuresKHR(
            static_cast<VkmCommandBufferVulkan*>(commandBuffer)->getVkCommandBuffer(), 1, &buildInfo,
            &rangePointer);
        commandBuffer->endCommandBuffer();

        CommandSubmitInfo submitInfo;
        submitInfo.commandBuffers[0] = commandBuffer;
        submitInfo.commandBufferCount = 1;
        const VkmGpuEventTimelineObject submitResult = commandQueue->submit(submitInfo);
        if (submitResult._gpuEventTimeline != nullptr)
        {
            submitResult._gpuEventTimeline->waitIdle(MAX_GPU_TIMEOUT_PER_FRAME);
        }
        commandQueue->getCommandBufferPool()->release(commandBuffer);
        vmaDestroyBuffer(_driverVulkan->getVmaAllocator(), scratchBuffer, scratchAllocation);

        const VkAccelerationStructureDeviceAddressInfoKHR deviceAddressInfo{
            .sType                 = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR,
            .accelerationStructure = _accelerationStructure,
        };
        _deviceAddress = vkGetAccelerationStructureDeviceAddressKHR(_driverVulkan->getDevice(), &deviceAddressInfo);
        _structureSize = sizes.accelerationStructureSize;
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
        vkSetDebugUtilsObjectNameEXT(_driverVulkan->getDevice(), &nameInfo);
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
        _deviceAddress = 0;
    }
} // namespace vkm
