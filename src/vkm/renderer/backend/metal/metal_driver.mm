// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/metal/metal_texture.h>
#include <vkm/renderer/backend/metal/metal_swapchain.h>
#include <vkm/renderer/backend/metal/metal_command_queue.h>

namespace vkm
{
    VkmDriverMetal::VkmDriverMetal(id<MTLDevice> mtlDevice)
        : VkmDriverBase(), _mtlDevice(mtlDevice)
    {
        
    }

    VkmDriverMetal::~VkmDriverMetal()
    {

    }

    bool VkmDriverMetal::initializeInner(const VkmEngineLaunchOptions* options)
    {
        _driverCapabilityFlags = VkmDriverCapabilityFlags::None;
        return true;
    }

    void VkmDriverMetal::destroyInner()
    {

    }

    VkmTexture* VkmDriverMetal::newTextureInner()
    {
        // TODO(snowapril) : create texture via resource pool backend
        return new VkmTextureMetal(this);
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