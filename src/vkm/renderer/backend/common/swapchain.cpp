// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/swapchain.h>

namespace vkm
{
    VkmSwapChain::VkmSwapChain(VkmDriverBase* driver)
        : _driver(driver)
    {

    }

    VkmSwapChain::~VkmSwapChain()
    {
        destroy();
    }

    bool VkmSwapChain::initialize(const VkmWindowInfo& windowInfo)
    {
        _extent = glm::uvec2(windowInfo._width, windowInfo._height);
        return createSwapChain(windowInfo._windowHandle);
    }

    void VkmSwapChain::destroy()
    {
        destroySwapChain();
    }

    void VkmSwapChain::resize(uint32_t width, uint32_t height)
    {
        (void)width; (void)height;
        // TODO: Implement this
        
        // 1. check we need to destroy and recreate swapchain
        // 2. ensure backbuffer related to swapchain not in use
        // 3. recreate swapchain and update extent
    }
    
    VkmResourceHandle VkmSwapChain::acquireNextImage()
    {
        return acquireNextImageInner();
    }

    void VkmSwapChain::present()
    {
        presentInner();
    }
} // namespace vkm