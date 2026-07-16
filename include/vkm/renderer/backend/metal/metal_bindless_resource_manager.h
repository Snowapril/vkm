// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/bindless_resource_manager.h>

#include <cstdint>

@protocol MTLBuffer;
@protocol MTL4ArgumentTable;

namespace vkm
{
    class VkmDriverMetal;

    // Metal implementation of the engine-global "set 0" bindless convention (see
    // common/bindless_resource_manager.h). Owns:
    //  - the Tier-2 argument buffer: one shared MTLBuffer of 8-byte entries where entry
    //    [id] holds a buffer's gpuAddress (texture MTLResourceIDs reserved at ids 0..4095),
    //    matching the [[id(N)]] layout vkm-compiler pins in the generated MSL;
    //  - the push-constant ring: a shared MTLBuffer sub-divided into fixed 256-byte
    //    entries handed out per setPushConstants() call;
    //  - the MTL4ArgumentTable every graphics encoder binds (argument buffer at
    //    [[buffer(2)]], push constants at [[buffer(3)]] -- single-threaded recording,
    //    the same assumption the ImGui Metal renderer already makes).
    class VkmBindlessResourceManagerMetal : public VkmBindlessResourceManagerBase
    {
    public:
        static constexpr uint32_t PUSH_CONSTANT_ENTRY_STRIDE = 256;
        static constexpr uint32_t PUSH_CONSTANT_ENTRY_COUNT  = 1024;

        explicit VkmBindlessResourceManagerMetal(VkmDriverMetal* driver);
        ~VkmBindlessResourceManagerMetal();

        bool initialize();
        void destroy() override final;

        uint32_t registerBuffer(VkmResourceHandle bufferHandle, VkmBindlessArrayType arrayType) override final;
        void unregisterBuffer(uint32_t slot, VkmBindlessArrayType arrayType) override final;

        // Copies `size` bytes into the next push-constant ring entry and returns that
        // entry's GPU address (to be bound at kVkmMetalPushConstantBufferIndex). The ring
        // wraps after PUSH_CONSTANT_ENTRY_COUNT allocations; entries are assumed retired
        // by then (logged if that assumption is at risk -- no per-frame reset hook exists).
        uint64_t allocatePushConstantSlot(const void* data, uint32_t size);

        inline id<MTL4ArgumentTable> getArgumentTable() const { return _argumentTable; }
        inline id<MTLBuffer> getArgumentBuffer() const { return _argumentBuffer; }

    private:
        VkmDriverMetal* _driver;

        id<MTLBuffer> _argumentBuffer = nullptr;      // kVkmMetalBindlessArgumentEntryCount x 8 bytes, shared storage
        id<MTLBuffer> _pushConstantRing = nullptr;    // PUSH_CONSTANT_ENTRY_COUNT x PUSH_CONSTANT_ENTRY_STRIDE, shared storage
        id<MTL4ArgumentTable> _argumentTable = nullptr;

        uint32_t _pushConstantCursor = 0;

        VkmBindlessSlotAllocator _textureSlots{kVkmBindlessTextureCapacity};
        VkmBindlessSlotAllocator _bufferSlots{kVkmBindlessBufferCapacity};
        VkmBindlessSlotAllocator _indexBufferSlots{kVkmBindlessIndexBufferCapacity};
    };
} // namespace vkm
