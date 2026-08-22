// Copyright (c) 2026 Snowapril

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

    /*
    * @brief Metal implementation of the engine-global "set 0" bindless convention (see
    * common/bindless_resource_manager.h).
    * @details Owns:
    *  - the Tier-2 argument buffer, one shared MTLBuffer of 8-byte entries where entry [id] holds a
    *    buffer's gpuAddress, texture MTLResourceIDs being reserved at ids 0..4095, matching the
    *    [[id(N)]] layout vkm-compiler pins in the generated MSL;
    *  - the push-constant ring, a shared MTLBuffer sub-divided into fixed 256-byte entries handed
    *    out per setPushConstants() call;
    *  - the MTL4ArgumentTable every graphics encoder binds, argument buffer at [[buffer(2)]] and
    *    push constants at [[buffer(3)]]. Recording is single-threaded, the same assumption the
    *    ImGui Metal renderer makes.
    */
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
        bool setAccelerationStructure(VkmResourceHandle accelerationStructureHandle) override final;

        // Publishes the texture's MTLResourceID at the returned slot of the argument
        // buffer's texture range.
        uint32_t registerTexture(VkmResourceHandle textureHandle) override final;
        void unregisterTexture(uint32_t slot) override final;

        /*
        * @brief Copies bytes into the next push-constant ring entry.
        * @details Entries come from the current frame slot's region, and the cursor wraps within it
        * after PUSH_CONSTANT_ENTRY_COUNT allocations, which would reuse entries this same frame
        * still references. That wrap is logged as the overflow it is.
        * @param data Source bytes.
        * @param size Number of bytes to copy.
        * @return The entry's GPU address, to be bound at kVkmMetalPushConstantBufferIndex.
        */
        uint64_t allocatePushConstantSlot(const void* data, uint32_t size);

        void beginFrame(uint32_t frameSlot) override final;

        inline id<MTL4ArgumentTable> getArgumentTable() const { return _argumentTable; }
        inline id<MTLBuffer> getArgumentBuffer() const { return _argumentBuffer; }

    private:
        // Byte-wise write of an opaque MTLResourceID into one 8-byte argument-buffer entry.
        void writeResourceIdEntry(uint32_t entryIndex, MTLResourceID resourceId);

        VkmDriverMetal* _driver;

        id<MTLBuffer> _argumentBuffer = nullptr;  // kVkmMetalBindlessArgumentEntryCount x 8 bytes, shared storage
        // kVkmPushConstantRingTotalEntryCount x PUSH_CONSTANT_ENTRY_STRIDE, shared storage:
        // FRAME_BUFFER_COUNT regions of PUSH_CONSTANT_ENTRY_COUNT entries each.
        id<MTLBuffer> _pushConstantRing = nullptr;
        id<MTL4ArgumentTable> _argumentTable = nullptr;
        id<MTLSamplerState> _defaultSampler = nullptr;  // published at kVkmMetalBindlessSamplerId

        VkmPushConstantRingAllocator _pushConstantEntries;

        VkmBindlessSlotAllocator _textureSlots{kVkmBindlessTextureCapacity};
        VkmBindlessSlotAllocator _bufferSlots{kVkmBindlessBufferCapacity};
        VkmBindlessSlotAllocator _indexBufferSlots{kVkmBindlessIndexBufferCapacity};
    };
} // namespace vkm
