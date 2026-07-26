// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/bindless_resource_manager.h>
#include <vkm/renderer/backend/common/gpu_offset_allocator.h>
#include <webgpu/webgpu.h>

#include <array>
#include <cstdint>
#include <unordered_map>

namespace vkm
{
    class VkmDriverWebGPU;

    // WebGPU emulation of the engine-global "set 0" bindless convention (see
    // common/bindless_resource_manager.h). WGSL has no runtime-sized descriptor arrays and
    // no push constants, so instead of an array of buffers this manager owns:
    //  - one read-only-storage "mega-buffer" per bindless array (vertex data / index
    //    data); registerBuffer() copies the source buffer's contents into an allocated
    //    range and publishes the range's u32-WORD offset in the slot table;
    //  - the slot table (storage buffer of u32 word offsets: [slot] = vertex-pool offset,
    //    [kVkmBindlessBufferCapacity + slot] = index-pool offset);
    //  - the push-constant ring: a uniform buffer bound with a dynamic offset, advanced
    //    256 bytes per setPushConstants() call;
    //  - the singleton buffers of VkmBindlessSingletonBuffer, at bindings 4..6; because a
    //    WebGPU bind group is immutable, setSingletonBuffer() recreates the bind group;
    //  - the engine-global bind group 0 (layout + bind group) every pipeline layout uses.
    //
    // Both mega-buffers are typed as plain `array<u32>` and the slot table holds WORD offsets, so
    // a registered buffer only has to be a multiple of 4 bytes -- the same opaque byte-range
    // treatment Vulkan and Metal already give it. This is what lets one pool hold vertices of any
    // stride (see VkmVertexLayout); the shaders reassemble attributes from words.
    class VkmBindlessResourceManagerWebGPU : public VkmBindlessResourceManagerBase
    {
    public:
        static constexpr uint32_t ELEMENT_STRIDE = 4; // both mega-buffers are u32 word arrays
        static constexpr uint32_t VERTEX_MEGA_BUFFER_SIZE = 16 * 1024 * 1024;
        static constexpr uint32_t INDEX_MEGA_BUFFER_SIZE  = 8 * 1024 * 1024;
        static constexpr uint32_t PUSH_CONSTANT_ENTRY_STRIDE = 256; // WebGPU minUniformBufferOffsetAlignment default
        static constexpr uint32_t PUSH_CONSTANT_ENTRY_COUNT  = 1024;
        // b0..b3 are the push-constant ring, the two mega-buffers and the slot table.
        static constexpr uint32_t kFirstSingletonBinding = 4;
        // Enough for one u32; only ever read by a shader whose singleton was never published.
        static constexpr uint32_t kSingletonPlaceholderSize = 16;

        explicit VkmBindlessResourceManagerWebGPU(VkmDriverWebGPU* driver);
        ~VkmBindlessResourceManagerWebGPU();

        bool initialize();
        void destroy() override final;

        uint32_t registerBuffer(VkmResourceHandle bufferHandle, VkmBindlessArrayType arrayType) override final;
        void unregisterBuffer(uint32_t slot, VkmBindlessArrayType arrayType) override final;
        bool setSingletonBuffer(VkmBindlessSingletonBuffer which, VkmResourceHandle bufferHandle) override final;

        // Unsupported here: WGSL has no runtime-sized texture arrays, so this backend's
        // bind group models no texture array to publish into. Always returns UINT32_MAX.
        uint32_t registerTexture(VkmResourceHandle textureHandle) override final;
        void unregisterTexture(uint32_t slot) override final;

        // Writes `size` bytes into the next push-constant ring entry and returns its byte
        // offset, to be passed as the dynamic offset of bind group 0. The ring wraps after
        // PUSH_CONSTANT_ENTRY_COUNT allocations (see the Metal manager for the same caveat).
        uint32_t writePushConstants(const void* data, uint32_t size);

        inline WGPUBindGroupLayout getBindGroupLayout() const { return _bindGroupLayout; }
        inline WGPUBindGroup getBindGroup() const { return _bindGroup; }

    private:
        VkmDriverWebGPU* _driver;

        // Rebuilds _bindGroup from the current mega-buffers and singleton bindings. WebGPU bind
        // groups are immutable, so every singleton change goes through this.
        bool recreateBindGroup();

        WGPUBuffer _vertexMegaBuffer = nullptr;
        WGPUBuffer _indexMegaBuffer = nullptr;
        WGPUBuffer _slotTable = nullptr;
        WGPUBuffer _pushConstantRing = nullptr;
        WGPUBindGroupLayout _bindGroupLayout = nullptr;
        WGPUBindGroup _bindGroup = nullptr;
        uint64_t _slotTableSize = 0;

        /*
        * Buffers published at the fixed singleton bindings. A WebGPU bind group must supply an
        * entry for every entry in its layout, so an unbound singleton binds _singletonPlaceholder
        * instead of being omitted; a shader that reads it sees zeroes rather than failing
        * validation.
        */
        WGPUBuffer _singletonPlaceholder = nullptr;
        std::array<WGPUBuffer, static_cast<size_t>(VkmBindlessSingletonBuffer::Count)> _singletonBuffers{};
        std::array<uint64_t, static_cast<size_t>(VkmBindlessSingletonBuffer::Count)> _singletonSizes{};

        uint32_t _pushConstantCursor = 0;

        VkmBindlessSlotAllocator _bufferSlots{kVkmBindlessBufferCapacity};
        VkmBindlessSlotAllocator _indexBufferSlots{kVkmBindlessIndexBufferCapacity};

        VkmOffsetAllocator _vertexMegaAllocator{VERTEX_MEGA_BUFFER_SIZE};
        VkmOffsetAllocator _indexMegaAllocator{INDEX_MEGA_BUFFER_SIZE};

        // Mega-buffer range owned by each live slot, keyed per array type, so
        // unregisterBuffer() can return the range to its allocator.
        std::unordered_map<uint32_t, VkmGpuMemoryAllocation> _vertexSlotRanges;
        std::unordered_map<uint32_t, VkmGpuMemoryAllocation> _indexSlotRanges;
    };
} // namespace vkm
