// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/acceleration_structure.h>
#import <Metal/Metal.h>

namespace vkm
{
    class VkmDriverMetal;

    /*
    * @brief An `MTLAccelerationStructure` built through the Metal 4 descriptors.
    *
    * @details Metal allocates the structure itself, so unlike Vulkan there is no separate storage
    * buffer to own -- just the structure, its instance buffer, and (for an updatable one) its
    * scratch.
    *
    * Metal 4 rebuilt this API rather than renaming it, and two differences drive the shape here:
    * geometry is addressed by `MTL4BufferRange` (a GPU address plus a length) instead of
    * `id<MTLBuffer>`, and an instance names its bottom-level structure by `MTLResourceID` in the
    * descriptor buffer -- the `MTLAccelerationStructureInstanceDescriptorTypeIndirect` layout --
    * rather than indexing an `instancedAccelerationStructures` array. The second is what lets
    * `updateInstances` rewrite transforms without touching the descriptor.
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

        // Records a rebuild. Only valid on a structure created with `_allowUpdate`, which is what
        // kept its scratch alive; see the Vulkan counterpart for why this is a rebuild not a refit.
        void recordBuild(id<MTL4ComputeCommandEncoder> encoder);

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
