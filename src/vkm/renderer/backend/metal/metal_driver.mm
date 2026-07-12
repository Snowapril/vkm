// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/metal/metal_texture.h>
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