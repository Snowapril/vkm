// Copyright (c) 2025 Snowapril

#pragma once

namespace vkm
{
    class VkmEngine;

    /*
    * @brief Installs the engine's GLFW key/char/mouse/cursor/scroll callbacks on the given
    * window and points its user pointer at the engine.
    * @details Takes the window as void* so platform layers can call this without dragging a
    * GLFW include into headers. Must be called AFTER VkmEngine::addSwapChain(), which is where
    * the ImGui GLFW backend installs its own callbacks: each glfwSetXxxCallback() returns the
    * previously installed handler (ImGui's), which we capture and forward to, instead of
    * silently unhooking it the way the old per-platform key callback did.
    * Only meaningful on GLFW-backed platforms; a no-op on the macOS Metal path, which uses
    * AppKit directly.
    */
    void installGlfwInputCallbacks(void* glfwWindow, VkmEngine* engine);

    /*
    * @brief Installs only the window-focus callback, so the engine learns which of its windows
    * holds keyboard focus (VkmEngine::getFocusedWindowIndex).
    * @details installGlfwInputCallbacks() already does this for the window it is given; this is
    * for the *other* windows, whose input callbacks belong to someone else (the ImGui window's
    * belong to the ImGui backend) and must not be replaced. Like the above, it forwards to the
    * handler it displaces and is a no-op on the macOS Metal path.
    */
    void installGlfwWindowFocusCallback(void* glfwWindow, VkmEngine* engine);
}
