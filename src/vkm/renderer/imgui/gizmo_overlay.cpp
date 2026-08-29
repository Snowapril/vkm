// Copyright (c) 2026 Snowapril

#include <vkm/renderer/imgui/gizmo_overlay.h>
#include <vkm/renderer/imgui/imgui_renderer.h>
#include <vkm/base/logger.h>

#include <imgui.h>
#include <ImGuizmo.h>

#if defined(VKM_USE_VULKAN_API)
#include <vkm/renderer/imgui/vulkan_imgui_renderer.h>
#elif defined(VKM_USE_METAL_API)
#include <vkm/renderer/imgui/metal_imgui_renderer.h>
#elif defined(VKM_USE_WEBGPU_API)
#include <vkm/renderer/imgui/webgpu_imgui_renderer.h>
#endif

namespace vkm
{
    VkmGizmoOverlay::VkmGizmoOverlay() = default;
    VkmGizmoOverlay::~VkmGizmoOverlay() = default;

    bool VkmGizmoOverlay::initialize(VkmDriverBase* driver, void* windowHandle, VkmFormat backBufferFormat)
    {
        VKM_ASSERT(driver != nullptr, "VkmGizmoOverlay::initialize requires a driver");

#if defined(VKM_USE_VULKAN_API)
        _renderer = std::make_unique<VkmImGuiRendererVulkan>(driver);
#elif defined(VKM_USE_METAL_API)
        _renderer = std::make_unique<VkmImGuiRendererMetal>(driver);
#elif defined(VKM_USE_WEBGPU_API)
        _renderer = std::make_unique<VkmImGuiRendererWebGPU>(driver);
#else
        (void)windowHandle;
        (void)backBufferFormat;
        return false;
#endif

        // No ini file and no platform input hooks: the panel context owns both, and a second
        // writer of either would take something from it. The mouse this context needs is polled
        // (Metal) or forwarded by the window's existing callbacks (GLFW).
        VkmImGuiRendererOptions options;
        options._installPlatformInput = false;
        options._enableIniFile = false;

        if (!_renderer->initialize(windowHandle, backBufferFormat, options))
        {
            VKM_DEBUG_INFO("Failed to create the scene-window gizmo overlay; the gizmo falls back "
                           "to the panel window");
            _renderer.reset();
            return false;
        }
        return true;
    }

    void VkmGizmoOverlay::shutdown()
    {
        if (_renderer)
        {
            _renderer->shutdown();
            _renderer.reset();
        }
    }

    void VkmGizmoOverlay::newFrame(bool sceneWindowFocused)
    {
        if (!_renderer)
        {
            return;
        }
        _begunThisFrame = false;
        _renderer->newFrame(sceneWindowFocused);
    }

    void VkmGizmoOverlay::discardFrame()
    {
        if (_renderer)
        {
            _renderer->discardFrame();
        }
    }

    bool VkmGizmoOverlay::begin()
    {
        if (!_renderer || _begunThisFrame)
        {
            return false;
        }
        _begunThisFrame = true;

        _previousContext = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(_renderer->getImGuiContext()));

        ImGuizmo::BeginFrame();
        ImGuizmo::SetOrthographic(false);
        // The context's own display size, not the swapchain extent: ImGuizmo lays its window out in
        // the same space io.DisplaySize is in, and on a HiDPI display the framebuffer extent is a
        // multiple of it.
        const ImGuiIO& io = ImGui::GetIO();
        ImGuizmo::SetRect(0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y);
        return true;
    }

    void VkmGizmoOverlay::end()
    {
        if (!_renderer)
        {
            return;
        }
        // Read before the context is put back: IsOver() hit-tests through the current ImGui context.
        _using = ImGuizmo::IsUsing() || ImGuizmo::IsOver();
        ImGui::SetCurrentContext(static_cast<ImGuiContext*>(_previousContext));
        _previousContext = nullptr;
    }
} // namespace vkm
