// Copyright (c) 2025 Snowapril

#include <vkm/renderer/imgui/imgui_renderer.h>
#include <imgui.h>

namespace vkm
{
    VkmImGuiRendererBase::VkmImGuiRendererBase(VkmDriverBase* driver)
        : _driver(driver)
    {
    }

    VkmImGuiRendererBase::~VkmImGuiRendererBase()
    {
    }

    bool VkmImGuiRendererBase::initialize(void* windowHandle, VkmFormat backBufferFormat)
    {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        return initializeInner(windowHandle, backBufferFormat);
    }

    void VkmImGuiRendererBase::newFrame(bool windowFocused)
    {
        _windowFocused = windowFocused;
        newFrameInner();
        _frameRendered = false;
    }

    void VkmImGuiRendererBase::renderDrawData(VkmCommandBufferBase* commandBuffer)
    {
        if (_frameRendered)
        {
            return;
        }
        _frameRendered = true;

        ImGui::Render();
        renderDrawDataInner(commandBuffer);
    }

    void VkmImGuiRendererBase::discardFrame()
    {
        if (_frameRendered)
        {
            return;
        }
        _frameRendered = true;

        ImGui::EndFrame();
    }

    void VkmImGuiRendererBase::shutdown()
    {
        shutdownInner();
        ImGui::DestroyContext();
    }
} // namespace vkm
