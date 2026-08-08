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
                                                 static_cast<uint64_t>(kVkmPushConstantRingTotalEntryCount) *
                                                     PUSH_CONSTANT_ENTRY_STRIDE,
                                                 WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst);
        if (_vertexMegaBuffer == nullptr || _indexMegaBuffer == nullptr ||
            _slotTable == nullptr || _pushConstantRing == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create bindless mega-buffers");
            return false;
        }

        // Stand in for any singleton binding nothing has published yet: a WebGPU bind group has to
        // supply an entry for every layout entry, so there is no "leave it out" option. One buffer
        // each -- see the member's comment for why sharing is not an option here.
        for (uint32_t i = 0; i < static_cast<uint32_t>(VkmBindlessSingletonBuffer::Count); ++i)
        {
            _singletonPlaceholders[i] =
                createBindlessBuffer(device, "VkmBindlessSingletonPlaceholder", kSingletonPlaceholderSize,
                                     WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst);
            if (_singletonPlaceholders[i] == nullptr)
            {
                VKM_DEBUG_ERROR("Failed to create a bindless singleton placeholder buffer");
                return false;
            }
        }
        _slotTableSize = slotTableSize;

        /*
        * Engine-global bind group 0 layout, mirroring the shader-side convention (see the
        * VKM_BACKEND_WEBGPU variants in the engine shaders): b0 = push-constant UBO (dynamic
        * offset), b1 = vertex mega-buffer, b2 = index mega-buffer, b3 = slot table, b4..b6 = the
        * VkmBindlessSingletonBuffer entries.
        *
        * IndirectArgument is the only writable-storage entry and is visible to compute ONLY.
        * WebGPU forbids writable storage in the vertex stage, and a buffer used as writable
        * storage inside a render pass's usage scope may not also be fetched as an indirect buffer
        * in that scope -- which is exactly what the GPU-driven draw path does. Keeping the entry
        * compute-only is what makes that legal, so no render pipeline's shader may declare it.
        */
        constexpr uint32_t kSingletonCount = static_cast<uint32_t>(VkmBindlessSingletonBuffer::Count);
        constexpr uint32_t kEntryCount = kFirstSingletonBinding + kSingletonCount;
        // Everything up to but excluding IndirectArgument, which is the first writable singleton.
        constexpr uint32_t kGraphicsEntryCount =
            kFirstSingletonBinding + static_cast<uint32_t>(VkmBindlessSingletonBuffer::IndirectArgument);

        WGPUBindGroupLayoutEntry layoutEntries[kEntryCount]{};
        layoutEntries[0].binding = 0;
        layoutEntries[0].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment | WGPUShaderStage_Compute;
        layoutEntries[0].buffer.type = WGPUBufferBindingType_Uniform;
        layoutEntries[0].buffer.hasDynamicOffset = true;
        layoutEntries[0].buffer.minBindingSize = kVkmBindlessPushConstantSize;
        for (uint32_t i = 1; i < kFirstSingletonBinding; ++i)
        {
            layoutEntries[i].binding = i;
            layoutEntries[i].visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment | WGPUShaderStage_Compute;
            layoutEntries[i].buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
        }
        for (uint32_t i = 0; i < kSingletonCount; ++i)
        {
            const VkmBindlessSingletonBuffer which = static_cast<VkmBindlessSingletonBuffer>(i);
            const bool isWritable = (which == VkmBindlessSingletonBuffer::IndirectArgument ||
                                     which == VkmBindlessSingletonBuffer::VisibleList);
            WGPUBindGroupLayoutEntry& entry = layoutEntries[kFirstSingletonBinding + i];
            entry.binding = kFirstSingletonBinding + i;
            entry.visibility = isWritable
                                   ? WGPUShaderStage_Compute
                                   : (WGPUShaderStage_Vertex | WGPUShaderStage_Fragment | WGPUShaderStage_Compute);
            entry.buffer.type = isWritable ? WGPUBufferBindingType_Storage
                                           : WGPUBufferBindingType_ReadOnlyStorage;
        }

        WGPUBindGroupLayoutDescriptor layoutDescriptor{};
        layoutDescriptor.label = toWGPUStringView("VkmBindlessBindGroupLayout");
        layoutDescriptor.entryCount = kEntryCount;
        layoutDescriptor.entries = layoutEntries;
        _bindGroupLayout = wgpuDeviceCreateBindGroupLayout(device, &layoutDescriptor);
        if (_bindGroupLayout == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create bindless bind group layout");
            return false;
        }

        // The graphics shape stops before the read-write singletons -- see getBindGroupLayout().
        // They are the last entries, so this is a prefix rather than a filtered copy.
        WGPUBindGroupLayoutDescriptor graphicsLayoutDescriptor{};
        graphicsLayoutDescriptor.label = toWGPUStringView("VkmBindlessGraphicsBindGroupLayout");
        graphicsLayoutDescriptor.entryCount = kGraphicsEntryCount;
        graphicsLayoutDescriptor.entries = layoutEntries;
        _graphicsBindGroupLayout = wgpuDeviceCreateBindGroupLayout(device, &graphicsLayoutDescriptor);
        if (_graphicsBindGroupLayout == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create the graphics bindless bind group layout");
            return false;
        }

        return recreateBindGroup();
    }

    bool VkmBindlessResourceManagerWebGPU::recreateBindGroup()
    {
        constexpr uint32_t kSingletonCount = static_cast<uint32_t>(VkmBindlessSingletonBuffer::Count);
        constexpr uint32_t kEntryCount = kFirstSingletonBinding + kSingletonCount;
        // Everything up to but excluding IndirectArgument, which is the first writable singleton.
        constexpr uint32_t kGraphicsEntryCount =
            kFirstSingletonBinding + static_cast<uint32_t>(VkmBindlessSingletonBuffer::IndirectArgument);

        WGPUBindGroupEntry bindEntries[kEntryCount]{};
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
        bindEntries[3].size = _slotTableSize;
        for (uint32_t i = 0; i < kSingletonCount; ++i)
        {
            WGPUBindGroupEntry& entry = bindEntries[kFirstSingletonBinding + i];
            entry.binding = kFirstSingletonBinding + i;
            const bool bound = _singletonBuffers[i] != nullptr;
            entry.buffer = bound ? _singletonBuffers[i] : _singletonPlaceholders[i];
            entry.size = bound ? _singletonSizes[i] : kSingletonPlaceholderSize;
        }

        WGPUBindGroupDescriptor bindGroupDescriptor{};
        bindGroupDescriptor.label = toWGPUStringView("VkmBindlessBindGroup");
        bindGroupDescriptor.layout = _bindGroupLayout;
        bindGroupDescriptor.entryCount = kEntryCount;
        bindGroupDescriptor.entries = bindEntries;

        WGPUBindGroup bindGroup = wgpuDeviceCreateBindGroup(_driver->getDevice(), &bindGroupDescriptor);
        if (bindGroup == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create bindless bind group");
            return false;
        }

        // The graphics shape is the same entries minus the read-write singletons, which are last.
        WGPUBindGroupDescriptor graphicsDescriptor{};
        graphicsDescriptor.label = toWGPUStringView("VkmBindlessGraphicsBindGroup");
        graphicsDescriptor.layout = _graphicsBindGroupLayout;
        graphicsDescriptor.entryCount = kGraphicsEntryCount;
        graphicsDescriptor.entries = bindEntries;

        WGPUBindGroup graphicsBindGroup = wgpuDeviceCreateBindGroup(_driver->getDevice(), &graphicsDescriptor);
        if (graphicsBindGroup == nullptr)
        {
            wgpuBindGroupRelease(bindGroup);
            VKM_DEBUG_ERROR("Failed to create the graphics bindless bind group");
            return false;
        }

        if (_bindGroup != nullptr)
        {
            wgpuBindGroupRelease(_bindGroup);
        }
        _bindGroup = bindGroup;
        if (_graphicsBindGroup != nullptr)
        {
            wgpuBindGroupRelease(_graphicsBindGroup);
        }
        _graphicsBindGroup = graphicsBindGroup;
        return true;
    }

    bool VkmBindlessResourceManagerWebGPU::setSingletonBuffer(VkmBindlessSingletonBuffer which, VkmResourceHandle bufferHandle)
    {
        VKM_ASSERT(which < VkmBindlessSingletonBuffer::Count, "Unknown VkmBindlessSingletonBuffer");

        const size_t index = static_cast<size_t>(which);
        if (bufferHandle == VKM_INVALID_RESOURCE_HANDLE)
        {
            if (_singletonBuffers[index] == nullptr)
            {
                return true;
            }
            _singletonBuffers[index] = nullptr;
            _singletonSizes[index] = 0;
            return recreateBindGroup();
        }

        VkmBufferWebGPU* bufferWebGPU = static_cast<VkmBufferWebGPU*>(
            _driver->getRenderResourcePool()->getResource<VkmBuffer>(bufferHandle));
        if (bufferWebGPU == nullptr)
        {
            VKM_DEBUG_ERROR("setSingletonBuffer was given a handle that is not a live buffer");
            return false;
        }

        _singletonBuffers[index] = bufferWebGPU->getBuffer();
        _singletonSizes[index] = bufferWebGPU->getBufferInfo()._size;
        return recreateBindGroup();
    }

    bool VkmBindlessResourceManagerWebGPU::setAccelerationStructure(VkmResourceHandle accelerationStructureHandle)
    {
        // Unbinding is the one case that succeeds, so a teardown path shared with the other
        // backends does not log an error on the way out of a scene that never bound one.
        if (accelerationStructureHandle == VKM_INVALID_RESOURCE_HANDLE)
        {
            return true;
        }
        VKM_DEBUG_ERROR("WebGPU has no acceleration structure API; this backend never reports "
                        "VkmDriverCapabilityFlags::RayTracing");
        return false;
    }

    void VkmBindlessResourceManagerWebGPU::destroy()
    {
        if (_bindGroup != nullptr)
        {
            wgpuBindGroupRelease(_bindGroup);
            _bindGroup = nullptr;
        }
        if (_graphicsBindGroup != nullptr)
        {
            wgpuBindGroupRelease(_graphicsBindGroup);
            _graphicsBindGroup = nullptr;
        }
        if (_bindGroupLayout != nullptr)
        {
            wgpuBindGroupLayoutRelease(_bindGroupLayout);
            _bindGroupLayout = nullptr;
        }
        if (_graphicsBindGroupLayout != nullptr)
        {
            wgpuBindGroupLayoutRelease(_graphicsBindGroupLayout);
            _graphicsBindGroupLayout = nullptr;
        }
        for (WGPUBuffer& placeholder : _singletonPlaceholders)
        {
            if (placeholder != nullptr)
            {
                wgpuBufferRelease(placeholder);
                placeholder = nullptr;
            }
        }
        for (WGPUBuffer* buffer : {&_pushConstantRing, &_slotTable, &_indexMegaBuffer, &_vertexMegaBuffer})
        {
            if (*buffer != nullptr)
            {
                wgpuBufferRelease(*buffer);
                *buffer = nullptr;
            }
        }
        // Not owned here -- the scene owns the buffers it published.
        _singletonBuffers.fill(nullptr);
        _singletonSizes.fill(0);
    }

    uint32_t VkmBindlessResourceManagerWebGPU::registerBuffer(VkmResourceHandle bufferHandle, VkmBindlessArrayType arrayType)
    {
        VKM_ASSERT(arrayType == VkmBindlessArrayType::Buffer || arrayType == VkmBindlessArrayType::IndexBuffer,
            "registerBuffer only accepts Buffer or IndexBuffer array types");

        const bool isVertexArray = (arrayType == VkmBindlessArrayType::Buffer);

        VkmBufferWebGPU* bufferWebGPU = static_cast<VkmBufferWebGPU*>(
            _driver->getRenderResourcePool()->getResource<VkmBuffer>(bufferHandle));
        const VkmBufferInfo& bufferInfo = bufferWebGPU->getBufferInfo();

        // Both mega-buffers are u32 word arrays, so the only requirement on a source is that it is
        // a whole number of words -- any vertex stride works (see the class comment).
        VKM_ASSERT(bufferInfo._size % ELEMENT_STRIDE == 0,
            "Bindless-registered WebGPU buffers must be a whole number of u32 words");
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
        // 16-byte alignment keeps a float4-aligned vertex attribute aligned inside the pool too,
        // and is a multiple of the u32 word the offsets are expressed in.
        const VkmGpuMemoryAllocation allocation =
            megaAllocator.allocate(static_cast<uint32_t>(bufferInfo._size), 16);
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

        // Publish the range's u32-word offset in the slot table (index-pool slots live in the
        // table's upper half -- see the shader-side convention).
        const uint32_t wordOffset = allocation._offset / ELEMENT_STRIDE;
        const uint32_t tableIndex = isVertexArray ? slot : (kVkmBindlessBufferCapacity + slot);
        wgpuQueueWriteBuffer(queue, _slotTable, tableIndex * sizeof(uint32_t),
                             &wordOffset, sizeof(wordOffset));

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

    uint32_t VkmBindlessResourceManagerWebGPU::registerTexture(VkmResourceHandle textureHandle)
    {
        // Pairs with onCopyBufferToTexture: see VkmDriverCapabilityFlags::TextureUpload.
        VKM_DEBUG_ERROR("registerTexture is not implemented on the WebGPU backend");
        return UINT32_MAX;
    }

    void VkmBindlessResourceManagerWebGPU::unregisterTexture(uint32_t slot)
    {
        // Nothing was ever registered, so there is no slot to release.
    }

    void VkmBindlessResourceManagerWebGPU::beginFrame(uint32_t frameSlot)
    {
        _pushConstantEntries.beginFrame(frameSlot);
    }

    uint32_t VkmBindlessResourceManagerWebGPU::writePushConstants(const void* data, uint32_t size)
    {
        VKM_ASSERT(size <= PUSH_CONSTANT_ENTRY_STRIDE, "Push constant data exceeds ring entry stride");

        bool overflowed = false;
        const uint32_t entryIndex = _pushConstantEntries.allocate(&overflowed);
        if (overflowed)
        {
            // Wrapping within a frame slot's own region reuses entries *this* frame is still
            // referencing, which no wait can make safe.
            VKM_DEBUG_WARN("Push-constant ring region overflowed: this frame pushed more than "
                           "kVkmPushConstantRingEntryCount times and is reusing its own entries");
        }

        const uint32_t byteOffset = entryIndex * PUSH_CONSTANT_ENTRY_STRIDE;
        wgpuQueueWriteBuffer(_driver->getQueue(), _pushConstantRing, byteOffset, data, size);
        return byteOffset;
    }
} // namespace vkm
