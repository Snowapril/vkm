// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/renderer/memory_report.h>

namespace vkm
{
    class VkmDriverBase;

    /*
    * @brief Engine-owned ImGui window showing what the engine's allocators tracked next to
    * what the OS and the graphics driver actually report. Toggled with F8; drawn from
    * VkmEngine::update(), before the frame's first ImGui::Render() call.
    *
    * Sampling is throttled (default twice a second), and the tag table -- the half that takes
    * MemoryTracker's global mutex, which every allocation in the process contends on -- is
    * sampled only while this window is open. The debug overlay reads the same cached snapshot
    * through getSnapshot() so one sample serves both.
    */
    class VkmMemoryInspector
    {
    public:
        // Re-captures when the refresh interval has elapsed (or when a refresh was requested).
        void update(VkmDriverBase* driver, double deltaTime);

        // No-op while the window is hidden.
        void draw();

        inline const VkmMemorySnapshot& getSnapshot() const { return _snapshot; }
        inline bool isVisible() const { return _visible; }
        // Requests a refresh as well: the tag table is only sampled while the window is open, so
        // opening it would otherwise show an empty table until the next interval elapses.
        inline void toggleVisible()
        {
            _visible = !_visible;
            _refreshRequested = true;
        }

    private:
        VkmMemorySnapshot _snapshot;
        double _secondsSinceCapture = 0.0;
        float _refreshIntervalSeconds = 0.5f;
        bool _refreshRequested = true; // capture on the very first update()
        bool _visible = false;
    };
} // namespace vkm
