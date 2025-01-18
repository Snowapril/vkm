// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_swapchain.h>
#include <QuartzCore/CAMetalLayer.h>

namespace vkm
{
    VkmSwapChainMetal::VkmSwapChainMetal(VkmDriverBase* driver)
        : VkmSwapChainBase(driver)
    {

    }

    VkmSwapChainMetal::~VkmSwapChainMetal()
    {

    }

    void VkmSwapChainMetal::overrideCurrentDrawable(CAMetalDrawable* currentDrawable)
    {
        _currentDrawable = currentDrawable;
    }

    bool VkmSwapChainMetal::createSwapChain(void* windowHandle)
    {
        return true;
    }

    void VkmSwapChainMetal::destroySwapChain()
    {

    }

    VkmResourceHandle VkmSwapChainMetal::acquireNextImageInner()
    {
        if ( _currentDrawable != nullptr )
        {
            
        }
        
        return VKM_INVALID_RESOURCE_HANDLE;
    }

    void VkmSwapChainMetal::presentInner()
    {

    }
} // namespace vkm
