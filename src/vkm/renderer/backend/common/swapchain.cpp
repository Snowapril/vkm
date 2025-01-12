// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/swapchain.h>

namespace vkm
{
    SwapChain::SwapChain(VkmDriverBase* driver)
        : _driver(driver)
    {

    }

    SwapChain::~SwapChain()
    {
        destroy();
    }

    bool SwapChain::initialize(uint32_t width, uint32_t height, const char* title)
    {
        _extent = glm::uvec2(width, height);
        return createSwapChain(title);
    }

    void SwapChain::destroy()
    {
        destroySwapChain();
    }

    void SwapChain::resize(uint32_t width, uint32_t height)
    {
        (void)width; (void)height;
        // TODO: Implement this
        
        // 1. check we need to destroy and recreate swapchain
        // 2. ensure backbuffer related to swapchain not in use
        // 3. recreate swapchain and update extent
    }
    
    VkmResourceHandle SwapChain::acquireNextImage()
    {
        return acquireNextImageInner();
    }

    void SwapChain::present()
    {
        presentInner();
    }
} // namespace vkm