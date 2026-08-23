// Copyright (c) 2026 Snowapril

#include <vkm/renderer/imgui/imgui_settings.h>

#include <vkm/base/common.h>

#include <imgui.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace vkm
{
    namespace
    {
        constexpr float kMinFontScale = 0.5f;
        constexpr float kMaxFontScale = 3.0f;

        constexpr const char* kFontScaleKey = "fontScale";

        // Empty when the settings cannot be cached, which makes every caller a no-op.
        std::filesystem::path settingsFilePath()
        {
#if defined(__EMSCRIPTEN__)
            // MEMFS is recreated on every run, so nothing written here would survive.
            return {};
#else
#if defined(_WIN32)
            const char* configRoot = std::getenv("APPDATA");
            const char* configDirName = "vkm";
#else
            const char* configRoot = std::getenv("HOME");
            const char* configDirName = ".vkm";
#endif
            if (configRoot == nullptr)
            {
                return {};
            }
            return std::filesystem::path(configRoot) / configDirName / "ui_settings.json";
#endif // __EMSCRIPTEN__
        }

        void saveFontScale(float fontScale)
        {
            const std::filesystem::path path = settingsFilePath();
            if (path.empty())
            {
                return;
            }

            std::error_code createError;
            std::filesystem::create_directories(path.parent_path(), createError);
            if (createError)
            {
                VKM_DEBUG_WARN(fmt::format("Failed to create the ImGui settings directory {}: {}",
                                           path.parent_path().string(), createError.message()).c_str());
                return;
            }

            std::ofstream out(path, std::ios::trunc);
            if (!out)
            {
                VKM_DEBUG_WARN(fmt::format("Failed to open the ImGui settings file {} for writing",
                                           path.string()).c_str());
                return;
            }
            // Two decimals, matching what the slider displays: a float widened to double
            // otherwise serializes as 1.2400000095367432, which is noise in a file a user may
            // want to read or edit by hand.
            out << nlohmann::json{{kFontScaleKey, std::round(fontScale * 100.0f) / 100.0}}.dump(4);
        }
    } // namespace

    void vkmLoadImGuiSettings()
    {
        const std::filesystem::path path = settingsFilePath();
        if (path.empty())
        {
            return;
        }

        std::ifstream in(path);
        if (!in)
        {
            // No cache yet: the ImGui defaults are the intended starting point.
            return;
        }

        const nlohmann::json root = nlohmann::json::parse(in, nullptr, /*allow_exceptions=*/false);
        if (root.is_discarded() || !root.is_object())
        {
            VKM_DEBUG_WARN(
                fmt::format("Ignoring the malformed ImGui settings file {}", path.string()).c_str());
            return;
        }

        const auto fontScale = root.find(kFontScaleKey);
        if (fontScale == root.end() || !fontScale->is_number())
        {
            return;
        }
        ImGui::GetStyle().FontScaleMain =
            std::clamp(fontScale->get<float>(), kMinFontScale, kMaxFontScale);
    }

    void vkmDrawImGuiFontScaleSlider()
    {
        ImGuiStyle& style = ImGui::GetStyle();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::SliderFloat("Font scale", &style.FontScaleMain, kMinFontScale, kMaxFontScale, "%.2fx");
        // Only once the drag ends: SliderFloat reports a change on every frame it is held.
        if (ImGui::IsItemDeactivatedAfterEdit())
        {
            saveFontScale(style.FontScaleMain);
        }
    }
} // namespace vkm
