// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/webgpu/webgpu_frame_constant_manager.h>
#include <vkm/renderer/backend/webgpu/webgpu_driver.h>
#include <vkm/renderer/backend/webgpu/webgpu_util.h>

namespace vkm
{
    VkmFrameConstantManagerWebGPU::VkmFrameConstantManagerWebGPU(VkmDriverWebGPU* driver)
        : _driver(driver)
    {
    }

    VkmFrameConstantManagerWebGPU::~VkmFrameConstantManagerWebGPU()
    {
    }

    bool VkmFrameConstantManagerWebGPU::initialize()
    {
        WGPUDevice device = _driver->getDevice();

        // CopyDst rather than MapWrite: WebGPU buffer usage is map-mode-exclusive (MapWrite may
        // only combine with CopySrc), so a uniform buffer can never be persistently mapped --
        // update() goes through wgpuQueueWriteBuffer instead.
        WGPUBufferDescriptor bufferDescriptor{};
        bufferDescriptor.label = toWGPUStringView("VkmFrameConstantBuffer");
        bufferDescriptor.usage = WGPUBufferUsage_Uniform | WGPUBufferUsage_CopyDst;
        bufferDescriptor.size = kVkmFrameConstantBufferSize;
        _buffer = wgpuDeviceCreateBuffer(device, &bufferDescriptor);
        if (_buffer == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create frame-constant buffer");
            return false;
        }

        // Identity rather than zero, so a shader that reads group 1 before the first update()
        // still produces something defined (see VkmFrameConstants' default initializers).
        const VkmFrameConstants identity{};
        for (uint32_t frameIndex = 0; frameIndex < FRAME_COUNT; ++frameIndex)
        {
            wgpuQueueWriteBuffer(_driver->getQueue(), _buffer,
                                 static_cast<uint64_t>(frameIndex) * kVkmFrameConstantStride,
                                 &identity, sizeof(VkmFrameConstants));
        }

        WGPUBindGroupLayoutEntry layoutEntry{};
        layoutEntry.binding = kVkmFrameConstantBinding;
        layoutEntry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
        layoutEntry.buffer.type = WGPUBufferBindingType_Uniform;
        layoutEntry.buffer.hasDynamicOffset = false;
        layoutEntry.buffer.minBindingSize = sizeof(VkmFrameConstants);

        WGPUBindGroupLayoutDescriptor layoutDescriptor{};
        layoutDescriptor.label = toWGPUStringView("VkmFrameConstantBindGroupLayout");
        layoutDescriptor.entryCount = 1;
        layoutDescriptor.entries = &layoutEntry;
        _bindGroupLayout = wgpuDeviceCreateBindGroupLayout(device, &layoutDescriptor);
        if (_bindGroupLayout == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create frame-constant bind group layout");
            return false;
        }

        // One bind group per frame slot, each pinned to its own region.
        for (uint32_t frameIndex = 0; frameIndex < FRAME_COUNT; ++frameIndex)
        {
            WGPUBindGroupEntry bindEntry{};
            bindEntry.binding = kVkmFrameConstantBinding;
            bindEntry.buffer = _buffer;
            bindEntry.offset = static_cast<uint64_t>(frameIndex) * kVkmFrameConstantStride;
            bindEntry.size = sizeof(VkmFrameConstants);

            WGPUBindGroupDescriptor bindGroupDescriptor{};
            bindGroupDescriptor.label = toWGPUStringView("VkmFrameConstantBindGroup");
            bindGroupDescriptor.layout = _bindGroupLayout;
            bindGroupDescriptor.entryCount = 1;
            bindGroupDescriptor.entries = &bindEntry;
            _bindGroups[frameIndex] = wgpuDeviceCreateBindGroup(device, &bindGroupDescriptor);
            if (_bindGroups[frameIndex] == nullptr)
            {
                VKM_DEBUG_ERROR("Failed to create frame-constant bind group");
                return false;
            }
        }

        return true;
    }

    void VkmFrameConstantManagerWebGPU::destroy()
    {
        for (WGPUBindGroup& bindGroup : _bindGroups)
        {
            if (bindGroup != nullptr)
            {
                wgpuBindGroupRelease(bindGroup);
                bindGroup = nullptr;
            }
        }
        if (_bindGroupLayout != nullptr)
        {
            wgpuBindGroupLayoutRelease(_bindGroupLayout);
            _bindGroupLayout = nullptr;
        }
        if (_buffer != nullptr)
        {
            wgpuBufferRelease(_buffer);
            _buffer = nullptr;
        }
    }

    void VkmFrameConstantManagerWebGPU::update(uint32_t frameIndex, const VkmFrameConstants& constants)
    {
        VKM_ASSERT(frameIndex < FRAME_COUNT, "Frame slot index out of range");
        if (_buffer == nullptr)
        {
            return;
        }
        _activeFrameIndex = frameIndex;
        // A queue operation, ordered against submits -- legal here because the engine writes
        // before any render-graph recording, and it takes effect before the next submit.
        wgpuQueueWriteBuffer(_driver->getQueue(), _buffer,
                             static_cast<uint64_t>(frameIndex) * kVkmFrameConstantStride,
                             &constants, sizeof(VkmFrameConstants));
    }
} // namespace vkm
