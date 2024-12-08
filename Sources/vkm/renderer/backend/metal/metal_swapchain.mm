// Copyright (c) 2024 Snowapril

#include "vkm/renderer/backend/metal/metal_swapchain.h"

namespace vkm
{
    MetalSwapChain::MetalSwapChain()
        : SwapChain()
    {

    }

    MetalSwapChain::~MetalSwapChain()
    {

    }

    bool MetalSwapChain::createSwapChain(const char* title)
    {
        return true;
    }

    void MetalSwapChain::destroySwapChain()
    {

    }

    uint8_t MetalSwapChain::acquireNextImageIndexInner()
    {
        return 0;
    }

    void MetalSwapChain::presentInner()
    {

    }
} // namespace vkm