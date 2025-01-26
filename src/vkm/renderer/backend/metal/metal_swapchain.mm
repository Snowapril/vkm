// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_swapchain.h>
#include <vkm/renderer/backend/metal/metal_command_queue.h>
#include <QuartzCore/CAMetalLayer.h>
#include <Metal/MTLCommandBuffer.h>
#include <Metal/MTLCommandQueue.h>

namespace vkm
{
    VkmSwapChainMetal::VkmSwapChainMetal(VkmDriverBase* driver)
        : VkmSwapChainBase(driver)
    {

    }

    VkmSwapChainMetal::~VkmSwapChainMetal()
    {

    }

    void VkmSwapChainMetal::overrideCurrentDrawable(id<CAMetalDrawable> currentDrawable)
    {
        _currentDrawable = currentDrawable;
    }

    void VkmSwapChainMetal::setDebugName(const char* name)
    {
        
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
        if (_currentDrawable != nil)
        {
            
        }
        
        return VKM_INVALID_RESOURCE_HANDLE;
    }

    void VkmSwapChainMetal::presentInner()
    {
        if (_currentDrawable == nil)
            return;

        VkmCommandQueueMetal* commandQueueMetal = static_cast<VkmCommandQueueMetal*>(_presentQueue);
        id<MTLCommandQueue> mtlCommandQueue = commandQueueMetal->getMTLCommandQueue();

        id<MTLCommandBuffer> presentCommandBuffer = [mtlCommandQueue commandBuffer];
        [presentCommandBuffer setLabel:@"Present Command Buffer"];
        [presentCommandBuffer presentDrawable:_currentDrawable];
        [presentCommandBuffer commit];
    }
} // namespace vkm
