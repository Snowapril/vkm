// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/swapchain.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>

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
        _windowHandle = windowInfo._windowHandle;
        _extent = glm::uvec2(windowInfo._width, windowInfo._height);
        return createSwapChain(_windowHandle);
    }

    void VkmSwapChainBase::destroy()
    {
        destroySwapChain();
    }

    void VkmSwapChainBase::resize(uint32_t width, uint32_t height)
    {
        if (width == _extent.x && height == _extent.y && _outOfDate == false)
        {
            return;
        }

        _extent = glm::uvec2(width, height);
        _outOfDate = false;
        // The new back buffers are a fresh set; nothing has been acquired from them yet.
        _currentBackBufferIndex = INVALID_VALUE32;

        destroySwapChain();

        // A minimized window has no surface to present to. Stay torn down until it comes back --
        // every backend rejects a zero-extent swapchain, and the engine skips a zero-extent window.
        if (width == 0 || height == 0)
        {
            return;
        }

        const bool result = createSwapChain(_windowHandle);
        VKM_ASSERT(result, "Failed to recreate swapchain");
    }
    
    VkmResourceHandle VkmSwapChainBase::acquireNextImage()
    {
        return acquireNextImageInner();
    }

    void VkmSwapChainBase::present()
    {
        presentInner();
    }

    void VkmSwapChainBase::destroySwapChainCommon()
    {
        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();
        for (VkmResourceHandle& handle : _backBuffers)
        {
            renderResourcePool->releaseResource(handle);
            // resize() creates a new set right after this, and a backend may produce fewer images
            // than it did before; leaving a released handle behind would double-release it next
            // time. Invalid entries are exactly what the _backBuffers contract expects.
            handle = VKM_INVALID_RESOURCE_HANDLE;
        }
        _backBufferCount = 0;
    }
} // namespace vkm