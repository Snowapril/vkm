// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/webgpu/webgpu_bindless_resource_manager.h>
#include <vkm/renderer/backend/webgpu/webgpu_driver.h>
#include <vkm/renderer/backend/webgpu/webgpu_buffer.h>
#include <vkm/renderer/backend/webgpu/webgpu_util.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/common/buffer.h>

namespace vkm
{
    namespace
    {
        WGPUBuffer createBindlessBuffer(WGPUDevice device, const char* label, uint64_t size, WGPUBufferUsage usage)
        {
            WGPUBufferDescriptor descriptor{};
            descriptor.label = toWGPUStringView(label);
            descriptor.usage = usage;
            descriptor.size = size;
            return wgpuDeviceCreateBuffer(device, &descriptor);
        }
    }

    VkmBindlessResourceManagerWebGPU::VkmBindlessResourceManagerWebGPU(VkmDriverWebGPU* driver)
        : _driver(driver)
    {
    }

    VkmBindlessResourceManagerWebGPU::~VkmBindlessResourceManagerWebGPU()
    {
    }

    bool VkmBindlessResourceManagerWebGPU::initialize()
    {
        WGPUDevice device = _driver->getDevice();

        _vertexMegaBuffer = createBindlessBuffer(device, "VkmBindlessVertexMegaBuffer",
                                                 VERTEX_MEGA_BUFFER_SIZE,
                                                 WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
        _indexMegaBuffer = createBindlessBuffer(device, "VkmBindlessIndexMegaBuffer",
                                                INDEX_MEGA_BUFFER_SIZE,
                                                WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
        const uint64_t slotTableSize =
            static_cast<uint64_t>(kVkmBindlessBufferCapacity + kVkmBindlessIndexBufferCapacity) * sizeof(uint32_t);
        _slotTable = createBindlessBuffer(device, "VkmBindlessSlotTable", slotTableSize,
                                          WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
        _pushConstantRing = createBindlessBuffer(device, "VkmPushConstantRing",
                                                 PUSH_CONSTANT_ENTRY_COUNT * PUSH_CONSTANT_ENTRY_STRIDE,
                                                 WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
        if (_vertexMegaBuffer == nullptr || _indexMegaBuffer == nullptr ||
            _slotTable == nullptr || _pushConstantRing == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create bindless mega-buffers");
            return false;
        }

        // Engine-global bind group 0 layout, mirroring the shader-side convention (see the
        // VKM_BACKEND_WEBGPU variant in triangle.hlsl): b0 = push-constant UBO (dynamic
        // offset), b1 = vertex mega-buffer, b2 = index mega-buffer, b3 = slot table.
        WGPUBindGroupLayoutEntry layoutEntries[4]{};
        layoutEntries[0].binding = 0;
        layoutEntries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
        layoutEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
        layoutEntries[0].buffer.hasDynamicOffset = true;
        layoutEntries[0].buffer.minBindingSize = kVkmBindlessPushConstantSize;
        for (uint32_t i = 1; i < 4; ++i)
        {
            layoutEntries[i].binding = i;
            layoutEntries[i].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
            layoutEntries[i].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
        }

        WGPUBindGroupLayoutDescriptor layoutDescriptor{};
        layoutDescriptor.label = toWGPUStringView("VkmBindlessBindGroupLayout");
        layoutDescriptor.entryCount = 4;
        layoutDescriptor.entries = layoutEntries;
        _bindGroupLayout = wgpuDeviceCreateBindGroupLayout(device, &layoutDescriptor);
        if (_bindGroupLayout == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create bindless bind group layout");
            return false;
        }

        WGPUBindGroupEntry bindEntries[4]{};
        bindEntries[0].binding = 0;
        bindEntries[0].buffer = _pushConstantRing;
        bindEntries[0].offset = 0;
        bindEntries[0].size = PUSH_CONSTANT_ENTRY_STRIDE;
        bindEntries[1].binding = 1;
        bindEntries[1].buffer = _vertexMegaBuffer;
        bindEntries[1].size = VERTEX_MEGA_BUFFER_SIZE;
        bindEntries[2].binding = 2;
        bindEntries[2].buffer = _indexMegaBuffer;
        bindEntries[2].size = INDEX_MEGA_BUFFER_SIZE;
        bindEntries[3].binding = 3;
        bindEntries[3].buffer = _slotTable;
        bindEntries[3].size = slotTableSize;

        WGPUBindGroupDescriptor bindGroupDescriptor{};
        bindGroupDescriptor.label = toWGPUStringView("VkmBindlessBindGroup");
        bindGroupDescriptor.layout = _bindGroupLayout;
        bindGroupDescriptor.entryCount = 4;
        bindGroupDescriptor.entries = bindEntries;
        _bindGroup = wgpuDeviceCreateBindGroup(device, &bindGroupDescriptor);
        if (_bindGroup == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create bindless bind group");
            return false;
        }

        return true;
    }

    void VkmBindlessResourceManagerWebGPU::destroy()
    {
        if (_bindGroup != nullptr)
        {
            wgpuBindGroupRelease(_bindGroup);
            _bindGroup = nullptr;
        }
        if (_bindGroupLayout != nullptr)
        {
            wgpuBindGroupLayoutRelease(_bindGroupLayout);
            _bindGroupLayout = nullptr;
        }
        for (WGPUBuffer* buffer : {&_pushConstantRing, &_slotTable, &_indexMegaBuffer, &_vertexMegaBuffer})
        {
            if (*buffer != nullptr)
            {
                wgpuBufferRelease(*buffer);
                *buffer = nullptr;
            }
        }
    }

    uint32_t VkmBindlessResourceManagerWebGPU::registerBuffer(VkmResourceHandle bufferHandle, VkmBindlessArrayType arrayType)
    {
        VKM_ASSERT(arrayType == VkmBindlessArrayType::Buffer || arrayType == VkmBindlessArrayType::IndexBuffer,
            "registerBuffer only accepts Buffer or IndexBuffer array types");

        const bool isVertexArray = (arrayType == VkmBindlessArrayType::Buffer);
        const uint32_t elementStride = isVertexArray ? VERTEX_ELEMENT_STRIDE : INDEX_ELEMENT_STRIDE;

        VkmBufferWebGPU* bufferWebGPU = static_cast<VkmBufferWebGPU*>(
            _driver->getRenderResourcePool()->getResource<VkmBuffer>(bufferHandle));
        const VkmBufferInfo& bufferInfo = bufferWebGPU->getBufferInfo();

        // The mega-buffers are typed WGSL arrays; sources must be whole elements and
        // copyable (see the class comment for the convention this enforces).
        VKM_ASSERT(bufferInfo._size % elementStride == 0,
            "Bindless-registered WebGPU buffers must be tightly packed arrays of the engine element type");
        VKM_ASSERT(bufferInfo._flags & VkmResourceCreateInfo::AllowTransferSrc,
            "Bindless-registered WebGPU buffers need AllowTransferSrc for the mega-buffer copy");

        VkmBindlessSlotAllocator& slots = isVertexArray ? _bufferSlots : _indexBufferSlots;
        const uint32_t slot = slots.allocate();
        if (slot == UINT32_MAX)
        {
            VKM_DEBUG_ERROR("Bindless buffer array is exhausted");
            return UINT32_MAX;
        }

        VkmOffsetAllocator& megaAllocator = isVertexArray ? _vertexMegaAllocator : _indexMegaAllocator;
        const VkmGpuMemoryAllocation allocation =
            megaAllocator.allocate(static_cast<uint32_t>(bufferInfo._size), elementStride);
        if (!allocation.isValid())
        {
            // Fail-hard at capacity: mega-buffer growth is deliberately out of scope (TODO.md).
            VKM_DEBUG_ERROR("Bindless mega-buffer is exhausted");
            slots.release(slot);
            return UINT32_MAX;
        }
        (isVertexArray ? _vertexSlotRanges : _indexSlotRanges)[slot] = allocation;

        WGPUDevice device = _driver->getDevice();
        WGPUQueue queue = _driver->getQueue();

        // Copy the buffer's contents into its mega-buffer range now, via a transient
        // one-copy submit. Queue ordering makes this correct without waiting: the source
        // was filled by uploadToBuffer()'s already-submitted copy, and any draw referencing
        // the slot is submitted later.
        WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
        wgpuCommandEncoderCopyBufferToBuffer(encoder, bufferWebGPU->getBuffer(), 0,
                                             isVertexArray ? _vertexMegaBuffer : _indexMegaBuffer,
                                             allocation._offset, bufferInfo._size);
        WGPUCommandBuffer commandBuffer = wgpuCommandEncoderFinish(encoder, nullptr);
        wgpuQueueSubmit(queue, 1, &commandBuffer);
        wgpuCommandBufferRelease(commandBuffer);
        wgpuCommandEncoderRelease(encoder);

        // Publish the range's element offset in the slot table (index-buffer slots live in
        // the table's upper half -- see the shader-side convention).
        const uint32_t elementOffset = allocation._offset / elementStride;
        const uint32_t tableIndex = isVertexArray ? slot : (kVkmBindlessBufferCapacity + slot);
        wgpuQueueWriteBuffer(queue, _slotTable, tableIndex * sizeof(uint32_t),
                             &elementOffset, sizeof(elementOffset));

        return slot;
    }

    void VkmBindlessResourceManagerWebGPU::unregisterBuffer(uint32_t slot, VkmBindlessArrayType arrayType)
    {
        VKM_ASSERT(arrayType == VkmBindlessArrayType::Buffer || arrayType == VkmBindlessArrayType::IndexBuffer,
            "unregisterBuffer only accepts Buffer or IndexBuffer array types");

        const bool isVertexArray = (arrayType == VkmBindlessArrayType::Buffer);
        auto& slotRanges = isVertexArray ? _vertexSlotRanges : _indexSlotRanges;
        auto rangeIt = slotRanges.find(slot);
        if (rangeIt != slotRanges.end())
        {
            (isVertexArray ? _vertexMegaAllocator : _indexMegaAllocator).free(rangeIt->second);
            slotRanges.erase(rangeIt);
        }
        (isVertexArray ? _bufferSlots : _indexBufferSlots).release(slot);
    }

    uint32_t VkmBindlessResourceManagerWebGPU::writePushConstants(const void* data, uint32_t size)
    {
        VKM_ASSERT(size <= PUSH_CONSTANT_ENTRY_STRIDE, "Push constant data exceeds ring entry stride");

        if (_pushConstantCursor != 0 && (_pushConstantCursor % PUSH_CONSTANT_ENTRY_COUNT) == 0)
        {
            VKM_DEBUG_WARN("Push-constant ring wrapped; entries older than 1024 allocations are being reused");
        }

        const uint32_t entryIndex = _pushConstantCursor % PUSH_CONSTANT_ENTRY_COUNT;
        _pushConstantCursor++;

        const uint32_t byteOffset = entryIndex * PUSH_CONSTANT_ENTRY_STRIDE;
        wgpuQueueWriteBuffer(_driver->getQueue(), _pushConstantRing, byteOffset, data, size);
        return byteOffset;
    }
} // namespace vkm
