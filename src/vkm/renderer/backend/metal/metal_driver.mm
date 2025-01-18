// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/metal/metal_texture.h>
#include <vkm/renderer/backend/metal/metal_swapchain.h>

namespace vkm
{
    VkmDriverMetal::VkmDriverMetal(MTLDevice* mtlDevice)
        : VkmDriverBase(), _mtlDevice(mtlDevice)
    {
        
    }

    VkmDriverMetal::~VkmDriverMetal()
    {

    }

    VkmTexture* VkmDriverMetal::newTextureInner()
    {
        return new VkmTextureMetal(this);
    }
    
    VkmSwapChain* VkmDriverMetal::newSwapChain()
    {
        return new VkmSwapChainMetal(this);
    }

    bool VkmDriverMetal::initializeInner(const VkmEngineLaunchOptions* options)
    {
        return true;
    }

    void VkmDriverMetal::destroyInner()
    {

    }
}