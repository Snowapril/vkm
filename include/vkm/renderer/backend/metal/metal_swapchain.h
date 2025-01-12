// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/swapchain.h>

namespace vkm
{
    class MetalSwapChain final : public SwapChain
    {
    public:
        MetalSwapChain(VkmDriverBase* driver);
        ~MetalSwapChain();

    protected:
        virtual bool createSwapChain(const char* title) override final;
        virtual void destroySwapChain() override final;
        virtual VkmResourceHandle acquireNextImageInner() override final;
        virtual void presentInner() override final;

    };
} // namespace vkm