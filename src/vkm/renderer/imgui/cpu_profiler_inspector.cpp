// Copyright (c) 2026 Snowapril

#include <vkm/renderer/imgui/cpu_profiler_inspector.h>

#if defined(ENABLE_CHROME_TRACING)
#include <vkm/base/common.h>
#endif // ENABLE_CHROME_TRACING

#include "profiler_chart_common.h"

#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace vkm
{
    using namespace profilerChart;

    namespace
    {
        // Largest depth used by any zone in the timeline, so the caller can reserve rows.
        uint16_t maxDepth(const VkmProfileThreadTimeline& timeline)
        {
            uint16_t depth = 0;
            for (const VkmProfileZone& zone : timeline._zones)
            {
                depth = std::max(depth, zone._depth);
            }
            return depth;
        }
    } // namespace

    void VkmCpuProfilerInspector::draw()
    {
        if (!_visible)
        {
            return;
        }

        ImGui::SetNextWindowSize(ImVec2(900, 520), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("CPU Profiler", &_visible))
        {
            ImGui::End();
            return;
        }

        drawToolbar();
        ImGui::Separator();

        const size_t displayIndex = drawFrameHistory();

        VkmProfileFrame frame;
        if (displayIndex != kNoFrame && VkmCpuProfiler::singleton().copyFrame(displayIndex, frame))
        {
            drawFlameChart(frame);
        }
        else
        {
            ImGui::TextDisabled("No frames collected yet.");
        }

        ImGui::End();
    }

    void VkmCpuProfilerInspector::drawToolbar()
    {
        VkmCpuProfiler& profiler = VkmCpuProfiler::singleton();
        const bool capturing = profiler.isCapturing();

        if (ImGui::Button(capturing ? "Stop capture" : "Start capture"))
        {
            // Resuming clears the ring (see VkmCpuProfiler::setCapturing), so drop the pin too
            // rather than leaving it pointing at a frame number that no longer exists.
            profiler.setCapturing(!capturing);
            _hasPinnedFrame = false;
        }

        ImGui::SameLine();
        if (ImGui::Button("Clear"))
        {
            profiler.clear();
            _hasPinnedFrame = false;
        }

#if defined(ENABLE_CHROME_TRACING)
        ImGui::SameLine();
        if (ImGui::Button("Export Chrome trace"))
        {
            // Lives here rather than behind a hotkey because capture is tied to this window
            // being open -- a global key pressed with the window closed would export nothing.
            static constexpr const char* kChromeTracePath = "vkm_cpu_trace.json";
            if (profiler.exportChromeTrace(kChromeTracePath))
            {
                VKM_DEBUG_LOG("Wrote CPU profile to vkm_cpu_trace.json");
            }
            else
            {
                VKM_DEBUG_ERROR("Failed to write vkm_cpu_trace.json");
            }
        }
        ImGui::SetItemTooltip("Write the collected frames to vkm_cpu_trace.json in the working "
                              "directory; open it in chrome://tracing or ui.perfetto.dev.");
#endif // ENABLE_CHROME_TRACING

        ImGui::SameLine();
        if (_hasPinnedFrame)
        {
            if (ImGui::Button("Go live"))
            {
                _hasPinnedFrame = false;
                profiler.setCapturing(true);
            }
            ImGui::SameLine();
            ImGui::Text("pinned frame #%u", _pinnedFrameNumber);
        }
        else
        {
            ImGui::TextDisabled(capturing ? "live" : "stopped");
        }

        ImGui::SameLine();
        if (ImGui::Button("Fit"))
        {
            _fitRequested = true;
        }
        ImGui::SetItemTooltip("Reset the flame chart's zoom and pan to span the whole frame.");

        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0f);
        ImGui::SliderFloat("Zoom", &_pixelsPerMs, 1.0f, 20000.0f, "%.0f px/ms", ImGuiSliderFlags_Logarithmic);
    }

    size_t VkmCpuProfilerInspector::drawFrameHistory()
    {
        const std::vector<VkmProfileFrameSummary> summaries = VkmCpuProfiler::singleton().copyFrameSummaries();

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
        ImGui::InvisibleButton("##frameHistory", ImVec2(width, kHistoryHeight));

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 bottomRight(origin.x + width, origin.y + kHistoryHeight);
        drawList->AddRectFilled(origin, bottomRight, IM_COL32(24, 24, 30, 255));

        if (summaries.empty())
        {
            drawList->AddText(ImVec2(origin.x + 6.0f, origin.y + 6.0f), IM_COL32(140, 140, 150, 255),
                              "no frames");
            return kNoFrame;
        }

        // Fixed slot width (rather than width / summaries.size()) so bars keep their size as
        // the ring fills and then scroll, instead of continuously squashing.
        const float slotWidth = std::max(width / static_cast<float>(VkmCpuProfiler::kMaxFrameHistory), 1.0f);

        float peakMs = kFrameBudgetMs;
        for (const VkmProfileFrameSummary& summary : summaries)
        {
            peakMs = std::max(peakMs, nsToMs(summary._durationNs));
        }

        // Budget line, so an over-budget frame is obvious without reading the tooltip.
        const float budgetY = bottomRight.y - (kFrameBudgetMs / peakMs) * kHistoryHeight;
        drawList->AddLine(ImVec2(origin.x, budgetY), ImVec2(bottomRight.x, budgetY), IM_COL32(90, 90, 110, 255));

        int hoveredIndex = -1;
        if (ImGui::IsItemHovered())
        {
            const int index = static_cast<int>((ImGui::GetIO().MousePos.x - origin.x) / slotWidth);
            if (index >= 0 && index < static_cast<int>(summaries.size()))
            {
                hoveredIndex = index;
            }
        }

        size_t displayIndex = summaries.size() - 1;
        for (size_t i = 0; i < summaries.size(); ++i)
        {
            const float durationMs = nsToMs(summaries[i]._durationNs);
            const float barHeight = std::max((durationMs / peakMs) * kHistoryHeight, 1.0f);
            const float x0 = origin.x + static_cast<float>(i) * slotWidth;
            const float x1 = x0 + std::max(slotWidth - 1.0f, 1.0f);

            ImU32 color = (durationMs > kFrameBudgetMs) ? IM_COL32(214, 106, 96, 255) : IM_COL32(96, 168, 214, 255);
            if (_hasPinnedFrame && summaries[i]._frameNumber == _pinnedFrameNumber)
            {
                color = IM_COL32(240, 220, 120, 255);
                displayIndex = i;
            }
            if (static_cast<int>(i) == hoveredIndex)
            {
                color = IM_COL32(255, 255, 255, 255);
            }

            drawList->AddRectFilled(ImVec2(x0, bottomRight.y - barHeight), ImVec2(x1, bottomRight.y), color);
        }

        if (hoveredIndex >= 0)
        {
            const VkmProfileFrameSummary& summary = summaries[static_cast<size_t>(hoveredIndex)];
            ImGui::SetTooltip("frame #%u\n%.3f ms", summary._frameNumber, nsToMs(summary._durationNs));

            if (ImGui::IsItemClicked())
            {
                // Pinning stops capture; otherwise the ring would keep rolling and eventually
                // drop the very frame the user asked to look at.
                _hasPinnedFrame = true;
                _pinnedFrameNumber = summary._frameNumber;
                _fitRequested = true;
                // The range was measured against a different frame's timeline; carrying it
                // over would show totals for zones that are no longer on screen.
                _hasSelection = false;
                VkmCpuProfiler::singleton().setCapturing(false);
            }
        }

        if (_hasPinnedFrame && summaries[displayIndex]._frameNumber != _pinnedFrameNumber)
        {
            // The pinned frame aged out of the ring (only reachable if capture was restarted
            // from elsewhere); fall back to the newest frame.
            _hasPinnedFrame = false;
        }

        return displayIndex;
    }

    void VkmCpuProfilerInspector::drawSelectionSummary(const VkmProfileFrame& frame)
    {
        if (!_hasSelection)
        {
            ImGui::TextDisabled("Drag across the time ruler to measure a range.");
            return;
        }

        // The chart lets the view pan past both ends of the frame, so a drag can reach beyond
        // where any zone exists; clamping keeps the reported span honest.
        const float frameMs = nsToMs(frame.getDurationNs());
        const float beginMs = std::clamp(_selectionStartMs, 0.0f, frameMs);
        const float endMs = std::clamp(_selectionEndMs, 0.0f, frameMs);
        const float spanMs = endMs - beginMs;

        const uint64_t beginNs = frame._beginNs + static_cast<uint64_t>(static_cast<double>(beginMs) * 1e6);
        const uint64_t endNs = frame._beginNs + static_cast<uint64_t>(static_cast<double>(endMs) * 1e6);
        const std::vector<VkmProfileScopeTotal> totals = vkmAggregateProfileRange(frame, beginNs, endNs);

        ImGui::Text("Selection %.4f ms", spanMs);
        ImGui::SameLine();
        ImGui::TextDisabled("(%.4f - %.4f ms)", beginMs, endMs);

        ImGui::SameLine();
        if (ImGui::SmallButton("Zoom to selection"))
        {
            _zoomToSelectionRequested = true;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Clear"))
        {
            _hasSelection = false;
        }

        if (totals.empty())
        {
            ImGui::TextDisabled("No scopes ran in this range.");
            return;
        }

        constexpr ImGuiTableFlags kTableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
        const int rowCount = std::min(kMaxSelectionRows, static_cast<int>(totals.size()));
        const float tableHeight = static_cast<float>(rowCount + 1) * ImGui::GetTextLineHeightWithSpacing();
        if (ImGui::BeginTable("##selectionTotals", 4, kTableFlags, ImVec2(0.0f, tableHeight)))
        {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Scope");
            ImGui::TableSetupColumn("Total");
            ImGui::TableSetupColumn("Calls");
            ImGui::TableSetupColumn("% of range");
            ImGui::TableHeadersRow();

            for (int row = 0; row < rowCount; ++row)
            {
                const VkmProfileScopeTotal& entry = totals[static_cast<size_t>(row)];
                const float totalMs = nsToMs(entry._totalNs);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(entry._name);
                ImGui::TableNextColumn();
                ImGui::Text("%.4f ms", totalMs);
                ImGui::TableNextColumn();
                ImGui::Text("%u", entry._count);
                ImGui::TableNextColumn();
                ImGui::Text("%.1f%%", (spanMs > 0.0f) ? (totalMs / spanMs) * 100.0f : 0.0f);
            }
            ImGui::EndTable();
        }

        if (static_cast<int>(totals.size()) > rowCount)
        {
            ImGui::TextDisabled("Showing %d of %zu scopes (longest first).", rowCount, totals.size());
        }
        ImGui::TextDisabled("Inclusive of nested scopes and summed across threads, so the "
                            "percentages can exceed 100%%.");
    }

    void VkmCpuProfilerInspector::drawFlameChart(const VkmProfileFrame& frame)
    {
        const float frameMs = std::max(nsToMs(frame.getDurationNs()), 0.001f);

        ImGui::Text("Frame #%u  %.3f ms  %zu thread(s)", frame._frameNumber, frameMs, frame._threads.size());

        drawSelectionSummary(frame);

        ImGui::BeginChild("##flameChart", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        const float chartWidth = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
        // Stay pending until a frame with real content arrives: the first frame of a capture is
        // always empty (beginFrame() closes a frame that nothing was recorded into yet), and
        // fitting to its zero duration would leave the chart zoomed in by six orders of magnitude.
        if (_fitRequested && frame.getDurationNs() > 0)
        {
            _pixelsPerMs = chartWidth / frameMs;
            _panMs = 0.0f;
            _fitRequested = false;
        }

        const float chartLeft = ImGui::GetCursorScreenPos().x;

        // Ruler geometry has to be captured before the strip is emitted: emitting it advances
        // the cursor, and chartBottom is measured from what is left below it.
        const ImVec2 rulerOrigin = ImGui::GetCursorScreenPos();
        const float rulerBottom = rulerOrigin.y + kRulerHeight;
        const float chartBottom = rulerOrigin.y + ImGui::GetContentRegionAvail().y;

        // The ruler is the one place a drag selects a range; everywhere else a drag still pans,
        // which is the gesture this chart already had.
        ImGui::SetCursorScreenPos(rulerOrigin);
        ImGui::InvisibleButton("##ruler", ImVec2(chartWidth, kRulerHeight));
        const bool rulerActive = ImGui::IsItemActive();
        const float msAtMouse = _panMs + (ImGui::GetIO().MousePos.x - chartLeft) / _pixelsPerMs;

        if (ImGui::IsItemActivated())
        {
            _selectionAnchorMs = msAtMouse;
            _draggingSelection = true;
            _hasSelection = false;
        }
        if (rulerActive && _draggingSelection)
        {
            _selectionStartMs = std::min(_selectionAnchorMs, msAtMouse);
            _selectionEndMs = std::max(_selectionAnchorMs, msAtMouse);
            // A click that never moved is how the selection is cleared.
            _hasSelection = (_selectionEndMs - _selectionStartMs) * _pixelsPerMs >= kMinSelectionWidthPx;
        }
        if (ImGui::IsItemDeactivated())
        {
            _draggingSelection = false;
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            ImGui::SetTooltip("Drag to select a time range");
        }

        if (_hasSelection && _zoomToSelectionRequested)
        {
            const float selectionMs = std::max(_selectionEndMs - _selectionStartMs, 1e-4f);
            _pixelsPerMs = std::clamp(chartWidth / selectionMs, 1.0f, 20000.0f);
            _panMs = _selectionStartMs;
        }
        _zoomToSelectionRequested = false;

        // Wheel zooms about the cursor (so the scope under the pointer stays put), left-drag pans.
        // Suppressed while the ruler owns the drag, so selecting cannot also pan the chart out
        // from under the selection.
        if (!rulerActive && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
        {
            const float wheel = ImGui::GetIO().MouseWheel;
            if (wheel != 0.0f)
            {
                const float cursorOffsetPx = ImGui::GetIO().MousePos.x - chartLeft;
                const float msAtCursor = _panMs + cursorOffsetPx / _pixelsPerMs;
                _pixelsPerMs = std::clamp(_pixelsPerMs * ((wheel > 0.0f) ? 1.25f : 0.8f), 1.0f, 20000.0f);
                _panMs = msAtCursor - cursorOffsetPx / _pixelsPerMs;
            }

            if (ImGui::IsMouseDragging(ImGuiMouseButton_Left))
            {
                _panMs -= ImGui::GetMouseDragDelta(ImGuiMouseButton_Left).x / _pixelsPerMs;
                ImGui::ResetMouseDragDelta(ImGuiMouseButton_Left);
            }
        }
        _panMs = std::clamp(_panMs, -frameMs, frameMs);

        ImDrawList* drawList = ImGui::GetWindowDrawList();

        // Time ruler along the top, plus the gridlines it labels.
        const float gridStepMs = chooseGridStepMs(_pixelsPerMs);
        for (float gridMs = std::floor(_panMs / gridStepMs) * gridStepMs;
             (gridMs - _panMs) * _pixelsPerMs < chartWidth; gridMs += gridStepMs)
        {
            const float x = chartLeft + (gridMs - _panMs) * _pixelsPerMs;
            if (x < chartLeft)
            {
                continue;
            }
            drawList->AddLine(ImVec2(x, rulerBottom), ImVec2(x, chartBottom), IM_COL32(60, 60, 70, 255));

            char label[32];
            std::snprintf(label, sizeof(label), (gridStepMs < 0.1f) ? "%.3f ms" : "%.2f ms", gridMs);
            drawList->AddText(ImVec2(x + 3.0f, rulerOrigin.y + 2.0f), IM_COL32(150, 150, 160, 255), label);
        }

        for (const VkmProfileThreadTimeline& timeline : frame._threads)
        {
            ImGui::SeparatorText(timeline._threadName.c_str());
            if (timeline._overflowed)
            {
                ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.3f, 1.0f),
                                   "zone limit reached; this frame's timeline is incomplete");
            }

            const ImVec2 rowsOrigin = ImGui::GetCursorScreenPos();
            const float rowsHeight = static_cast<float>(maxDepth(timeline) + 1) * (kZoneRowHeight + kZoneRowGap);

            for (const VkmProfileZone& zone : timeline._zones)
            {
                const float beginMs = nsToMs(zone._beginNs - frame._beginNs);
                const float endMs = nsToMs(zone._endNs - frame._beginNs);
                const float x0 = chartLeft + (beginMs - _panMs) * _pixelsPerMs;
                const float x1 = std::max(chartLeft + (endMs - _panMs) * _pixelsPerMs, x0 + 1.0f);
                if (x1 < chartLeft || x0 > chartLeft + chartWidth)
                {
                    continue;
                }

                const float y0 = rowsOrigin.y + static_cast<float>(zone._depth) * (kZoneRowHeight + kZoneRowGap);
                const float y1 = y0 + kZoneRowHeight;
                const ImVec2 topLeft(x0, y0);
                const ImVec2 bottomRight(x1, y1);

                drawList->AddRectFilled(topLeft, bottomRight, zoneColor(zone._name));

                if (x1 - x0 >= kMinLabelWidth)
                {
                    drawList->PushClipRect(ImVec2(x0 + 2.0f, y0), ImVec2(x1 - 2.0f, y1), true);
                    drawList->AddText(ImVec2(x0 + 4.0f, y0 + 2.0f), IM_COL32(16, 16, 20, 255), zone._name);
                    drawList->PopClipRect();
                }

                if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                    ImGui::IsMouseHoveringRect(topLeft, bottomRight))
                {
                    ImGui::SetTooltip("%s\n%.4f ms\nstart %.4f ms  depth %u", zone._name, endMs - beginMs, beginMs,
                                      static_cast<unsigned int>(zone._depth));
                }
            }

            ImGui::Dummy(ImVec2(chartWidth, rowsHeight));
        }

        // Last, so the band reads on top of the zones it covers rather than under them.
        if (_hasSelection)
        {
            const float x0 = chartLeft + (_selectionStartMs - _panMs) * _pixelsPerMs;
            const float x1 = chartLeft + (_selectionEndMs - _panMs) * _pixelsPerMs;
            drawList->AddRectFilled(ImVec2(x0, rulerOrigin.y), ImVec2(x1, chartBottom),
                                    IM_COL32(240, 220, 120, 36));
            drawList->AddLine(ImVec2(x0, rulerOrigin.y), ImVec2(x0, chartBottom), IM_COL32(240, 220, 120, 200));
            drawList->AddLine(ImVec2(x1, rulerOrigin.y), ImVec2(x1, chartBottom), IM_COL32(240, 220, 120, 200));
        }

        ImGui::EndChild();
    }
} // namespace vkm
