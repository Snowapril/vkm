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

    /*
    * @brief WebGPU emulation of the engine-global "set 0" bindless convention (see
    * common/bindless_resource_manager.h).
    * @details WGSL has no runtime-sized descriptor arrays and no push constants, so instead of an
    * array of buffers this manager owns:
    *  - one read-only-storage "mega-buffer" per bindless array, vertex data and index data;
    *    registerBuffer() copies the source buffer into an allocated range and publishes the
    *    range's u32-WORD offset in the slot table;
    *  - the slot table, a storage buffer of u32 word offsets where [slot] is a vertex-pool offset
    *    and [kVkmBindlessBufferCapacity + slot] an index-pool offset;
    *  - the push-constant ring, a uniform buffer bound with a dynamic offset, advanced 256 bytes
    *    per setPushConstants() call;
    *  - the singleton buffers of VkmBindlessSingletonBuffer, at bindings 4..6. A WebGPU bind group
    *    is immutable, so setSingletonBuffer() recreates the bind group;
    *  - the engine-global bind group 0, layout and bind group, every pipeline layout uses.
    * Both mega-buffers are typed as plain `array<u32>` and the slot table holds WORD offsets, so a
    * registered buffer only has to be a multiple of 4 bytes -- the same opaque byte-range treatment
    * Vulkan and Metal give it. That lets one pool hold vertices of any stride, the shaders
    * reassembling attributes from words.
    */
    class VkmBindlessResourceManagerWebGPU : public VkmBindlessResourceManagerBase
    {
    public:
        static constexpr uint32_t ELEMENT_STRIDE = 4; // both mega-buffers are u32 word arrays
        static constexpr uint32_t VERTEX_MEGA_BUFFER_SIZE = 16 * 1024 * 1024;
        static constexpr uint32_t INDEX_MEGA_BUFFER_SIZE  = 8 * 1024 * 1024;
        static constexpr uint32_t PUSH_CONSTANT_ENTRY_STRIDE = 256; // WebGPU minUniformBufferOffsetAlignment default
        static constexpr uint32_t PUSH_CONSTANT_ENTRY_COUNT  = kVkmPushConstantRingEntryCount;
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
        // WebGPU has no acceleration structure API, so this logs and fails rather than being a
        // silent no-op -- the same shape registerTexture() takes here.
        bool setAccelerationStructure(VkmResourceHandle accelerationStructureHandle) override final;

        // Unsupported here: WGSL has no runtime-sized texture arrays, so this backend's bind group
        // models no texture array to publish into. Always returns UINT32_MAX.
        uint32_t registerTexture(VkmResourceHandle textureHandle) override final;
        void unregisterTexture(uint32_t slot) override final;

        /*
        * @brief Writes into the next push-constant ring entry.
        * @details Entries come from the current frame slot's region, and the cursor wraps within it
        * after PUSH_CONSTANT_ENTRY_COUNT allocations.
        * @param data Source bytes.
        * @param size Number of bytes to write.
        * @return The entry's byte offset, to be passed as the dynamic offset of bind group 0.
        */
        uint32_t writePushConstants(const void* data, uint32_t size);

        void beginFrame(uint32_t frameSlot) override final;

        /*
        * @brief Set 0's layout and bind group, in the two shapes a WebGPU pipeline can want.
        * @details The graphics shape omits the read-write singletons, IndirectArgument and
        * VisibleList. WebGPU forbids a buffer from carrying a writable usage and any other usage in
        * one synchronization scope, and a render pass that binds set 0 while fetching
        * `drawIndirect` arguments out of the same buffer is exactly that. Omitting the entries
        * takes them out of the pass's usage scope; not declaring them in the shader is not enough,
        * the scope covering the bind group rather than the shader. No render pipeline may declare
        * those two anyway, so the graphics shape loses nothing.
        * @param compute True for the compute shape, false for the graphics one.
        */
        inline WGPUBindGroupLayout getBindGroupLayout(bool compute) const
        {
            return compute ? _bindGroupLayout : _graphicsBindGroupLayout;
        }
        inline WGPUBindGroup getBindGroup(bool compute) const
        {
            return compute ? _bindGroup : _graphicsBindGroup;
        }

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
        // The same bindings minus the two read-write singletons; see getBindGroupLayout().
        WGPUBindGroupLayout _graphicsBindGroupLayout = nullptr;
        WGPUBindGroup _graphicsBindGroup = nullptr;
        uint64_t _slotTableSize = 0;

        /*
        * Buffers published at the fixed singleton bindings. A WebGPU bind group must supply an
        * entry for every entry in its layout, so an unbound singleton binds its placeholder rather
        * than being omitted, and a shader that reads it sees zeroes instead of failing validation.
        * One placeholder per singleton, not one shared between them: WebGPU rejects a bind group
        * whose writable storage bindings overlap, and separately rejects a buffer used both
        * read-write and read-only within one synchronization scope, and these bindings are a mix of
        * both kinds. Distinct slices of one buffer satisfy the first constraint but not the second.
        * Each placeholder is tiny and lives for the manager's lifetime.
        */
        std::array<WGPUBuffer, static_cast<size_t>(VkmBindlessSingletonBuffer::Count)> _singletonPlaceholders{};
        std::array<WGPUBuffer, static_cast<size_t>(VkmBindlessSingletonBuffer::Count)> _singletonBuffers{};
        std::array<uint64_t, static_cast<size_t>(VkmBindlessSingletonBuffer::Count)> _singletonSizes{};

        // The ring holds kVkmPushConstantRingTotalEntryCount entries: one region per frame slot.
        VkmPushConstantRingAllocator _pushConstantEntries;

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
