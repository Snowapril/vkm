// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>

#include <memory>

namespace vkm
{
    class VkmDriverBase;
    class VkmImGuiRendererBase;

    /*
    * @brief A second ImGui context, bound to the scene window, that carries nothing but the
    * transform gizmo.
    * @details The debug panels live in their own OS window on desktop, and a gizmo drawn there is
    * a manipulator floating over a grey background while the thing it moves is in another window.
    * This owns a context bound to the scene window instead, so the gizmo is drawn and dragged over
    * the scene while every panel stays where it was.
    *
    * ImGuizmo keeps its state in one file-static context, not one per ImGui context, so exactly one
    * gizmo can be live per frame -- begin() refuses a second within the same frame, and callers
    * that own several gizmos must still arbitrate between them. Every ImGuizmo call must be made
    * between begin() and end(); IsOver() in particular reads the current ImGui context.
    */
    class VkmGizmoOverlay
    {
    public:
        VkmGizmoOverlay();
        ~VkmGizmoOverlay();

        /*
        * @brief Creates the overlay's own ImGui context and backend against the scene window.
        * @details Logs and returns false rather than asserting: a missing overlay is a supported
        * state, and the caller falls back to drawing the gizmo in whatever context it already has.
        * @param driver Driver the backend renders through.
        * @param windowHandle The scene window, whose drawable the gizmo is sized and drawn to.
        * @param backBufferFormat Format of that window's back buffer.
        * @return False when the context or its backend could not be created.
        */
        bool initialize(VkmDriverBase* driver, void* windowHandle, VkmFormat backBufferFormat);
        void shutdown();

        // Opens the overlay's ImGui frame. `sceneWindowFocused` says whether the scene window holds
        // focus, which is what decides whether its cursor may be polled -- see
        // VkmImGuiRendererBase::newFrame.
        void newFrame(bool sceneWindowFocused);
        void discardFrame();

        /*
        * @brief Makes the overlay current and opens ImGuizmo against the scene window.
        * @details Sets the manipulator's rect from this context's own display size, which is the
        * space ImGuizmo's window is laid out in -- the swapchain extent is in framebuffer pixels
        * and differs from it by the backing scale.
        * @return False when the overlay is unavailable, or when a gizmo has already been begun
        * this frame. A caller that gets false must not call ImGuizmo at all.
        */
        bool begin();
        // Caches whether the gizmo is hovered or held, then restores the previous context.
        void end();

        VkmImGuiRendererBase* getRenderer() const { return _renderer.get(); }
        bool isAvailable() const { return _renderer != nullptr; }
        // Whether the gizmo took the mouse on the frame just drawn, so a drag on a handle does not
        // also reach the camera controller.
        bool isUsing() const { return _using; }

    private:
        std::unique_ptr<VkmImGuiRendererBase> _renderer;
        void* _previousContext = nullptr;
        bool _begunThisFrame = false;
        bool _using = false;
    };
} // namespace vkm
