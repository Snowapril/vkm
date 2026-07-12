// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_render_resource_pool.h>
#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/metal/metal_buffer.h>
#include <vkm/renderer/backend/metal/metal_texture.h>
#include <vkm/renderer/backend/metal/metal_staging_buffer.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>

#import <Metal/MTLDevice.h>
#import <Metal/MTLResidencySet.h>

namespace vkm
{
    namespace
    {
        // Only Buffer/Texture/StagingBuffer own a distinct MTLAllocation-conforming native
        // object; samplers, texture views and buffer views have no separate allocation.
        id<MTLAllocation> fetchAllocation(VkmRenderResourcePoolMetal* pool, VkmResourceHandle handle)
        {
            switch (handle.type)
            {
            case VkmResourceType::Buffer:
                return pool->getResource<VkmBufferMetal>(handle)->getBuffer();
            case VkmResourceType::Texture:
                return pool->getResource<VkmTextureMetal>(handle)->getInternalHandle();
            case VkmResourceType::StagingBuffer:
                return pool->getResource<VkmStagingBufferMetal>(handle)->getBuffer();
            default:
                return nil;
            }
        }
    }

    VkmRenderResourcePoolMetal::VkmRenderResourcePoolMetal(VkmDriverBase* driver)
        : VkmRenderResourcePool(driver)
    {
        VkmDriverMetal* driverMetal = static_cast<VkmDriverMetal*>(driver);
        id<MTLDevice> device = driverMetal->getMTLDevice();

        for (uint8_t poolType = 0; poolType < (uint8_t)VkmResourcePoolType::Count; ++poolType)
        {
            MTLResidencySetDescriptor* descriptor = [[MTLResidencySetDescriptor alloc] init];
            descriptor.label = @"VkmResidencySet.Default";

            NSError* error = nil;
            _residencySets[poolType] = [device newResidencySetWithDescriptor:descriptor error:&error];
            if (_residencySets[poolType] == nil)
            {
                VKM_DEBUG_ERROR("Failed to create MTLResidencySet");
            }
        }
    }

    VkmRenderResourcePoolMetal::~VkmRenderResourcePoolMetal()
    {
        // ARC releases the id<MTLResidencySet> members; no manual cleanup needed.
    }

    void VkmRenderResourcePoolMetal::onResourceInitialized(VkmResourceHandle handle)
    {
        id<MTLAllocation> allocation = fetchAllocation(this, handle);
        if (allocation == nil)
        {
            // Deferred-creation or not-yet-bound external handle; nothing to register.
            return;
        }

        std::lock_guard<std::mutex> lock(_residencyMutex);
        [_residencySets[(uint8_t)handle.poolType] addAllocation:allocation];
        _residencyDirty = true;
    }

    void VkmRenderResourcePoolMetal::releaseResource(VkmResourceHandle handle)
    {
        // Remove the residency entry before the base class destroys the resource.
        id<MTLAllocation> allocation = fetchAllocation(this, handle);
        if (allocation != nil)
        {
            // The residency set retains staged allocations, so deferring the commit only
            // delays the Metal object's release until the next flush -- never a dangle.
            std::lock_guard<std::mutex> lock(_residencyMutex);
            [_residencySets[(uint8_t)handle.poolType] removeAllocation:allocation];
            _residencyDirty = true;
        }

        VkmRenderResourcePool::releaseResource(handle);
    }

    void VkmRenderResourcePoolMetal::commitPendingResidencyChanges()
    {
        std::lock_guard<std::mutex> lock(_residencyMutex);
        if (!_residencyDirty)
        {
            return;
        }

        for (id<MTLResidencySet> residencySet : _residencySets)
        {
            if (residencySet != nil)
            {
                [residencySet commit];
            }
        }
        _residencyDirty = false;
    }
} // namespace vkm
