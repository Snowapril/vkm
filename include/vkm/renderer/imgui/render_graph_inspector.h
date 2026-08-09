// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/imgui/imgui_canvas.h>
#include <vkm/renderer/imgui/texture_browser.h>

#include <cstdint>
#include <string>

namespace vkm
{
    class VkmDriverBase;
    class VkmGpuProfiler;
    class VkmImGuiRendererBase;
    class VkmPipelineStateManager;
    class VkmRenderGraphCapture;

    /*
    * @brief Engine-owned ImGui debug window (F5), drawn from VkmEngine::update() before the
    * frame's first ImGui::Render() call. Four tabs:
    *
    *   Capture   -- the VkmRenderGraphCapture: pass list, per-pass pipelines, dependencies,
    *                inputs and outputs with texture previews, and a captured-buffer hex view.
    *   Graph     -- the same passes as a node-link diagram, laid out in dependency levels.
    *   Pipelines -- every loaded PSO json, with staleness marking and reload buttons.
    *   Textures  -- VkmTextureBrowser over every live texture in the resource pool.
    *
    * The Capture and Graph tabs share one selection, so a node picked in one is the pass detailed
    * in the other.
    */
    class VkmRenderGraphInspector
    {
    public:
        /*
        * @brief Draws the window.
        * @param capture Capture the Capture and Graph tabs read.
        * @param driver Driver the Textures tab browses.
        * @param imGuiRenderer Renderer that turns texture handles into ImGui texture ids.
        * @param pipelineStateManager Manager the Pipelines tab lists and reloads.
        * @param gpuProfiler Source of the Graph tab's per-subgraph averages. May be null, and on a
        * backend without timestamp support the averages simply stay empty.
        */
        void draw(VkmRenderGraphCapture& capture, VkmDriverBase* driver,
                  VkmImGuiRendererBase* imGuiRenderer, VkmPipelineStateManager* pipelineStateManager,
                  const VkmGpuProfiler* gpuProfiler);

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
        /*
        * @brief Draws the captured passes as a node-link diagram: one node per subgraph, one edge
        * per dependency the render graph's analysis found.
        * @details Nodes are placed in columns by dependency level -- level 0 depends on nothing,
        * and a node sits one column right of its deepest producer -- so every edge points left to
        * right and the columns read as "what can run at the same time". Each node carries its
        * rolling average GPU time and the render targets it reads and writes.
        * @param capture Capture whose passes are drawn.
        * @param gpuProfiler Source of the averages, or null for none.
        */
        void drawGraphTab(VkmRenderGraphCapture& capture, const VkmGpuProfiler* gpuProfiler);
        void drawPipelinesTab(VkmPipelineStateManager* pipelineStateManager);
        // Reloads `sourceIndex` and records the outcome for the Pipelines tab's error box.
        void reloadSource(VkmPipelineStateManager* pipelineStateManager, size_t sourceIndex);

        VkmTextureBrowser _textureBrowser;
        bool _visible = true;
        int _selectedPass = 0;
        int _selectedBuffer = -1; // index into the selected pass's capturedBuffers, -1 = none
        VkmImGuiCanvas _graphCanvas;
        // Draw only the selected node's own edges, for a frame with more edges than can be read.
        bool _graphIsolateSelection = false;
        // List each node's render-target inputs and outputs, which is what makes the nodes tall.
        bool _graphShowResources = true;
        // Passes the view was last framed for, so a capture of a different graph is re-framed.
        size_t _graphFittedPassCount = 0;
        bool _recompileShadersOnReload = true;
        std::string _reloadStatus; // last reload's result, shown under the source table
        bool _reloadFailed = false;
    };
} // namespace vkm
