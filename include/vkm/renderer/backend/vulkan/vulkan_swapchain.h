// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/swapchain.h>
#include <volk.h>

namespace vkm
{
    class VkmSwapChainVulkan final : public VkmSwapChainBase
    {
    public:
        VkmSwapChainVulkan(VkmDriverBase* driver);
        ~VkmSwapChainVulkan();

        virtual void setDebugName(const char* name) override final;

    protected:
        virtual bool createSwapChain(void* windowHandle) override final;
        virtual void destroySwapChain() override final;
        virtual VkmResourceHandle acquireNextImageInner() override final;
        virtual void presentInner() override final;

    private:
        VkSwapchainKHR  _swapChain{VK_NULL_HANDLE};
        VkFormat        _imageFormat;
        VkSurfaceKHR    _surface{VK_NULL_HANDLE};
        VkFence         _acquireFence{VK_NULL_HANDLE};
    };
} // namespace vkm