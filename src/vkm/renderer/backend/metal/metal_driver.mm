// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/metal/metal_texture.h>

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
    
    SwapChain* VkmDriverMetal::newSwapChain(const VkmWindowInfo& windowInfo)
    {
        (void)windowInfo;
        // TODO(snowapril) : implement swapchain creation
        return nullptr;
    }

    bool VkmDriverMetal::initializeInner(const VkmEngineLaunchOptions* options)
    {
        return true;
    }

    void VkmDriverMetal::destroyInner()
    {

    }
}