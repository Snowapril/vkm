// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/cpu_profiler.h>

#include <cstddef>
#include <cstdint>

namespace vkm
{
    /*
    * @brief Engine-owned ImGui window showing what VkmCpuProfiler collected: a frame-time
    * history strip on top, and below it a per-thread flame chart of the selected frame.
    * Toggled with F7; drawn from VkmEngine::update(), before the frame's first ImGui::Render()
    * call.
    *
    * Capture follows visibility -- the engine starts the profiler when this window opens and
    * stops it when it closes -- so an application that never opens the window pays nothing but
    * one relaxed atomic load per instrumented scope.
    *
    * With no frame pinned the newest collected frame is shown and the chart updates live every
    * frame. Clicking a bar in the history strip pins that frame and stops capture, which is
    * what keeps the ring (and therefore the pinned frame) still while it is being read.
    */
    class VkmCpuProfilerInspector
    {
    public:
        // No-op while the window is hidden.
        void draw();

        inline bool isVisible() const { return _visible; }
        inline void toggleVisible() { _visible = !_visible; }
        inline void setVisible(bool visible) { _visible = visible; }

    private:
        void drawToolbar();
        // Draws the history strip and returns the ring index of the frame to display, or
        // kNoFrame when the ring is empty.
        size_t drawFrameHistory();
        void drawFlameChart(const VkmProfileFrame& frame);
        /*
        * @brief Draws what the selected time range contains: its span, and which scopes spent
        * the most time inside it.
        * @details Drawn above the chart rather than inside it, so the totals stay put while
        * the chart underneath is panned and zoomed.
        */
        void drawSelectionSummary(const VkmProfileFrame& frame);

        bool _visible = false;
        // Frame number the user clicked, held only while _hasPinnedFrame. Pinning by number
        // rather than by ring index so the selection cannot silently slide onto another frame.
        bool _hasPinnedFrame = false;
        uint32_t _pinnedFrameNumber = 0;
        // Horizontal zoom of the flame chart, and the pan offset in milliseconds from the
        // displayed frame's start.
        float _pixelsPerMs = 60.0f;
        float _panMs = 0.0f;
        // Set when the displayed frame changes, to re-fit zoom/pan to the new frame's span.
        bool _fitRequested = true;

        /*
        * Time range the user dragged out across the ruler, in milliseconds from the displayed
        * frame's start -- the same units as _panMs, so it survives zooming and panning.
        *
        * Only the ruler starts a selection. Dragging inside the zone rows keeps panning the
        * chart, which is the gesture that was already there.
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
