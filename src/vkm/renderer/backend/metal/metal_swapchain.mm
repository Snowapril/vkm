// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_swapchain.h>

namespace vkm
{
    VkmSwapChainMetal::VkmSwapChainMetal(VkmDriverBase* driver)
        : VkmSwapChain(driver)
    {

    }

    VkmSwapChainMetal::~VkmSwapChainMetal()
    {

    }

    bool VkmSwapChainMetal::createSwapChain(VkmWindowHandle windowHandle)
    {
        return true;
    }

    void VkmSwapChainMetal::destroySwapChain()
    {

    }

    VkmResourceHandle VkmSwapChainMetal::acquireNextImageInner()
    {
        return VKM_INVALID_RESOURCE_HANDLE;
    }

    void VkmSwapChainMetal::presentInner()
    {

    }
} // namespace vkm