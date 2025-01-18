// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/swapchain.h>

namespace vkm
{
    VkmSwapChainBase::VkmSwapChainBase(VkmDriverBase* driver)
        : _driver(driver)
    {

    }

    VkmSwapChainBase::~VkmSwapChainBase()
    {
        destroy();
    }

    bool VkmSwapChainBase::initialize(const VkmWindowInfo& windowInfo)
    {
        _extent = glm::uvec2(windowInfo._width, windowInfo._height);
        return createSwapChain(windowInfo._windowHandle);
    }

    void VkmSwapChainBase::destroy()
    {
        destroySwapChain();
    }

    void VkmSwapChainBase::resize(uint32_t width, uint32_t height)
    {
        (void)width; (void)height;
        // TODO: Implement this
        
        // 1. check we need to destroy and recreate swapchain
        // 2. ensure backbuffer related to swapchain not in use
        // 3. recreate swapchain and update extent
    }
    
    VkmResourceHandle VkmSwapChainBase::acquireNextImage()
    {
        return acquireNextImageInner();
    }

    void VkmSwapChainBase::present()
    {
        presentInner();
    }
} // namespace vkm