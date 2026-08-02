// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/bindless_resource_manager.h>

#include <cstdint>

@protocol MTLBuffer;
@protocol MTLSamplerState;
@protocol MTL4ArgumentTable;
struct MTLResourceID;

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
        static constexpr uint32_t PUSH_CONSTANT_ENTRY_COUNT  = kVkmPushConstantRingEntryCount;

        explicit VkmBindlessResourceManagerMetal(VkmDriverMetal* driver);
        ~VkmBindlessResourceManagerMetal();

        bool initialize();
        void destroy() override final;

        uint32_t registerBuffer(VkmResourceHandle bufferHandle, VkmBindlessArrayType arrayType) override final;
        void unregisterBuffer(uint32_t slot, VkmBindlessArrayType arrayType) override final;
        bool setSingletonBuffer(VkmBindlessSingletonBuffer which, VkmResourceHandle bufferHandle) override final;

        // Publishes the texture's MTLResourceID at the returned slot of the argument
        // buffer's texture range.
        uint32_t registerTexture(VkmResourceHandle textureHandle) override final;
        void unregisterTexture(uint32_t slot) override final;

        // Copies `size` bytes into the next push-constant ring entry and returns that
        // entry's GPU address (to be bound at kVkmMetalPushConstantBufferIndex). The ring
        // wraps after PUSH_CONSTANT_ENTRY_COUNT allocations; entries are assumed retired
        // by then (logged if that assumption is at risk -- no per-frame reset hook exists).
        uint64_t allocatePushConstantSlot(const void* data, uint32_t size);

        inline id<MTL4ArgumentTable> getArgumentTable() const { return _argumentTable; }
        inline id<MTLBuffer> getArgumentBuffer() const { return _argumentBuffer; }

    private:
        // Byte-wise write of an opaque MTLResourceID into one 8-byte argument-buffer entry.
        void writeResourceIdEntry(uint32_t entryIndex, MTLResourceID resourceId);

        VkmDriverMetal* _driver;

        id<MTLBuffer> _argumentBuffer = nullptr;      // kVkmMetalBindlessArgumentEntryCount x 8 bytes, shared storage
        id<MTLBuffer> _pushConstantRing = nullptr;    // PUSH_CONSTANT_ENTRY_COUNT x PUSH_CONSTANT_ENTRY_STRIDE, shared storage
        id<MTL4ArgumentTable> _argumentTable = nullptr;
        id<MTLSamplerState> _defaultSampler = nullptr; // published at kVkmMetalBindlessSamplerId

        uint32_t _pushConstantCursor = 0;

        VkmBindlessSlotAllocator _textureSlots{kVkmBindlessTextureCapacity};
        VkmBindlessSlotAllocator _bufferSlots{kVkmBindlessBufferCapacity};
        VkmBindlessSlotAllocator _indexBufferSlots{kVkmBindlessIndexBufferCapacity};
    };
} // namespace vkm
