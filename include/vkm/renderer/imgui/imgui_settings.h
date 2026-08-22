// Copyright (c) 2026 Snowapril

#pragma once

namespace vkm
{
    /*
    * @brief Loads the cached ImGui presentation settings and applies them to the current context.
    * @details Must run after ImGui::CreateContext() and before the first ImGui::NewFrame(): the
    * atlas is seeded with a scalable default font here, and the font scale has to be in ImGuiStyle
    * before ImGui resolves any text size. The cache lives in the user's config directory
    * (%APPDATA%/vkm/ui_settings.json on Windows, $HOME/.vkm/ui_settings.json elsewhere); the ImGui
    * defaults stay in place when it is absent or unparseable, and no cache exists under Emscripten.
    */
    void vkmLoadImGuiSettings();

    /*
    * @brief Submits the font-scale slider into the enclosing ImGui window.
    * @details Writes the new value to the cache file when the drag ends rather than every frame.
    * A change takes effect immediately for windows opened after this one, and for the enclosing
    * window from the next frame.
    */
    void vkmDrawImGuiFontScaleSlider();
} // namespace vkm
