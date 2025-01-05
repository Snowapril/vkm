// Copyright (c) 2024 Snowapril

#include "vkm/renderer/backend/common/swapchain.h"

namespace vkm
{
    SwapChain::SwapChain()
    {

    }

    SwapChain::~SwapChain()
    {

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
    
    uint8_t SwapChain::acquireNextImageIndex()
    {

        return acquireNextImageIndexInner();
    }

    void SwapChain::present()
    {
        presentInner();
    }
} // namespace vkm