// Copyright (c) 2026 Snowapril

#include <vkm/renderer/backend/metal/metal_frame_constant_manager.h>
#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/metal/metal_render_resource_pool.h>
#include <vkm/renderer/backend/common/bindless_resource_manager.h>

#import <Metal/MTLBuffer.h>
#import <Metal/MTLDevice.h>

#include <cstring>

namespace vkm
{
    VkmFrameConstantManagerMetal::VkmFrameConstantManagerMetal(VkmDriverMetal* driver)
        : _driver(driver)
    {
    }

    VkmFrameConstantManagerMetal::~VkmFrameConstantManagerMetal()
    {
    }

    bool VkmFrameConstantManagerMetal::initialize()
    {
        id<MTLDevice> device = _driver->getMTLDevice();

        _constantBuffer = [device newBufferWithLength:kVkmFrameConstantBufferSize
                                             options:MTLResourceStorageModeShared];
        if (_constantBuffer == nil)
        {
            VKM_DEBUG_ERROR("Failed to create frame-constant buffer");
            return false;
        }
        _constantBuffer.label = @"VkmFrameConstantBuffer";

        // Identity rather than zero, so a shader that reads set 1 before the first update()
        // still produces something defined (see VkmFrameConstants' default initializers).
        const VkmFrameConstants identity{};
        for (uint32_t frameIndex = 0; frameIndex < FRAME_COUNT; ++frameIndex)
        {
            std::memcpy(static_cast<uint8_t*>(_constantBuffer.contents) + frameIndex * kVkmFrameConstantStride,
                        &identity, sizeof(VkmFrameConstants));
        }

        // The shader dereferences this buffer's raw GPU address, and the buffer never went
        // through newBuffer(), so it joins no residency set on its own -- a non-resident
        // buffer here is a GPU fault, not a warning. Same explicit registration the bindless
        // manager does for its argument buffer and push-constant ring.
        static_cast<VkmRenderResourcePoolMetal*>(_driver->getRenderResourcePool())
            ->registerExternalAllocation(_constantBuffer);

        return true;
    }

    void VkmFrameConstantManagerMetal::destroy()
    {
        if (_constantBuffer != nil)
        {
            [_constantBuffer release];
            _constantBuffer = nil;
        }
    }

    void VkmFrameConstantManagerMetal::update(uint32_t frameIndex, const VkmFrameConstants& constants)
    {
        VKM_ASSERT(frameIndex < FRAME_COUNT, "Frame slot index out of range");
        if (_constantBuffer == nil)
        {
            return;
        }
        _activeFrameIndex = frameIndex;
        std::memcpy(static_cast<uint8_t*>(_constantBuffer.contents) + frameIndex * kVkmFrameConstantStride,
                    &constants, sizeof(VkmFrameConstants));
    }

    uint64_t VkmFrameConstantManagerMetal::getActiveGpuAddress() const
    {
        if (_constantBuffer == nil)
        {
            return 0;
        }
        return _constantBuffer.gpuAddress + _activeFrameIndex * kVkmFrameConstantStride;
    }
} // namespace vkm
