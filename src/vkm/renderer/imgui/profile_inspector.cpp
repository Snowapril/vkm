// Copyright (c) 2026 Snowapril

#include <vkm/renderer/imgui/profile_inspector.h>
#include <vkm/base/common.h>

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
        template <typename TimelineType>
        uint16_t maxDepth(const TimelineType& timeline)
        {
            uint16_t depth = 0;
            for (const auto& zone : timeline._zones)
            {
                depth = std::max(depth, zone._depth);
            }
            return depth;
        }

        const char* queueTypeName(const VkmCommandQueueType queueType)
        {
            switch (queueType)
            {
                case VkmCommandQueueType::Graphics: return "Graphics";
                case VkmCommandQueueType::Compute:  return "Compute";
                case VkmCommandQueueType::Transfer: return "Transfer";
                default:                            return "Unknown";
            }
        }

        const char* correlationName(const VkmGpuClockCorrelation correlation)
        {
            switch (correlation)
            {
                case VkmGpuClockCorrelation::Calibrated: return "calibrated";
                case VkmGpuClockCorrelation::Estimated:  return "estimated";
                default:                                 return "uncorrelated";
            }
        }
    } // namespace

    void VkmProfileInspector::draw(VkmGpuProfiler* gpuProfiler)
    {
        if (!_visible || gpuProfiler == nullptr)
        {
            return;
        }

        ImGui::SetNextWindowSize(ImVec2(1000, 620), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Profile", &_visible))
        {
            ImGui::End();
            return;
        }

        drawToolbar(*gpuProfiler);
        ImGui::Separator();

        DisplayFrame frame;
        if (drawFrameHistory(*gpuProfiler, frame))
        {
            drawChart(frame);
        }
        else
        {
            ImGui::TextDisabled("No frames collected yet.");
        }

        ImGui::End();
    }

    void VkmProfileInspector::drawToolbar(VkmGpuProfiler& gpuProfiler)
    {
        VkmCpuProfiler& cpuProfiler = VkmCpuProfiler::singleton();
        const bool capturing = cpuProfiler.isCapturing();

        if (ImGui::Button(capturing ? "Stop capture" : "Start capture"))
        {
            // Both rings are armed together, which is what keeps their frame numbers describing
            // the same frame-loop iterations. Resuming clears them, so drop the pin too rather
            // than leaving it pointing at a frame number that no longer exists.
            cpuProfiler.setCapturing(!capturing);
            gpuProfiler.setCapturing(!capturing);
            _hasPinnedFrame = false;
        }

        ImGui::SameLine();
        if (ImGui::Button("Clear"))
        {
            cpuProfiler.clear();
            gpuProfiler.clear();
            _hasPinnedFrame = false;
        }

        ImGui::SameLine();
        if (_hasPinnedFrame)
        {
            if (ImGui::Button("Go live"))
            {
                _hasPinnedFrame = false;
                cpuProfiler.setCapturing(true);
                gpuProfiler.setCapturing(true);
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
        ImGui::SetItemTooltip("Reset the chart's zoom and pan to span the whole frame.");

        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0f);
        ImGui::SliderFloat("Zoom", &_pixelsPerMs, 1.0f, 20000.0f, "%.0f px/ms", ImGuiSliderFlags_Logarithmic);

        if (!gpuProfiler.isSupported())
        {
            // A capability, not a failure -- say so plainly rather than leaving the GPU half of
            // the chart looking like the device did no work.
            ImGui::TextDisabled("This backend does not support GPU timestamp queries; CPU rows only.");
            return;
        }

        const VkmGpuClockCorrelation correlation = gpuProfiler.getClockCorrelation();
        if (correlation == VkmGpuClockCorrelation::Calibrated)
        {
            ImGui::TextDisabled("GPU clock: calibrated");
            ImGui::SetItemTooltip("GPU timestamps are anchored to a clock pair sampled from the "
                                  "driver, so the wait between a submit and the queue starting "
                                  "that work is measured.");
        }
        else
        {
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.3f, 1.0f), "GPU clock: estimated");
            ImGui::SetItemTooltip("This backend has no CPU/GPU clock calibration, so each "
                                  "submission's GPU work is drawn from the moment it was "
                                  "submitted. Durations are exact; queue latency is not measured.");
        }
    }

    bool VkmProfileInspector::drawFrameHistory(VkmGpuProfiler& gpuProfiler, DisplayFrame& outFrame)
    {
        VkmCpuProfiler& cpuProfiler = VkmCpuProfiler::singleton();
        const std::vector<VkmProfileFrameSummary> summaries = cpuProfiler.copyFrameSummaries();

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
        ImGui::InvisibleButton("##profileFrameHistory", ImVec2(width, kHistoryHeight));

        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const ImVec2 bottomRight(origin.x + width, origin.y + kHistoryHeight);
        drawList->AddRectFilled(origin, bottomRight, IM_COL32(24, 24, 30, 255));

        if (summaries.empty())
        {
            drawList->AddText(ImVec2(origin.x + 6.0f, origin.y + 6.0f), IM_COL32(140, 140, 150, 255),
                              "no frames");
            return false;
        }

        // The CPU ring drives the strip: it records one frame per loop iteration, while the GPU
        // ring only gains an entry once a submission retires into it.
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

        // Live, the newest frames have no GPU half yet -- their submissions are still in flight.
        // Showing the newest frame that does keeps the bottom half of the window from being
        // permanently empty, at a lag of a frame or two.
        size_t displayIndex = summaries.size() - 1;
        VkmGpuProfileFrame gpuFrame;
        bool hasGpu = false;
        if (!_hasPinnedFrame)
        {
            for (size_t i = summaries.size(); i-- > 0;)
            {
                if (gpuProfiler.copyFrameByNumber(summaries[i]._frameNumber, gpuFrame))
                {
                    displayIndex = i;
                    hasGpu = true;
                    break;
                }
            }
        }

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

            // The frame's GPU time as a tick within the same slot, in green, so one strip carries
            // both without a second row to correlate by eye.
            VkmGpuProfileFrame barGpuFrame;
            if (gpuProfiler.copyFrameByNumber(summaries[i]._frameNumber, barGpuFrame))
            {
                const float gpuMs = nsToMs(barGpuFrame.getDurationNs());
                const float gpuY = bottomRight.y - std::max((gpuMs / peakMs) * kHistoryHeight, 1.0f);
                drawList->AddLine(ImVec2(x0, gpuY), ImVec2(x1, gpuY), IM_COL32(110, 190, 130, 255), 2.0f);
            }
        }

        if (hoveredIndex >= 0)
        {
            const VkmProfileFrameSummary& summary = summaries[static_cast<size_t>(hoveredIndex)];
            VkmGpuProfileFrame hoveredGpu;
            if (gpuProfiler.copyFrameByNumber(summary._frameNumber, hoveredGpu))
            {
                ImGui::SetTooltip("frame #%u\nCPU %.3f ms\nGPU %.3f ms", summary._frameNumber,
                                  nsToMs(summary._durationNs), nsToMs(hoveredGpu.getDurationNs()));
            }
            else
            {
                ImGui::SetTooltip("frame #%u\nCPU %.3f ms\nGPU not resolved yet", summary._frameNumber,
                                  nsToMs(summary._durationNs));
            }

            if (ImGui::IsItemClicked())
            {
                // Pinning stops capture; otherwise the rings would keep rolling and eventually
                // drop the very frame the user asked to look at.
                _hasPinnedFrame = true;
                _pinnedFrameNumber = summary._frameNumber;
                _fitRequested = true;
                // The range was measured against a different frame's timeline; carrying it over
                // would show totals for zones that are no longer on screen.
                _hasSelection = false;
                VkmCpuProfiler::singleton().setCapturing(false);
                gpuProfiler.setCapturing(false);
            }
        }

        if (_hasPinnedFrame && summaries[displayIndex]._frameNumber != _pinnedFrameNumber)
        {
            // The pinned frame aged out of the ring (only reachable if capture was restarted
            // from elsewhere); fall back to the newest frame.
            _hasPinnedFrame = false;
        }

        if (!cpuProfiler.copyFrame(displayIndex, outFrame._cpu))
        {
            return false;
        }
        if (_hasPinnedFrame)
        {
            hasGpu = gpuProfiler.copyFrameByNumber(outFrame._cpu._frameNumber, gpuFrame);
        }
        if (hasGpu)
        {
            outFrame._gpu = std::move(gpuFrame);
            outFrame._hasGpu = true;
        }

        // The span covers both halves: GPU work submitted this frame usually finishes after the
        // CPU frame that submitted it has closed, and clipping it would hide exactly the overhang
        // this window exists to show.
        outFrame._beginNs = outFrame._cpu._beginNs;
        outFrame._endNs = outFrame._cpu._endNs;
        if (outFrame._hasGpu && outFrame._gpu.getDurationNs() > 0)
        {
            outFrame._beginNs = std::min(outFrame._beginNs, outFrame._gpu._beginNs);
            outFrame._endNs = std::max(outFrame._endNs, outFrame._gpu._endNs);
        }
        return true;
    }

    void VkmProfileInspector::drawSelectionSummary(const DisplayFrame& frame)
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

        const std::vector<VkmProfileScopeTotal> cpuTotals =
            vkmAggregateProfileRange(frame._cpu, beginNs, endNs);
        const std::vector<VkmGpuProfileZoneTotal> gpuTotals =
            frame._hasGpu ? vkmAggregateGpuProfileRange(frame._gpu, beginNs, endNs)
                          : std::vector<VkmGpuProfileZoneTotal>{};

        constexpr ImGuiTableFlags kTableFlags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                                ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY;
        const int cpuRows = std::min(kMaxSelectionRows, static_cast<int>(cpuTotals.size()));
        const int gpuRows = std::min(kMaxSelectionRows, static_cast<int>(gpuTotals.size()));
        const float tableHeight =
            static_cast<float>(std::max(std::max(cpuRows, gpuRows), 1) + 1) * ImGui::GetTextLineHeightWithSpacing();

        // Side by side, and fed the same range -- which only means anything because both halves
        // are on one clock.
        if (ImGui::BeginTable("##selectionSplit", 2, ImGuiTableFlags_SizingStretchSame))
        {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (cpuTotals.empty())
            {
                ImGui::TextDisabled("No CPU work in this range.");
            }
            else if (ImGui::BeginTable("##cpuSelectionTotals", 4, kTableFlags, ImVec2(0.0f, tableHeight)))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("CPU scope");
                ImGui::TableSetupColumn("Total");
                ImGui::TableSetupColumn("Calls");
                ImGui::TableSetupColumn("%");
                ImGui::TableHeadersRow();
                for (int row = 0; row < cpuRows; ++row)
                {
                    const VkmProfileScopeTotal& entry = cpuTotals[static_cast<size_t>(row)];
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

            ImGui::TableNextColumn();
            if (gpuTotals.empty())
            {
                ImGui::TextDisabled("No GPU work in this range.");
            }
            else if (ImGui::BeginTable("##gpuSelectionTotals", 4, kTableFlags, ImVec2(0.0f, tableHeight)))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("GPU zone");
                ImGui::TableSetupColumn("Total");
                ImGui::TableSetupColumn("Count");
                ImGui::TableSetupColumn("%");
                ImGui::TableHeadersRow();
                for (int row = 0; row < gpuRows; ++row)
                {
                    const VkmGpuProfileZoneTotal& entry = gpuTotals[static_cast<size_t>(row)];
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
            ImGui::EndTable();
        }

        ImGui::TextDisabled("Both sides count nested scopes at every level and sum across threads "
                            "and queues, so the percentages can exceed 100%%.");
    }

    void VkmProfileInspector::drawSubmitLane(const VkmGpuQueueTimeline& queue, const DisplayFrame& frame,
                                             const float laneY, const float chartLeft, const float chartWidth,
                                             const bool drawLatency)
    {
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        const float laneBottom = laneY + kSubmitLaneHeight;

        for (const VkmGpuSubmitMarker& marker : queue._submits)
        {
            const float beginMs = nsToMs(marker._beginNs - frame._beginNs);
            const float endMs = nsToMs(marker._endNs - frame._beginNs);
            const float x0 = chartLeft + (beginMs - _panMs) * _pixelsPerMs;
            const float x1 = std::max(chartLeft + (endMs - _panMs) * _pixelsPerMs, x0 + 2.0f);

            // The wait from the submit returning to the queue picking the work up. Suppressed
            // without calibration, where anchoring makes it zero by construction rather than by
            // measurement.
            float latencyX1 = x1;
            if (drawLatency && marker._hasGpuWork && marker._gpuBeginNs > marker._endNs)
            {
                const float gpuBeginMs = nsToMs(marker._gpuBeginNs - frame._beginNs);
                latencyX1 = chartLeft + (gpuBeginMs - _panMs) * _pixelsPerMs;

                const float midY = laneY + kSubmitLaneHeight * 0.5f;
                drawList->AddLine(ImVec2(x1, midY), ImVec2(latencyX1, midY), kQueueLatencyColor, 2.0f);
                drawList->AddTriangleFilled(ImVec2(latencyX1, midY), ImVec2(latencyX1 - 4.0f, midY - 3.0f),
                                            ImVec2(latencyX1 - 4.0f, midY + 3.0f), kQueueLatencyColor);
            }

            if (latencyX1 < chartLeft || x0 > chartLeft + chartWidth)
            {
                continue;
            }
            drawList->AddRectFilled(ImVec2(x0, laneY), ImVec2(x1, laneBottom), kSubmitMarkerColor);

            if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) &&
                ImGui::IsMouseHoveringRect(ImVec2(x0 - 3.0f, laneY), ImVec2(x1 + 3.0f, laneBottom)))
            {
                if (marker._hasGpuWork && drawLatency)
                {
                    ImGui::SetTooltip("submit to %s\nsubmitted at %.4f ms (took %.4f ms)\n"
                                      "GPU started %.4f ms later",
                                      queue._queueName.c_str(), beginMs, endMs - beginMs,
                                      nsToMs(marker._gpuBeginNs - marker._endNs));
                }
                else if (marker._hasGpuWork)
                {
                    ImGui::SetTooltip("submit to %s\nsubmitted at %.4f ms (took %.4f ms)\n"
                                      "queue latency not measured without clock calibration",
                                      queue._queueName.c_str(), beginMs, endMs - beginMs);
                }
                else
                {
                    ImGui::SetTooltip("submit to %s\nsubmitted at %.4f ms (took %.4f ms)\nno timed GPU work",
                                      queue._queueName.c_str(), beginMs, endMs - beginMs);
                }

                // Only the hovered marker gets a line up through the CPU rows to the scope that
                // made it; drawing every one turns the chart into a barcode.
                drawList->AddLine(ImVec2(x0, ImGui::GetWindowPos().y), ImVec2(x0, laneY),
                                  IM_COL32(236, 200, 96, 90));
            }
        }
    }

    void VkmProfileInspector::drawChart(const DisplayFrame& frame)
    {
        const float frameMs = std::max(nsToMs(frame.getDurationNs()), 0.001f);

        if (frame._hasGpu)
        {
            ImGui::Text("Frame #%u  CPU %.3f ms  GPU %.3f ms  [%s]", frame._cpu._frameNumber,
                        nsToMs(frame._cpu.getDurationNs()), nsToMs(frame._gpu.getDurationNs()),
                        correlationName(frame._gpu._correlation));
        }
        else
        {
            ImGui::Text("Frame #%u  CPU %.3f ms", frame._cpu._frameNumber,
                        nsToMs(frame._cpu.getDurationNs()));
            ImGui::SameLine();
            ImGui::TextDisabled("(GPU not resolved yet)");
        }

        drawSelectionSummary(frame);

        ImGui::BeginChild("##profileChart", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        const float chartWidth = std::max(ImGui::GetContentRegionAvail().x, 1.0f);
        // Stay pending until a frame with real content arrives: fitting to a zero-duration frame
        // would leave the chart zoomed in by six orders of magnitude.
        if (_fitRequested && frame.getDurationNs() > 0)
        {
            _pixelsPerMs = chartWidth / frameMs;
            _panMs = 0.0f;
            _fitRequested = false;
        }

        const float chartLeft = ImGui::GetCursorScreenPos().x;

        // Ruler geometry has to be captured before the strip is emitted: emitting it advances the
        // cursor, and chartBottom is measured from what is left below it.
        const ImVec2 rulerOrigin = ImGui::GetCursorScreenPos();
        const float rulerBottom = rulerOrigin.y + kRulerHeight;
        const float chartBottom = rulerOrigin.y + ImGui::GetContentRegionAvail().y;

        // The ruler is the one place a drag selects a range; everywhere else a drag pans.
        ImGui::SetCursorScreenPos(rulerOrigin);
        ImGui::InvisibleButton("##profileRuler", ImVec2(chartWidth, kRulerHeight));
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

        // Wheel zooms about the cursor (so the zone under the pointer stays put), left-drag pans.
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

        // One zone row loop for both halves; only the row grouping above it differs.
        const auto drawZoneRows = [&](const auto& zones, const ImVec2& rowsOrigin, const bool isGpu) {
            for (const auto& zone : zones)
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
                    ImGui::SetTooltip("%s\n%.4f ms\nstart %.4f ms  %s depth %u", zone._name, endMs - beginMs,
                                      beginMs, isGpu ? "GPU" : "CPU", static_cast<unsigned int>(zone._depth));
                }
            }
        };

        for (const VkmProfileThreadTimeline& timeline : frame._cpu._threads)
        {
            char header[160];
            std::snprintf(header, sizeof(header), "CPU: %s", timeline._threadName.c_str());
            ImGui::SeparatorText(header);
            if (timeline._overflowed)
            {
                ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.3f, 1.0f),
                                   "zone limit reached; this frame's timeline is incomplete");
            }

            const ImVec2 rowsOrigin = ImGui::GetCursorScreenPos();
            const float rowsHeight = static_cast<float>(maxDepth(timeline) + 1) * (kZoneRowHeight + kZoneRowGap);
            drawZoneRows(timeline._zones, rowsOrigin, /*isGpu=*/false);
            ImGui::Dummy(ImVec2(chartWidth, rowsHeight));
        }

        if (frame._hasGpu)
        {
            const bool drawLatency = frame._gpu._correlation == VkmGpuClockCorrelation::Calibrated;
            for (const VkmGpuQueueTimeline& timeline : frame._gpu._queues)
            {
                char header[192];
                std::snprintf(header, sizeof(header), "GPU: %s  (%s queue %u)", timeline._queueName.c_str(),
                              queueTypeName(timeline._queueType), timeline._queueIndex);
                ImGui::SeparatorText(header);
                if (timeline._overflowed)
                {
                    ImGui::TextColored(ImVec4(0.95f, 0.65f, 0.3f, 1.0f),
                                       "zone limit reached; this frame's timeline is incomplete");
                }

                // The submit lane sits above the zones it explains, so a marker and the work it
                // produced read top to bottom.
                const ImVec2 laneOrigin = ImGui::GetCursorScreenPos();
                drawSubmitLane(timeline, frame, laneOrigin.y, chartLeft, chartWidth, drawLatency);
                ImGui::Dummy(ImVec2(chartWidth, kSubmitLaneHeight));

                const ImVec2 rowsOrigin = ImGui::GetCursorScreenPos();
                const float rowsHeight = static_cast<float>(maxDepth(timeline) + 1) * (kZoneRowHeight + kZoneRowGap);
                drawZoneRows(timeline._zones, rowsOrigin, /*isGpu=*/true);
                ImGui::Dummy(ImVec2(chartWidth, rowsHeight));
            }
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
