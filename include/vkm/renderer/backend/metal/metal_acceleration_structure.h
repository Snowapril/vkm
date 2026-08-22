// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/renderer/backend/common/acceleration_structure.h>
#import <Metal/Metal.h>

namespace vkm
{
    class VkmDriverMetal;

    /*
    * @brief An `MTLAccelerationStructure` built through the Metal 4 descriptors.
    * @details Metal allocates the structure itself, so unlike Vulkan there is no separate storage
    * buffer to own: just the structure, its instance buffer, and, for an updatable one, its scratch.
    * Two Metal 4 API traits drive the shape here. Geometry is addressed by `MTL4BufferRange`, a GPU
    * address plus a length, rather than `id<MTLBuffer>`; and an instance names its bottom-level
    * structure by `MTLResourceID` in the descriptor buffer -- the
    * `MTLAccelerationStructureInstanceDescriptorTypeIndirect` layout -- rather than indexing an
    * `instancedAccelerationStructures` array. The second lets `updateInstances` rewrite transforms
    * without touching the descriptor.
    */
    class VkmAccelerationStructureMetal : public VkmAccelerationStructure
    {
    public:
        explicit VkmAccelerationStructureMetal(VkmDriverBase* driver);
        ~VkmAccelerationStructureMetal() override;

        bool initialize(VkmResourceHandle handle, const VkmAccelerationStructureInfo& info) override final;
        bool updateInstances(const std::vector<VkmAccelerationStructureInstance>& instances) override final;
        uint64_t getAllocatedSize() const override final { return _structureSize; }
        void setDebugName(const char* name) override final;

        inline id<MTLAccelerationStructure> getAccelerationStructure() const { return _accelerationStructure; }

        // What a build needs to know about this structure, for the one caller that records one:
        // VkmCommandBufferMetal::onBuildAccelerationStructure. A nil scratch means the structure was
        // built without `_allowUpdate` and a rebuild must refuse -- Metal's debug layer aborts the
        // process on a nil scratch buffer rather than reporting it.
        inline MTL4AccelerationStructureDescriptor* getDescriptor() const { return _descriptor; }
        inline id<MTLBuffer> getScratchBuffer() const { return _scratchBuffer; }

    private:
        MTL4AccelerationStructureDescriptor* makeDescriptor(const VkmAccelerationStructureInfo& info);
        bool createInstanceBuffer(const VkmAccelerationStructureInfo& info);
        void releaseAll();

        VkmDriverMetal* _driverMetal = nullptr;
        id<MTLAccelerationStructure> _accelerationStructure = nil;
        id<MTLBuffer> _instanceBuffer = nil;
        id<MTLBuffer> _scratchBuffer = nil;
        // Retained: a rebuild is described by the same descriptor the structure was sized against,
        // only the instance buffer's contents differ.
        MTL4AccelerationStructureDescriptor* _descriptor = nil;
        uint32_t _instanceCapacity = 0;
        uint64_t _structureSize = 0;
        bool _allowUpdate = false;
    };
} // namespace vkm
