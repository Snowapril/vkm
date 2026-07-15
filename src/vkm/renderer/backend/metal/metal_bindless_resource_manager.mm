// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_bindless_resource_manager.h>
#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/metal/metal_buffer.h>
#include <vkm/renderer/backend/metal/metal_render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>

#import <Metal/MTLDevice.h>
#import <Metal/MTLBuffer.h>
#import <Metal/MTL4ArgumentTable.h>

#include <cstring>

namespace vkm
{
    VkmBindlessResourceManagerMetal::VkmBindlessResourceManagerMetal(VkmDriverMetal* driver)
        : _driver(driver)
    {
    }

    VkmBindlessResourceManagerMetal::~VkmBindlessResourceManagerMetal()
    {
    }

    bool VkmBindlessResourceManagerMetal::initialize()
    {
        id<MTLDevice> device = _driver->getMTLDevice();

        const uint64_t argumentBufferSize =
            static_cast<uint64_t>(kVkmMetalBindlessArgumentEntryCount) * sizeof(uint64_t);
        _argumentBuffer = [device newBufferWithLength:argumentBufferSize
                                              options:MTLResourceStorageModeShared];
        if (_argumentBuffer == nil)
        {
            VKM_DEBUG_ERROR("Failed to create bindless argument buffer");
            return false;
        }
        _argumentBuffer.label = @"VkmBindlessArgumentBuffer";
        std::memset(_argumentBuffer.contents, 0, argumentBufferSize);

        _pushConstantRing = [device newBufferWithLength:PUSH_CONSTANT_ENTRY_COUNT * PUSH_CONSTANT_ENTRY_STRIDE
                                                options:MTLResourceStorageModeShared];
        if (_pushConstantRing == nil)
        {
            VKM_DEBUG_ERROR("Failed to create push-constant ring buffer");
            return false;
        }
        _pushConstantRing.label = @"VkmPushConstantRing";

        // Neither buffer goes through newBuffer(), so neither joins a residency set on its
        // own; register them explicitly (same pattern as heap pool blocks in
        // VkmDriverMetal::allocateFromHeapPool).
        VkmRenderResourcePoolMetal* renderResourcePoolMetal =
            static_cast<VkmRenderResourcePoolMetal*>(_driver->getRenderResourcePool());
        renderResourcePoolMetal->registerExternalAllocation(_argumentBuffer);
        renderResourcePoolMetal->registerExternalAllocation(_pushConstantRing);

        MTL4ArgumentTableDescriptor* argTableDesc = [[MTL4ArgumentTableDescriptor alloc] init];
        // Indices 0/1 stay reserved for the vertex-stream buffers; 2 = bindless argument
        // buffer, 3 = push constants (see common/bindless_resource_manager.h).
        argTableDesc.maxBufferBindCount = kVkmMetalPushConstantBufferIndex + 1;
        argTableDesc.label = @"VkmBindlessArgumentTable";
        NSError* error = nil;
        _argumentTable = [device newArgumentTableWithDescriptor:argTableDesc error:&error];
        [argTableDesc release];
        if (_argumentTable == nil)
        {
            VKM_DEBUG_ERROR("Failed to create bindless argument table");
            return false;
        }

        return true;
    }

    void VkmBindlessResourceManagerMetal::destroy()
    {
        if (_argumentTable != nil)
        {
            [_argumentTable release];
            _argumentTable = nil;
        }
        if (_pushConstantRing != nil)
        {
            [_pushConstantRing release];
            _pushConstantRing = nil;
        }
        if (_argumentBuffer != nil)
        {
            [_argumentBuffer release];
            _argumentBuffer = nil;
        }
    }

    uint32_t VkmBindlessResourceManagerMetal::registerBuffer(VkmResourceHandle bufferHandle, VkmBindlessArrayType arrayType)
    {
        VKM_ASSERT(arrayType == VkmBindlessArrayType::Buffer || arrayType == VkmBindlessArrayType::IndexBuffer,
            "registerBuffer only accepts Buffer or IndexBuffer array types");

        VkmBindlessSlotAllocator& slots =
            (arrayType == VkmBindlessArrayType::Buffer) ? _bufferSlots : _indexBufferSlots;
        const uint32_t slot = slots.allocate();
        if (slot == UINT32_MAX)
        {
            VKM_DEBUG_ERROR("Bindless buffer array is exhausted");
            return UINT32_MAX;
        }

        VkmBufferMetal* bufferMetal = static_cast<VkmBufferMetal*>(
            _driver->getRenderResourcePool()->getResource<VkmBuffer>(bufferHandle));

        const uint32_t idBase = (arrayType == VkmBindlessArrayType::Buffer)
            ? kVkmMetalBindlessBufferIdBase
            : kVkmMetalBindlessIndexBufferIdBase;
        // Tier-2 argument buffers hold one 8-byte GPU address per [[id(N)]] entry; writing
        // through shared-storage contents is the Metal analog of Vulkan's update-after-bind
        // descriptor write (setup-time only, same in-flight caveat).
        uint64_t* entries = static_cast<uint64_t*>(_argumentBuffer.contents);
        entries[idBase + slot] = bufferMetal->getBuffer().gpuAddress;

        return slot;
    }

    void VkmBindlessResourceManagerMetal::unregisterBuffer(uint32_t slot, VkmBindlessArrayType arrayType)
    {
        uint32_t idBase = 0;
        switch (arrayType)
        {
            case VkmBindlessArrayType::Buffer:
                idBase = kVkmMetalBindlessBufferIdBase;
                _bufferSlots.release(slot);
                break;
            case VkmBindlessArrayType::IndexBuffer:
                idBase = kVkmMetalBindlessIndexBufferIdBase;
                _indexBufferSlots.release(slot);
                break;
            case VkmBindlessArrayType::Texture:
                idBase = kVkmMetalBindlessTextureIdBase;
                _textureSlots.release(slot);
                break;
        }
        static_cast<uint64_t*>(_argumentBuffer.contents)[idBase + slot] = 0;
    }

    uint64_t VkmBindlessResourceManagerMetal::allocatePushConstantSlot(const void* data, uint32_t size)
    {
        VKM_ASSERT(size <= PUSH_CONSTANT_ENTRY_STRIDE, "Push constant data exceeds ring entry stride");

        if (_pushConstantCursor != 0 && (_pushConstantCursor % PUSH_CONSTANT_ENTRY_COUNT) == 0)
        {
            // Wrapping reuses entry 0's storage; safe only if the GPU already consumed the
            // draw that referenced it PUSH_CONSTANT_ENTRY_COUNT allocations ago.
            VKM_DEBUG_WARN("Push-constant ring wrapped; entries older than 1024 allocations are being reused");
        }

        const uint32_t entryIndex = _pushConstantCursor % PUSH_CONSTANT_ENTRY_COUNT;
        _pushConstantCursor++;

        const uint64_t byteOffset = static_cast<uint64_t>(entryIndex) * PUSH_CONSTANT_ENTRY_STRIDE;
        std::memcpy(static_cast<uint8_t*>(_pushConstantRing.contents) + byteOffset, data, size);
        return _pushConstantRing.gpuAddress + byteOffset;
    }
} // namespace vkm
