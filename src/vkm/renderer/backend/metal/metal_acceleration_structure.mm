// Copyright (c) 2026 Snowapril

#include <vkm/renderer/backend/metal/metal_acceleration_structure.h>
#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/metal/metal_buffer.h>
#include <vkm/renderer/backend/common/buffer_view.h>
#include <vkm/renderer/backend/metal/metal_command_buffer.h>
#include <vkm/renderer/backend/common/command_queue.h>
#include <vkm/renderer/backend/metal/metal_render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>

namespace vkm
{
    namespace
    {
        // A geometry range as Metal 4 wants it: an address that already includes the offset, and a
        // length so validation can catch a range running off the end of its buffer.
        // The range a build should read a geometry view from. The view's offset is relative to its
        // parent, which is what MTLBuffer.gpuAddress already points at.
        MTL4BufferRange viewRange(VkmRenderResourcePool* pool, VkmResourceHandle viewHandle)
        {
            VkmBufferView* view = pool->getResource<VkmBufferView>(viewHandle);
            if (view == nullptr)
            {
                return MTL4BufferRange{ 0, 0 };
            }
            VkmBuffer* parent = view->tryGetParent();
            if (parent == nullptr)
            {
                return MTL4BufferRange{ 0, 0 };
            }
            const uint64_t offset = view->getBufferViewInfo()._offset;
            id<MTLBuffer> mtlBuffer = static_cast<VkmBufferMetal*>(parent)->getBuffer();
            return MTL4BufferRange{ mtlBuffer.gpuAddress + offset, mtlBuffer.length - offset };
        }

        // glm is column-major and so is MTLPackedFloat4x3, so this drops the projective row rather
        // than transposing -- unlike the Vulkan side, whose VkTransformMatrixKHR is row-major.
        MTLPackedFloat4x3 packTransform(const glm::mat4& transform)
        {
            MTLPackedFloat4x3 packed{};
            for (uint32_t column = 0; column < 4; ++column)
            {
                packed.columns[column].x = transform[column][0];
                packed.columns[column].y = transform[column][1];
                packed.columns[column].z = transform[column][2];
            }
            return packed;
        }

        MTLIndirectAccelerationStructureInstanceDescriptor makeInstanceDescriptor(
            const VkmAccelerationStructureInstance& source, id<MTLAccelerationStructure> blas)
        {
            MTLIndirectAccelerationStructureInstanceDescriptor descriptor{};
            descriptor.transformationMatrix = packTransform(source._transform);
            // Opaque for the same reason Vulkan passes VK_GEOMETRY_OPAQUE_BIT_KHR: the engine has
            // no alpha-tested ray path, and a non-opaque instance without one accepts every hit.
            descriptor.options = MTLAccelerationStructureInstanceOptionOpaque |
                                 MTLAccelerationStructureInstanceOptionDisableTriangleCulling;
            descriptor.mask = 0xFF;
            descriptor.intersectionFunctionTableOffset = 0;
            descriptor.userID = source._instanceId;
            descriptor.accelerationStructureID = blas.gpuResourceID;
            return descriptor;
        }
    } // namespace

    VkmAccelerationStructureMetal::VkmAccelerationStructureMetal(VkmDriverBase* driver)
        : VkmAccelerationStructure(driver), _driverMetal(static_cast<VkmDriverMetal*>(driver))
    {
    }

    VkmAccelerationStructureMetal::~VkmAccelerationStructureMetal()
    {
        releaseAll();
    }

    bool VkmAccelerationStructureMetal::createInstanceBuffer(const VkmAccelerationStructureInfo& info)
    {
        if (info._instances.empty())
        {
            return true; // an empty top-level structure is valid; there is nothing to upload
        }

        std::vector<MTLIndirectAccelerationStructureInstanceDescriptor> descriptors;
        descriptors.reserve(info._instances.size());
        for (const VkmAccelerationStructureInstance& source : info._instances)
        {
            VkmAccelerationStructure* blas =
                _driverMetal->getRenderResourcePool()->getResource<VkmAccelerationStructure>(source._blas);
            if (blas == nullptr)
            {
                VKM_DEBUG_ERROR("Top-level acceleration structure instance references an invalid bottom-level handle");
                return false;
            }
            descriptors.push_back(makeInstanceDescriptor(
                source, static_cast<VkmAccelerationStructureMetal*>(blas)->getAccelerationStructure()));
        }

        const NSUInteger byteSize =
            descriptors.size() * sizeof(MTLIndirectAccelerationStructureInstanceDescriptor);
        // Shared so updateInstances can rewrite it from the CPU without a staging copy, which is
        // the whole point of a per-frame rebuild.
        _instanceBuffer = [_driverMetal->getMTLDevice() newBufferWithBytes:descriptors.data()
                                                                    length:byteSize
                                                                   options:MTLResourceStorageModeShared];
        if (_instanceBuffer == nil)
        {
            VKM_DEBUG_ERROR("Failed to create the acceleration structure instance buffer");
            return false;
        }
        _instanceCapacity = static_cast<uint32_t>(descriptors.size());
        // Registered explicitly, for the reason the bindless manager registers its own two
        // buffers: this does not go through newBuffer(), so nothing else ever makes it resident.
        // A build reads it on the GPU, so without this the top-level structure is built over
        // whatever memory happens to be mapped -- which is why the failure depended on what the
        // process had allocated earlier rather than on anything about the scene.
        static_cast<VkmRenderResourcePoolMetal*>(_driverMetal->getRenderResourcePool())
            ->registerExternalAllocation(_instanceBuffer);
        return true;
    }

    MTL4AccelerationStructureDescriptor* VkmAccelerationStructureMetal::makeDescriptor(
        const VkmAccelerationStructureInfo& info)
    {
        VkmRenderResourcePool* pool = _driverMetal->getRenderResourcePool();

        if (info._type == VkmAccelerationStructureType::TopLevel)
        {
            if (!createInstanceBuffer(info))
            {
                return nil;
            }
            MTL4InstanceAccelerationStructureDescriptor* descriptor =
                [[MTL4InstanceAccelerationStructureDescriptor alloc] init];
            // Indirect, so each instance names its bottom-level structure by MTLResourceID in the
            // buffer. The alternative layouts index an array the descriptor holds, which would
            // make updateInstances have to rebuild the descriptor rather than rewrite a buffer.
            descriptor.instanceDescriptorType = MTLAccelerationStructureInstanceDescriptorTypeIndirect;
            descriptor.instanceDescriptorStride = sizeof(MTLIndirectAccelerationStructureInstanceDescriptor);
            descriptor.instanceCount = info._instances.size();
            if (_instanceBuffer != nil)
            {
                descriptor.instanceDescriptorBuffer =
                    MTL4BufferRange{ _instanceBuffer.gpuAddress, _instanceBuffer.length };
            }
            return descriptor;
        }

        NSMutableArray<MTL4AccelerationStructureGeometryDescriptor*>* geometries =
            [[NSMutableArray alloc] initWithCapacity:info._geometries.size()];
        for (const VkmAccelerationStructureGeometry& source : info._geometries)
        {
            if (source._indexCount == 0 || (source._indexCount % 3) != 0)
            {
                VKM_DEBUG_ERROR("Acceleration structure geometry needs a non-zero index count that is a multiple of 3");
                [geometries release];
                return nil;
            }
            const MTL4BufferRange vertexRange = viewRange(pool, source._vertexView);
            const MTL4BufferRange indexRange = viewRange(pool, source._indexView);
            if (vertexRange.bufferAddress == 0 || indexRange.bufferAddress == 0)
            {
                VKM_DEBUG_ERROR("Acceleration structure geometry references a buffer that could not be resolved");
                [geometries release];
                return nil;
            }

            MTL4AccelerationStructureTriangleGeometryDescriptor* geometry =
                [[[MTL4AccelerationStructureTriangleGeometryDescriptor alloc] init] autorelease];
            geometry.vertexBuffer = vertexRange;
            geometry.vertexFormat = MTLAttributeFormatFloat3;
            geometry.vertexStride = source._vertexStride;
            geometry.indexBuffer = indexRange;
            geometry.indexType = MTLIndexTypeUInt32;
            geometry.triangleCount = source._indexCount / 3;
            geometry.opaque = YES;
            [geometries addObject:geometry];
        }

        MTL4PrimitiveAccelerationStructureDescriptor* descriptor =
            [[MTL4PrimitiveAccelerationStructureDescriptor alloc] init];
        descriptor.geometryDescriptors = geometries;
        [geometries release];
        return descriptor;
    }

    bool VkmAccelerationStructureMetal::initialize(VkmResourceHandle handle,
                                                   const VkmAccelerationStructureInfo& info)
    {
        if (!initializeAccelerationStructureCommon(handle, info))
        {
            return false;
        }
        _allowUpdate = info._allowUpdate;

        /*
        * Owned outright, not autoreleased: a rebuild is described by this same descriptor, so it
        * has to outlive initialize() by the structure's whole lifetime. These sources are compiled
        * without ARC, and releaseAll() is what balances it.
        */
        _descriptor = makeDescriptor(info);
        if (_descriptor == nil)
        {
            return false;
        }

        id<MTLDevice> device = _driverMetal->getMTLDevice();
        const MTLAccelerationStructureSizes sizes = [device accelerationStructureSizesWithDescriptor:_descriptor];
        _accelerationStructure = [device newAccelerationStructureWithSize:sizes.accelerationStructureSize];
        if (_accelerationStructure == nil)
        {
            VKM_DEBUG_ERROR("Failed to create the acceleration structure");
            return false;
        }
        _structureSize = sizes.accelerationStructureSize;
        /*
        * Made resident BEFORE the build, not after. VkmDriverBase::newAccelerationStructure calls
        * onResourceInitialized() -- what normally adds a resource to the pool's residency set --
        * only once this initialize() has returned, so the build below would otherwise write into a
        * structure that is not resident, and a top-level build would read bottom-level ones that
        * are not either. That traverses to zero hits, with nothing reported by Metal.
        * Registering here is idempotent with the later onResourceInitialized(), which finds it
        * already present and does nothing.
        */
        static_cast<VkmRenderResourcePoolMetal*>(_driverMetal->getRenderResourcePool())
            ->registerExternalAllocation(_accelerationStructure);

        if (sizes.buildScratchBufferSize > 0)
        {
            _scratchBuffer = [device newBufferWithLength:sizes.buildScratchBufferSize
                                                 options:MTLResourceStorageModePrivate];
            if (_scratchBuffer == nil)
            {
                VKM_DEBUG_ERROR("Failed to create the acceleration structure scratch buffer");
                return false;
            }
            // Private storage and raw-allocated, so nothing else makes it resident either.
            static_cast<VkmRenderResourcePoolMetal*>(_driverMetal->getRenderResourcePool())
                ->registerExternalAllocation(_scratchBuffer);
        }

        /*
        * Recorded through the ordinary command-buffer entry point rather than by reaching for the
        * encoder here, and submitted through the engine's queue -- the shape
        * VkmDriverBase::uploadToBuffer uses. `buildAccelerationStructure` rather than a hand-rolled
        * encoder block, so the initial build and the per-frame rebuild are literally the same path.
        */
        VkmCommandQueueBase* commandQueue = _driverMetal->getCommandQueue(VkmCommandQueueType::Graphics, 0);
        VkmCommandBufferBase* commandBuffer = commandQueue->getCommandBufferPool()->allocate();
        commandBuffer->beginCommandBuffer();
        commandBuffer->buildAccelerationStructure(handle);
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

        if (!_allowUpdate)
        {
            // A structure that will never be rebuilt has no further use for its scratch, and it is
            // the largest allocation here on a big scene.
            [_scratchBuffer release];
            _scratchBuffer = nil;
        }
        return true;
    }

    bool VkmAccelerationStructureMetal::updateInstances(
        const std::vector<VkmAccelerationStructureInstance>& instances)
    {
        if (_info._type != VkmAccelerationStructureType::TopLevel)
        {
            VKM_DEBUG_ERROR("updateInstances is only valid on a top-level acceleration structure");
            return false;
        }
        if (_instanceBuffer == nil)
        {
            VKM_DEBUG_ERROR("updateInstances on a structure created with no instances");
            return false;
        }
        if (instances.size() > _instanceCapacity)
        {
            VKM_DEBUG_ERROR("updateInstances was given more instances than the structure was created with");
            return false;
        }

        VkmRenderResourcePool* pool = _driverMetal->getRenderResourcePool();
        auto* destination = static_cast<MTLIndirectAccelerationStructureInstanceDescriptor*>(_instanceBuffer.contents);
        for (size_t i = 0; i < instances.size(); ++i)
        {
            VkmAccelerationStructure* blas = pool->getResource<VkmAccelerationStructure>(instances[i]._blas);
            if (blas == nullptr)
            {
                VKM_DEBUG_ERROR("updateInstances references an invalid bottom-level handle");
                return false;
            }
            destination[i] = makeInstanceDescriptor(
                instances[i], static_cast<VkmAccelerationStructureMetal*>(blas)->getAccelerationStructure());
        }

        // A shorter list has to shrink the descriptor's count too, or the rebuild reads stale
        // descriptors past what was written. The Vulkan side does the same to its primitive count.
        if ([_descriptor isKindOfClass:[MTL4InstanceAccelerationStructureDescriptor class]])
        {
            static_cast<MTL4InstanceAccelerationStructureDescriptor*>(_descriptor).instanceCount = instances.size();
        }
        // Keep the retained info matching what the next build reads, so the command buffer's
        // GPU-usage walk and the debug inspector see the rebuilt structure, not the created one.
        _info._instances = instances;
        return true;
    }

    void VkmAccelerationStructureMetal::setDebugName(const char* name)
    {
        if (_accelerationStructure != nil && name != nullptr)
        {
            _accelerationStructure.label = [NSString stringWithUTF8String:name];
        }
    }

    void VkmAccelerationStructureMetal::releaseAll()
    {
        VkmRenderResourcePoolMetal* pool =
            static_cast<VkmRenderResourcePoolMetal*>(_driverMetal->getRenderResourcePool());
        if (pool != nullptr)
        {
            pool->unregisterExternalAllocation(_instanceBuffer);
            pool->unregisterExternalAllocation(_scratchBuffer);
            // Idempotent with VkmRenderResourcePoolMetal::releaseResource, which removes the
            // structure again on its way out; the second erase finds nothing and does nothing.
            pool->unregisterExternalAllocation(_accelerationStructure);
        }
        [_accelerationStructure release];
        _accelerationStructure = nil;
        [_instanceBuffer release];
        _instanceBuffer = nil;
        [_scratchBuffer release];
        _scratchBuffer = nil;
        [_descriptor release];
        _descriptor = nil;
        _instanceCapacity = 0;
        _structureSize = 0;
    }
} // namespace vkm
