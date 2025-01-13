// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/swapchain.h>
#include <volk.h>

namespace vkm
{
    class VkmSwapChainVulkan final : public VkmSwapChain
    {
    public:
        VkmSwapChainVulkan(VkmDriverBase* driver);
        ~VkmSwapChainVulkan();

    protected:
        virtual bool createSwapChain(VkmWindowHandle windowHandle) override final;
        virtual void destroySwapChain() override final;
        virtual VkmResourceHandle acquireNextImageInner() override final;
        virtual void presentInner() override final;

    private:
        VkSwapchainKHR  _swapChain;
        VkFormat        _imageFormat;
        VkSurfaceKHR    _surface;
    };
} // namespace vkm