// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/swapchain.h>

namespace vkm
{
    class VkmSwapChainMetal final : public VkmSwapChain
    {
    public:
        VkmSwapChainMetal(VkmDriverBase* driver);
        ~VkmSwapChainMetal();

    protected:
        virtual bool createSwapChain(VkmWindowHandle windowHandle) override final;
        virtual void destroySwapChain() override final;
        virtual VkmResourceHandle acquireNextImageInner() override final;
        virtual void presentInner() override final;

    };
} // namespace vkm