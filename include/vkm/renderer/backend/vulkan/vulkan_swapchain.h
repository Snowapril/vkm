// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/swapchain.h>
#include <volk.h>

namespace vkm
{
    class VulkanSwapChain final : public SwapChain
    {
    public:
        VulkanSwapChain(VkmDriverBase* driver);
        ~VulkanSwapChain();

    protected:
        virtual bool createSwapChain(const char* title) override final;
        virtual void destroySwapChain() override final;
        virtual VkmResourceHandle acquireNextImageInner() override final;
        virtual void presentInner() override final;

    private:
        VkSwapchainKHR  _swapchain;
        VkFormat        _format;
        VkSurfaceKHR    _surface;
    };
} // namespace vkm