// Copyright (c) 2024 Snowapril

#include "vkm/renderer/backend/metal/metal_swapchain.h"

namespace vkm
{
    MetalSwapChain::MetalSwapChain(VkmDriverBase* driver)
        : SwapChain(driver)
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

    VkmResourceHandle MetalSwapChain::acquireNextImageIndex()
    {
        return VKM_INVALID_RESOURCE_HANDLE;
    }

    void MetalSwapChain::presentInner()
    {

    }
} // namespace vkm