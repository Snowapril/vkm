// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/renderer_common.h>

#include <vector>
#include <cstdint>

namespace vkm
{
    // Which bindless array a registered buffer belongs to. Each maps to a fixed binding
    // within the engine-global bindless resource set ("set 0"); every backend implements
    // the same convention (Vulkan: descriptor set, Metal: argument buffer, WebGPU:
    // mega-buffer emulation -- see each VkmBindlessResourceManager* implementation).
    enum class VkmBindlessArrayType : uint8_t
    {
        Texture     = 0, // set 0, binding 0 (sampled image array) -- reserved, unused so far
        Buffer      = 1, // set 0, binding 1 (storage buffer array) -- vertex-pulling data
        IndexBuffer = 2, // set 0, binding 2 (storage buffer array) -- index-pulling data
    };

    // Engine-global bindless binding convention. These constants are the single source of
    // truth shared by the shader compiler (vkm-compiler MSL/WGSL generation) and every
    // backend runtime; changing one side without the other breaks shader/runtime ABI.
    inline constexpr uint32_t kVkmBindlessTextureCapacity     = 4096; // set 0, binding 0
    inline constexpr uint32_t kVkmBindlessBufferCapacity      = 4096; // set 0, binding 1
    inline constexpr uint32_t kVkmBindlessIndexBufferCapacity = 4096; // set 0, binding 2

    // Push constants: 8 bytes {uint32 vertexBufferIndex, uint32 indexBufferIndex},
    // vertex stage, offset 0 (see VkmPipelineStateVulkan::createInner).
    inline constexpr uint32_t kVkmBindlessPushConstantSize = sizeof(uint32_t) * 2;

    // Metal (argument buffers Tier 2): [[buffer(0)]]/[[buffer(1)]] remain the vertex-stream
    // indices; the set-0 argument buffer and the push-constant buffer are pinned after them.
    // Within the Tier-2 argument buffer each resource occupies one 8-byte entry (GPU address
    // or MTLResourceID) at entry index = argument-buffer id; the three arrays are laid out
    // back-to-back: textures at ids [0, 4096), buffers at [4096, 8192), index buffers at
    // [8192, 12288).
    inline constexpr uint32_t kVkmMetalBindlessArgumentBufferIndex = 2;
    inline constexpr uint32_t kVkmMetalPushConstantBufferIndex     = 3;
    inline constexpr uint32_t kVkmMetalBindlessTextureIdBase     = 0;
    inline constexpr uint32_t kVkmMetalBindlessBufferIdBase      = kVkmBindlessTextureCapacity;
    inline constexpr uint32_t kVkmMetalBindlessIndexBufferIdBase = kVkmBindlessTextureCapacity + kVkmBindlessBufferCapacity;
    inline constexpr uint32_t kVkmMetalBindlessArgumentEntryCount =
        kVkmBindlessTextureCapacity + kVkmBindlessBufferCapacity + kVkmBindlessIndexBufferCapacity;

    // Fixed-capacity slot allocator for one bindless array: LIFO free-list plus a monotonic
    // high-water mark (a plain free-list, not VkmOffsetAllocator's byte-range coalescing,
    // which is the wrong shape for uniform-size array-element allocation).
    class VkmBindlessSlotAllocator
    {
    public:
        explicit VkmBindlessSlotAllocator(uint32_t capacity);

        // Pops the most recently released slot if any, otherwise bumps the high-water mark.
        // Returns UINT32_MAX if capacity is exhausted.
        uint32_t allocate();
        void release(uint32_t slot);

    private:
        uint32_t _capacity;
        uint32_t _nextSlot = 0;
        std::vector<uint32_t> _freeSlots;
    };

    // Backend-agnostic interface to the engine-global bindless resource set. Each backend
    // driver owns one implementation (see VkmDriverBase::getBindlessResourceManager()).
    class VkmBindlessResourceManagerBase
    {
    public:
        virtual ~VkmBindlessResourceManagerBase() = default;

        virtual void destroy() = 0;

        // Registers an existing storage buffer resource into the given bindless array and
        // publishes it at the returned slot. Returns UINT32_MAX if the array is exhausted.
        virtual uint32_t registerBuffer(VkmResourceHandle bufferHandle, VkmBindlessArrayType arrayType) = 0;
        virtual void unregisterBuffer(uint32_t slot, VkmBindlessArrayType arrayType) = 0;
    };
} // namespace vkm
