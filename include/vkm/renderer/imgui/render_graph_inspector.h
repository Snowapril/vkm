// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/imgui/texture_browser.h>

#include <cstdint>
#include <string>

namespace vkm
{
    class VkmDriverBase;
    class VkmImGuiRendererBase;
    class VkmPipelineStateManager;
    class VkmRenderGraphCapture;

    /*
    * @brief Engine-owned ImGui debug window (F7), drawn from VkmEngine::update() before the
    * frame's first ImGui::Render() call. Three tabs:
    *
    *   Capture   -- the VkmRenderGraphCapture: pass list, per-pass pipelines, dependencies,
    *                inputs and outputs with texture previews, and a captured-buffer hex view.
    *   Pipelines -- every loaded PSO json, with staleness marking and reload buttons.
    *   Textures  -- VkmTextureBrowser over every live texture in the resource pool.
    */
    class VkmRenderGraphInspector
    {
    public:
        void draw(VkmRenderGraphCapture& capture, VkmDriverBase* driver,
                  VkmImGuiRendererBase* imGuiRenderer, VkmPipelineStateManager* pipelineStateManager);

        void toggleVisible() { _visible = !_visible; }
        bool isVisible() const { return _visible; }

        // Releases GPU resources owned by the sub-views. Called from VkmEngine::destroy().
        void releaseResources(VkmDriverBase* driver) { _textureBrowser.releaseResources(driver); }

        /*
        * @brief Textures this window emitted an ImGui::Image() for during the last draw().
        * The engine references them on the ImGui overlay subgraph so their GPU usage stays
        * tracked while those draws are in flight.
        */
        const std::vector<VkmResourceHandle>& getTexturesSampledLastDraw() const
        {
            return _textureBrowser.getTexturesSampledLastDraw();
        }

    private:
        void drawCaptureTab(VkmRenderGraphCapture& capture, VkmImGuiRendererBase* imGuiRenderer,
                            VkmPipelineStateManager* pipelineStateManager);
        void drawPipelinesTab(VkmPipelineStateManager* pipelineStateManager);
        // Reloads `sourceIndex` and records the outcome for the Pipelines tab's error box.
        void reloadSource(VkmPipelineStateManager* pipelineStateManager, size_t sourceIndex);

        VkmTextureBrowser _textureBrowser;
        bool _visible = true;
        int _selectedPass = 0;
        int _selectedBuffer = -1; // index into the selected pass's capturedBuffers, -1 = none
        bool _recompileShadersOnReload = true;
        std::string _reloadStatus; // last reload's result, shown under the source table
        bool _reloadFailed = false;
    };
} // namespace vkm
