// Copyright (c) 2025 Snowapril

#include <vkm/renderer/imgui/imgui_canvas.h>

#include <algorithm>
#include <cmath>

namespace vkm
{
    namespace
    {
        // Zoom per unit of wheel delta: one notch of a line-based wheel is 1.0 and scales by this.
        // Proportional rather than a fixed step per event, because a trackpad delivers a stream of
        // small deltas per gesture and a fixed step would run the scale into its limit at once.
        // Kept mild so that one two-finger swipe, worth a couple of dozen units, crosses a useful
        // part of the range rather than all of it.
        constexpr float kWheelZoomStep = 1.10f;
    }

    bool VkmImGuiCanvas::begin(const char* id, const ImVec2& size)
    {
        const bool visible = ImGui::BeginChild(id, size, ImGuiChildFlags_Borders,
                                               ImGuiWindowFlags_NoScrollbar |
                                               ImGuiWindowFlags_NoScrollWithMouse |
                                               ImGuiWindowFlags_NoMove);
        if (!visible)
        {
            return false;
        }

        _screenMin = ImGui::GetCursorScreenPos();
        const ImVec2 region = ImVec2(std::max(ImGui::GetContentRegionAvail().x, 1.0f),
                                     std::max(ImGui::GetContentRegionAvail().y, 1.0f));

        if (_fitRequested)
        {
            const float scaleX = (_contentToFit.x > 0.0f) ? region.x / _contentToFit.x : 1.0f;
            const float scaleY = (_contentToFit.y > 0.0f) ? region.y / _contentToFit.y : 1.0f;
            _scale = std::clamp(std::min(scaleX, scaleY), kMinScale, kMaxScale);
            // Centre whichever axis the fit left slack on.
            _origin = ImVec2(-(region.x / _scale - _contentToFit.x) * 0.5f,
                             -(region.y / _scale - _contentToFit.y) * 0.5f);
            _fitRequested = false;
        }

        // Submitted before the caller's items so those take hover and click priority, which is
        // what makes a drag pan only when it misses everything drawn on top. Without the overlap
        // opt-in this button would claim the hovered id for the whole region and nothing drawn
        // afterwards could ever be hovered.
        ImGui::SetCursorScreenPos(_screenMin);
        ImGui::SetNextItemAllowOverlap();
        ImGui::InvisibleButton("##canvasBackground", region,
                               ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
        const bool draggingBackground = ImGui::IsItemActive();

        const ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsWindowHovered() && io.MouseWheel != 0.0f)
        {
            const ImVec2 anchor = toCanvas(io.MousePos);
            _scale = std::clamp(_scale * std::pow(kWheelZoomStep, io.MouseWheel), kMinScale, kMaxScale);
            // Re-anchor so the canvas point under the cursor stays under the cursor.
            _origin = ImVec2(anchor.x - (io.MousePos.x - _screenMin.x) / _scale,
                             anchor.y - (io.MousePos.y - _screenMin.y) / _scale);
        }

        if (draggingBackground)
        {
            _origin.x -= io.MouseDelta.x / _scale;
            _origin.y -= io.MouseDelta.y / _scale;
        }

        ImGui::GetWindowDrawList()->PushClipRect(_screenMin,
                                                 ImVec2(_screenMin.x + region.x, _screenMin.y + region.y), true);
        _clipPushed = true;
        return true;
    }

    void VkmImGuiCanvas::end()
    {
        if (_clipPushed)
        {
            ImGui::GetWindowDrawList()->PopClipRect();
            _clipPushed = false;
        }
        ImGui::EndChild();
    }

    ImVec2 VkmImGuiCanvas::toScreen(const ImVec2& canvasPos) const
    {
        return ImVec2(_screenMin.x + (canvasPos.x - _origin.x) * _scale,
                      _screenMin.y + (canvasPos.y - _origin.y) * _scale);
    }

    ImVec2 VkmImGuiCanvas::toCanvas(const ImVec2& screenPos) const
    {
        return ImVec2(_origin.x + (screenPos.x - _screenMin.x) / _scale,
                      _origin.y + (screenPos.y - _screenMin.y) / _scale);
    }

    ImDrawList* VkmImGuiCanvas::getDrawList() const
    {
        return ImGui::GetWindowDrawList();
    }

    void VkmImGuiCanvas::requestFit(const ImVec2& contentSize)
    {
        _contentToFit = contentSize;
        _fitRequested = true;
    }

    void VkmImGuiCanvas::resetView()
    {
        _origin = ImVec2(0.0f, 0.0f);
        _scale = 1.0f;
        _fitRequested = false;
    }
} // namespace vkm
