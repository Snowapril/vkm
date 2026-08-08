// Copyright (c) 2025 Snowapril

#pragma once

#include <imgui.h>

namespace vkm
{
    /*
    * @brief Pan-and-zoom viewport for ImGui content drawn in its own coordinate space.
    * @details Content is laid out in unscaled canvas units and mapped to the screen through
    * toScreen(); lengths and font sizes are multiplied by getScale(). The wheel zooms about the
    * cursor and a drag that starts on empty space pans, so the region carries no scrollbars. Zoom
    * is proportional to the wheel delta, so a trackpad's two-finger scroll and its pinch gesture
    * (which the platform layer forwards as a wheel delta) both work as well as a mouse notch.
    * Items submitted between begin() and end() are hit-tested as usual: the canvas submits its own
    * background button first, so anything drawn afterwards takes hover and click priority over it
    * and only a drag that misses every item pans the view.
    * One instance holds one view, so it must live as long as the window drawing into it.
    */
    class VkmImGuiCanvas
    {
    public:
        /*
        * @brief Opens the canvas region and consumes this frame's pan and zoom input.
        * @details end() must be called whatever this returns.
        * @param id Child window id, unique within the enclosing window.
        * @param size Region size; a zero component fills the remaining space on that axis.
        * @return False when the region is fully clipped and nothing needs to be drawn.
        */
        bool begin(const char* id, const ImVec2& size = ImVec2(0.0f, 0.0f));
        void end();

        ImVec2 toScreen(const ImVec2& canvasPos) const;
        ImVec2 toCanvas(const ImVec2& screenPos) const;
        inline float getScale() const { return _scale; }

        // Draw list of the canvas region. Valid only between begin() and end().
        ImDrawList* getDrawList() const;

        /*
        * @brief Frames `contentSize` canvas units in the viewport on the next begin().
        * @details Deferred to begin() because that is where the viewport size is known.
        * @param contentSize Extent of the content in canvas units, measured from the origin.
        */
        void requestFit(const ImVec2& contentSize);

        // Returns to 1:1 scale with the canvas origin at the region's top left.
        void resetView();

        static constexpr float kMinScale = 0.2f;
        static constexpr float kMaxScale = 4.0f;

    private:
        // Canvas-space point drawn at the region's top-left corner.
        ImVec2 _origin{0.0f, 0.0f};
        float _scale = 1.0f;
        // Screen position of the region's top-left corner, refreshed by begin().
        ImVec2 _screenMin{0.0f, 0.0f};
        ImVec2 _contentToFit{0.0f, 0.0f};
        bool _fitRequested = false;
        // Whether begin() opened a region whose clip rect end() still has to pop.
        bool _clipPushed = false;
    };
} // namespace vkm
