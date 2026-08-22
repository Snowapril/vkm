// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/renderer/backend/common/gpu_profiler.h>

#include <cstddef>
#include <cstdint>

namespace vkm
{
    /*
    * @brief Engine-owned ImGui window showing what VkmGpuProfiler collected: a GPU frame-time
    * history strip on top, and below it one timeline per command queue for the selected frame,
    * with the whole submission on the top row and each render graph subgraph under it.
    * @details Every duration is in milliseconds. Toggled with F6; drawn from VkmEngine::update(),
    * before the frame's first ImGui::Render() call.
    * The CPU counterpart is VkmCpuProfilerInspector, and this window behaves the same way: same
    * history strip, same pin-a-frame gesture, same zoom/pan/drag-to-select interaction, and,
    * through profiler_chart_common.h, the same color for a given zone name.
    * Unlike the CPU profiler, closing the window does not stop the GPU profiler recording
    * timestamps, the debug overlay's always-visible "GPU" stat reading the same collector. Capture
    * here only controls whether resolved frames are kept for history.
    */
    class VkmGpuProfilerInspector
    {
    public:
        // No-op while the window is hidden, or when `profiler` is null.
        void draw(VkmGpuProfiler* profiler);

        inline bool isVisible() const { return _visible; }
        inline void toggleVisible() { _visible = !_visible; }
        inline void setVisible(bool visible) { _visible = visible; }

    private:
        void drawToolbar(VkmGpuProfiler& profiler);
        // Draws the history strip and returns the ring index of the frame to display, or
        // kNoFrame when the ring is empty.
        size_t drawFrameHistory(VkmGpuProfiler& profiler);
        void drawQueueTimelines(const VkmGpuProfileFrame& frame);
        /*
        * @brief Draws what the selected time range contains: its span, and which subgraphs spent
        * the most GPU time inside it.
        * @details Drawn above the chart rather than inside it, so the totals stay put while the
        * chart underneath is panned and zoomed.
        */
        void drawSelectionSummary(const VkmGpuProfileFrame& frame);

        bool _visible = false;
        // Frame number the user clicked, held only while _hasPinnedFrame. Pinning by number
        // rather than by ring index so the selection cannot silently slide onto another frame.
        bool _hasPinnedFrame = false;
        uint32_t _pinnedFrameNumber = 0;
        // Horizontal zoom of the chart, and the pan offset in milliseconds from the displayed
        // frame's start.
        float _pixelsPerMs = 60.0f;
        float _panMs = 0.0f;
        // Set when the displayed frame changes, to re-fit zoom/pan to the new frame's span.
        bool _fitRequested = true;

        /*
        * Time range the user dragged out across the ruler, in milliseconds from the displayed
        * frame's start -- the same units as _panMs, so it survives zooming and panning.
        *
        * Only the ruler starts a selection. Dragging inside the zone rows keeps panning the
        * chart.
        */
        bool _hasSelection = false;
        bool _draggingSelection = false;
        // Where the drag started; the range is this and the cursor, in whichever order.
        float _selectionAnchorMs = 0.0f;
        float _selectionStartMs = 0.0f;
        float _selectionEndMs = 0.0f;
        // Raised by the summary's button, which is drawn before the chart that acts on it.
        bool _zoomToSelectionRequested = false;

        static constexpr size_t kNoFrame = static_cast<size_t>(-1);
    };
} // namespace vkm
