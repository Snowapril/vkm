// Copyright (c) 2026 Snowapril

#pragma once

// Drawing helpers for the profile inspector's chart. Internal to src/vkm/renderer/imgui --
// there is nothing here an engine user would call. Kept apart from the window itself so the
// geometry and the zone palette are stated once, for CPU and GPU rows alike.

#include <imgui.h>

#include <algorithm>
#include <cstdint>

namespace vkm
{
    namespace profilerChart
    {
        // One 60 Hz frame. Only used to scale/annotate a history strip.
        constexpr float kFrameBudgetMs = 1000.0f / 60.0f;
        constexpr float kHistoryHeight = 64.0f;
        constexpr float kRulerHeight = 18.0f;
        constexpr float kZoneRowHeight = 18.0f;
        constexpr float kZoneRowGap = 1.0f;
        // The lane above a queue's zone rows holding its submit markers and the latency bars
        // that run from each marker to the work it produced.
        constexpr float kSubmitLaneHeight = 10.0f;
        // A submit marker, and the bar from it to the moment the queue picked the work up.
        constexpr ImU32 kSubmitMarkerColor = IM_COL32(236, 200, 96, 255);
        constexpr ImU32 kQueueLatencyColor = IM_COL32(150, 128, 72, 190);
        // A zone narrower than this gets no label; narrower than one pixel is still drawn one
        // pixel wide so that a burst of tiny scopes reads as a solid band rather than vanishing.
        constexpr float kMinLabelWidth = 24.0f;
        // Below this a drag reads as a click, which clears the selection instead of leaving a
        // sliver behind.
        constexpr float kMinSelectionWidthPx = 3.0f;
        // Enough to show where the time went without turning into a second flame chart.
        constexpr int kMaxSelectionRows = 12;

        inline float nsToMs(const uint64_t ns)
        {
            return static_cast<float>(static_cast<double>(ns) * 1e-6);
        }

        /*
        * @brief Deterministic fill color for a zone, hashed from its name so the same scope
        * keeps the same color across frames, across threads, and -- because both inspectors call
        * this -- between the CPU and GPU charts, where a subgraph appears in both.
        */
        inline ImU32 zoneColor(const char* name)
        {
            uint32_t hash = 2166136261u; // FNV-1a
            for (const char* c = name; c != nullptr && *c != '\0'; ++c)
            {
                hash = (hash ^ static_cast<uint32_t>(static_cast<unsigned char>(*c))) * 16777619u;
            }

            float r = 0.0f, g = 0.0f, b = 0.0f;
            ImGui::ColorConvertHSVtoRGB(static_cast<float>(hash & 0xFFFFu) / 65535.0f, 0.55f, 0.80f, r, g, b);
            return ImGui::GetColorU32(ImVec4(r, g, b, 1.0f));
        }

        // Grid step in milliseconds that keeps gridlines at least ~80 px apart, snapped to a
        // 1/2/5 sequence so the labels stay readable at any zoom.
        inline float chooseGridStepMs(const float pixelsPerMs)
        {
            const float minStepMs = 80.0f / std::max(pixelsPerMs, 0.001f);
            float step = 0.001f;
            while (step < minStepMs)
            {
                if (step * 2.0f >= minStepMs) { step *= 2.0f; break; }
                if (step * 5.0f >= minStepMs) { step *= 5.0f; break; }
                step *= 10.0f;
            }
            return step;
        }
    } // namespace profilerChart
} // namespace vkm
