// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/imgui/imgui_renderer.h>

namespace vkm
{
    class VkmImGuiRendererVulkan final : public VkmImGuiRendererBase
    {
    public:
        VkmImGuiRendererVulkan(VkmDriverBase* driver);
        ~VkmImGuiRendererVulkan();

    protected:
        virtual bool initializeInner(void* windowHandle, VkmFormat backBufferFormat) override final;
        virtual void newFrameInner() override final;
        virtual void renderDrawDataInner(VkmCommandBufferBase* commandBuffer) override final;
        virtual void shutdownInner() override final;
    };
} // namespace vkm
