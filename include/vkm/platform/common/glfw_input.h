// Copyright (c) 2025 Snowapril

#pragma once

namespace vkm
{
    class VkmEngine;

    /*
    * @brief Installs the engine's GLFW key/char/mouse/cursor/scroll callbacks on the given
    * window and points its user pointer at the engine.
    * @details Takes the window as void* so platform layers can call this without dragging a GLFW
    * include into headers. Must be called AFTER VkmEngine::addSwapChain(), where the ImGui GLFW
    * backend installs its own callbacks: each glfwSetXxxCallback() returns the handler it
    * displaces, which this captures and forwards to rather than unhooking.
    * Only meaningful on GLFW-backed platforms; a no-op on the macOS Metal path, which uses AppKit
    * directly.
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

    /*
    * @brief Installs the framebuffer-size callback, so the engine rebuilds the window's swapchain
    * when it is resized (VkmEngine::onWindowResized). Also publishes the current size once, since
    * the size the swapchain was created with is the requested one, not necessarily the real one.
    * @details Install on every window, like installGlfwWindowFocusCallback and unlike the input
    * callbacks. Forwards to the handler it displaces.
    * GLFW has no resize-started/ended notification. What stands in for one is that this callback
    * only fires from glfwPollEvents(), which runs once per frame right before the engine loop, so
    * a frame's worth of size changes coalesces into at most one rebuild at the newest size. How
    * much of a drag that covers is platform-dependent: Win32 and Cocoa run a modal resize loop, so
    * glfwPollEvents() does not return for its duration and no frames are drawn, while X11 has no
    * such loop and a drag rebuilds once per frame.
    * Like the above, a no-op on the macOS Metal path, which uses AppKit's live-resize brackets.
    */
    void installGlfwWindowResizeCallback(void* glfwWindow, VkmEngine* engine);
}
