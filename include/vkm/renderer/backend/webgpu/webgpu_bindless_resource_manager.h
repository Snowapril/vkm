// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/bindless_resource_manager.h>
#include <vkm/renderer/backend/common/gpu_offset_allocator.h>
#include <webgpu/webgpu.h>

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
    //    range and publishes the range's ELEMENT offset in the slot table;
    //  - the slot table (storage buffer of u32 element offsets: [slot] = vertex-buffer
    //    offset in VertexData strides, [kVkmBindlessBufferCapacity + slot] = index-buffer
    //    offset in uint strides);
    //  - the push-constant ring: a uniform buffer bound with a dynamic offset, advanced
    //    256 bytes per setPushConstants() call;
    //  - the engine-global bind group 0 (layout + bind group) every pipeline layout uses.
    //
    // Consequence of the typed mega-buffers (deviation from Vulkan's opaque byte-range
    // treatment, logged in implementation-notes.md): registered Buffer-array buffers must
    // be tightly packed 32-byte engine VertexData elements and IndexBuffer-array buffers
    // tightly packed u32s -- registerBuffer() enforces this.
    class VkmBindlessResourceManagerWebGPU : public VkmBindlessResourceManagerBase
    {
    public:
        static constexpr uint32_t VERTEX_ELEMENT_STRIDE = 32; // sizeof triangle-convention VertexData (float3+pad, float4)
        static constexpr uint32_t INDEX_ELEMENT_STRIDE  = 4;  // sizeof uint
        static constexpr uint32_t VERTEX_MEGA_BUFFER_SIZE = 16 * 1024 * 1024;
        static constexpr uint32_t INDEX_MEGA_BUFFER_SIZE  = 8 * 1024 * 1024;
        static constexpr uint32_t PUSH_CONSTANT_ENTRY_STRIDE = 256; // WebGPU minUniformBufferOffsetAlignment default
        static constexpr uint32_t PUSH_CONSTANT_ENTRY_COUNT  = 1024;

        explicit VkmBindlessResourceManagerWebGPU(VkmDriverWebGPU* driver);
        ~VkmBindlessResourceManagerWebGPU();

        bool initialize();
        void destroy() override final;

        uint32_t registerBuffer(VkmResourceHandle bufferHandle, VkmBindlessArrayType arrayType) override final;
        void unregisterBuffer(uint32_t slot, VkmBindlessArrayType arrayType) override final;

        // Writes `size` bytes into the next push-constant ring entry and returns its byte
        // offset, to be passed as the dynamic offset of bind group 0. The ring wraps after
        // PUSH_CONSTANT_ENTRY_COUNT allocations (see the Metal manager for the same caveat).
        uint32_t writePushConstants(const void* data, uint32_t size);

        inline WGPUBindGroupLayout getBindGroupLayout() const { return _bindGroupLayout; }
        inline WGPUBindGroup getBindGroup() const { return _bindGroup; }

    private:
        VkmDriverWebGPU* _driver;

        WGPUBuffer _vertexMegaBuffer = nullptr;
        WGPUBuffer _indexMegaBuffer = nullptr;
        WGPUBuffer _slotTable = nullptr;
        WGPUBuffer _pushConstantRing = nullptr;
        WGPUBindGroupLayout _bindGroupLayout = nullptr;
        WGPUBindGroup _bindGroup = nullptr;

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
