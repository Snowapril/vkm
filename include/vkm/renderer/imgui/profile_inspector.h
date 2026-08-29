// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/base/cpu_profiler.h>
#include <vkm/renderer/backend/common/gpu_profiler.h>

#include <cstddef>
#include <cstdint>

namespace vkm
{
    /*
    * @brief Engine-owned ImGui window showing what VkmCpuProfiler and VkmGpuProfiler collected on
    * one shared timeline: a frame-time history strip on top, and below it a flame chart with a row
    * group per CPU thread followed by a row group per command queue.
    * @details Every duration is in milliseconds. Toggled with F6, or opened at launch with
    * --gv_profile=1; drawn from VkmEngine::update(), before the frame's first ImGui::Render() call.
    * The two halves share a time axis because VkmGpuProfiler places its zones on
    * VkmCpuProfiler::nowNs()'s clock. Each queue row group carries a lane of submit markers, one
    * per submit() the frame made, with a bar from the marker to the moment the queue actually
    * started that work -- which is the wait between the two the window exists to show. Where the
    * backend has no clock calibration that bar is suppressed and the frame is labelled estimated,
    * since anchoring GPU work to its submit makes the wait read as zero by construction.
    * Capture follows visibility, so an application that never opens the window pays nothing but one
    * relaxed atomic load per instrumented scope. Closing it stops CPU collection outright, but only
    * stops the GPU profiler keeping history: the debug overlay's always-visible "GPU" stat reads
    * the same collector.
    * With no frame pinned the newest frame whose GPU work has resolved is shown and the chart
    * updates live. Clicking a bar in the history strip pins that frame and stops capture, which
    * keeps the ring, and therefore the pinned frame, still while it is being read.
    */
    class VkmProfileInspector
    {
    public:
        // No-op while the window is hidden, or when `gpuProfiler` is null.
        void draw(VkmGpuProfiler* gpuProfiler);

        inline bool isVisible() const { return _visible; }
        inline void toggleVisible() { _visible = !_visible; }
        inline void setVisible(bool visible) { _visible = visible; }

    private:
        /*
        * @brief One frame's CPU and GPU halves, and the time span covering both.
        * @details `_hasGpu` is false for a frame whose submissions the GPU has not finished, which
        * is the normal state of the newest frames; the chart then draws the CPU rows alone.
        */
        struct DisplayFrame
        {
            VkmProfileFrame _cpu;
            VkmGpuProfileFrame _gpu;
            bool _hasGpu = false;
            uint64_t _beginNs = 0;
            uint64_t _endNs = 0;

            inline uint64_t getDurationNs() const { return (_endNs > _beginNs) ? (_endNs - _beginNs) : 0; }
        };

        void drawToolbar(VkmGpuProfiler& gpuProfiler);
        // Draws the history strip and fills `outFrame` with the frame to display. Returns false
        // when nothing has been collected yet.
        bool drawFrameHistory(VkmGpuProfiler& gpuProfiler, DisplayFrame& outFrame);
        void drawChart(const DisplayFrame& frame);
        // Draws one queue's submit markers, and the wait from each to the work it produced.
        void drawSubmitLane(const VkmGpuQueueTimeline& queue, const DisplayFrame& frame,
                            float laneY, float chartLeft, float chartWidth, bool drawLatency);
        /*
        * @brief Draws what the selected time range contains: its span, and which scopes and zones
        * spent the most time inside it.
        * @details Drawn above the chart rather than inside it, so the totals stay put while the
        * chart underneath is panned and zoomed.
        */
        void drawSelectionSummary(const DisplayFrame& frame);

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
