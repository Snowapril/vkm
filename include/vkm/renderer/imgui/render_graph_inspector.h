// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>

#include <cstdint>

namespace vkm
{
    class VkmDriverBase;
    class VkmImGuiRendererBase;
    class VkmRenderGraphCapture;

    /*
    * @brief Engine-owned ImGui window for browsing a VkmRenderGraphCapture: pass list on
    * the left, selected-pass details (pipelines, dependencies, inputs, outputs) on the
    * right, with texture snapshot previews and a captured-buffer hex viewer. Drawn from
    * VkmEngine::update(), before the frame's first ImGui::Render() call.
    */
    class VkmRenderGraphInspector
    {
    public:
        void draw(VkmRenderGraphCapture& capture, VkmDriverBase* driver, VkmImGuiRendererBase* imGuiRenderer);

    private:
        int _selectedPass = 0;
        int _selectedBuffer = -1; // index into the selected pass's capturedBuffers, -1 = none
    };
} // namespace vkm
