// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/metal/metal_texture.h>
#include <vkm/renderer/backend/metal/metal_buffer.h>
#include <vkm/renderer/backend/metal/metal_staging_buffer.h>
#include <vkm/renderer/backend/metal/metal_sampler.h>
#include <vkm/renderer/backend/metal/metal_texture_view.h>
#include <vkm/renderer/backend/metal/metal_buffer_view.h>
#include <vkm/renderer/backend/metal/metal_gpu_heap_pool.h>
#include <vkm/renderer/backend/metal/metal_swapchain.h>
#include <vkm/renderer/backend/metal/metal_command_queue.h>
#include <vkm/renderer/backend/metal/metal_pipeline_state.h>

#import <Metal/MTLDevice.h>

namespace vkm
{
    VkmDriverMetal::VkmDriverMetal(id<MTLDevice> mtlDevice)
        : VkmDriverBase(), _mtlDevice(mtlDevice)
    {
        
    }

    VkmDriverMetal::~VkmDriverMetal()
    {

    }

    VkmInitResult VkmDriverMetal::initializeInner(const VkmEngineLaunchOptions* options)
    {
        if (_mtlDevice == nil)
        {
            VKM_DEBUG_ERROR("No Metal device available on this system.");
            return VkmInitResult{VkmInitResultCode::HardwareUnsupported, "No Metal device available on this system."};
        }

        if (@available(macOS 26.0, iOS 26.0, *))
        {
            if (![_mtlDevice supportsFamily:MTLGPUFamilyApple9])
            {
                VKM_DEBUG_ERROR("Metal 4 requires Apple Silicon (MTLGPUFamilyApple9 or later). Non-Apple Silicon devices are not supported.");
                return VkmInitResult{VkmInitResultCode::HardwareUnsupported, "Metal 4 requires Apple Silicon (MTLGPUFamilyApple9 or later); this GPU does not support it."};
            }
        }
        else
        {
            VKM_DEBUG_ERROR("Metal 4 requires macOS 26 / iOS 26 or later. This OS version is not supported.");
            return VkmInitResult{VkmInitResultCode::HardwareUnsupported, "Metal 4 requires macOS 26 / iOS 26 or later; this OS version is not supported."};
        }

        _driverCapabilityFlags = VkmDriverCapabilityFlags::None;
        return VkmInitResult{VkmInitResultCode::Success, ""};
    }

    void VkmDriverMetal::destroyInner()
    {

    }

    VkmTexture* VkmDriverMetal::newTextureInner()
    {
        // TODO(snowapril) : create texture via resource pool backend
        return new VkmTextureMetal(this);
    }

    VkmBuffer* VkmDriverMetal::newBufferInner()
    {
        return new VkmBufferMetal(this);
    }

    VkmStagingBuffer* VkmDriverMetal::newStagingBufferInner()
    {
        return new VkmStagingBufferMetal(this);
    }

    VkmSampler* VkmDriverMetal::newSamplerInner()
    {
        return new VkmSamplerMetal(this);
    }

    VkmTextureView* VkmDriverMetal::newTextureViewInner()
    {
        return new VkmTextureViewMetal(this);
    }

    VkmBufferView* VkmDriverMetal::newBufferViewInner()
    {
        return new VkmBufferViewMetal(this);
    }

    id<MTLBuffer> VkmDriverMetal::allocateFromHeapPool(uint64_t sizeBytes, uint64_t alignment, uint64_t options)
    {
        for (auto& pool : _heapPools)
        {
            id<MTLBuffer> buffer = pool->tryAllocateBuffer(sizeBytes, alignment, options);
            if (buffer != nil)
            {
                return buffer;
            }
        }

        if (sizeBytes > VkmGpuHeapPoolMetal::POOL_BLOCK_SIZE_BYTES)
        {
            VKM_DEBUG_ERROR("Buffer allocation exceeds heap pool block size; use a committed allocation instead");
            return nil;
        }

        auto newPool = std::make_unique<VkmGpuHeapPoolMetal>(this);
        if (!newPool->initialize())
        {
            return nil;
        }

        id<MTLBuffer> buffer = newPool->tryAllocateBuffer(sizeBytes, alignment, options);
        _heapPools.push_back(std::move(newPool));
        return buffer;
    }

    VkmPipelineStateBase* VkmDriverMetal::newPipelineStateInner()
    {
        return new VkmPipelineStateMetal(this);
    }

    VkmSwapChainBase* VkmDriverMetal::newSwapChainInner()
    {
        return new VkmSwapChainMetal(this);
    }

    VkmCommandQueueBase* VkmDriverMetal::newCommandQueueInner()
    {
        return new VkmCommandQueueMetal(this);
    }
}