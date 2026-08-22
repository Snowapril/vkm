// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/imgui/imgui_canvas.h>

#include <cstddef>

namespace vkm
{
    class VkmDriverBase;

    /*
    * @brief Engine-owned ImGui debug window (F4) over every live acceleration structure in the
    * resource pool, drawn from VkmEngine::update() like the other inspectors. Three tabs:
    *
    *   Structures -- a table of every structure with per-selection detail: a top-level one lists
    *                 its instances (id, target, transform), a bottom-level one its triangle
    *                 geometries and object-space bounds.
    *   Graph      -- a node-link diagram, top-level structures on the left and bottom-level on
    *                 the right, one edge per instanced relationship.
    *   Spatial    -- a top-down world-XZ canvas of every instance's bounds transformed by its
    *                 instance matrix.
    *
    * The tabs share one selection, so a node picked in the Graph or Spatial tab is the structure
    * detailed in the Structures tab. Instance lists stay live across rebuilds because
    * VkmAccelerationStructure::updateInstances refreshes the info this window draws from.
    *
    * A checkbox above the tabs drives a fourth view that is not a tab: the in-scene wireframe
    * overlay VkmAccelerationStructureDebugRenderer draws, which shows the same instances the
    * Spatial tab flattens as oriented boxes in the 3D scene. This window only holds that toggle
    * and the shared selection; the engine reads both every frame, so the overlay keeps drawing
    * while this window is closed.
    */
    class VkmAccelerationStructureInspector
    {
    public:
        /*
        * @brief Draws the window.
        * @param driver Driver whose resource pool is enumerated. On a backend without ray
        * tracing the pool simply holds no structures and the window says so.
        */
        void draw(VkmDriverBase* driver);

        void toggleVisible() { _visible = !_visible; }
        bool isVisible() const { return _visible; }

        // Independent of window visibility: closing this window leaves the overlay drawing.
        bool isSceneOverlayEnabled() const { return _sceneOverlay; }
        // The structure the overlay highlights, shared with the tabs.
        VkmResourceHandle getSelected() const { return _selected; }

    private:
        void drawStructuresTab(VkmDriverBase* driver);
        void drawGraphTab(VkmDriverBase* driver);
        void drawSpatialTab(VkmDriverBase* driver);

        bool _visible = false;
        // Whether the engine draws the in-scene wireframe overlay this frame.
        bool _sceneOverlay = false;
        // Shared by all three tabs: the handle of the structure being detailed.
        VkmResourceHandle _selected = VKM_INVALID_RESOURCE_HANDLE;
        VkmImGuiCanvas _graphCanvas;
        VkmImGuiCanvas _spatialCanvas;
        // Structure count each canvas was last framed for, so a new scene is re-framed.
        size_t _graphFittedCount = 0;
        size_t _spatialFittedCount = 0;
    };
} // namespace vkm
