// Copyright (c) 2026 Snowapril

#include <vkm/renderer/imgui/imgui_renderer.h>
#include <imgui.h>

namespace vkm
{
    namespace
    {
        /*
        * Selects one renderer's context for the duration of a call and puts back whatever was
        * current before. Every entry point below is bracketed by one: with more than one context
        * alive, acting on "the current context" is acting on whichever call ran last.
        */
        class ScopedImGuiContext
        {
        public:
            explicit ScopedImGuiContext(void* context)
                : _previous(ImGui::GetCurrentContext())
            {
                if (context != nullptr)
                {
                    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(context));
                }
            }

            ~ScopedImGuiContext() { ImGui::SetCurrentContext(_previous); }

            ScopedImGuiContext(const ScopedImGuiContext&) = delete;
            ScopedImGuiContext& operator=(const ScopedImGuiContext&) = delete;

        private:
            ImGuiContext* _previous;
        };
    } // namespace

    VkmImGuiRendererBase::VkmImGuiRendererBase(VkmDriverBase* driver)
        : _driver(driver)
    {
    }

    VkmImGuiRendererBase::~VkmImGuiRendererBase()
    {
    }

    bool VkmImGuiRendererBase::initialize(void* windowHandle, VkmFormat backBufferFormat,
                                          const VkmImGuiRendererOptions& options)
    {
        IMGUI_CHECKVERSION();
        _options = options;
        // Created and left current for initializeInner: the backends' Init functions store their
        // state on the context that is current when they run.
        ImGuiContext* const previous = ImGui::GetCurrentContext();
        ImGuiContext* const context = ImGui::CreateContext();
        _context = context;
        if (!_options._enableIniFile)
        {
            // Two contexts sharing the default filename would each overwrite the other's layout.
            ImGui::GetIO().IniFilename = nullptr;
        }

        const bool initialized = initializeInner(windowHandle, backBufferFormat);
        ImGui::SetCurrentContext(previous);
        return initialized;
    }

    void VkmImGuiRendererBase::newFrame(bool windowFocused)
    {
        // Deliberately not restored: this context stays current for the rest of the frame's UI, so
        // that every plain ImGui:: call in engine and app code lands on it without knowing there is
        // more than one. The engine opens the frames in the order it wants that to settle in.
        if (_context != nullptr)
        {
            ImGui::SetCurrentContext(static_cast<ImGuiContext*>(_context));
        }
        _windowFocused = windowFocused;
        newFrameInner();
        _frameRendered = false;
    }

    void VkmImGuiRendererBase::renderDrawData(VkmCommandBufferBase* commandBuffer)
    {
        ScopedImGuiContext scoped(_context);
        if (_frameRendered)
        {
            return;
        }
        _frameRendered = true;

        ImGui::Render();
        renderDrawDataInner(commandBuffer);
    }

    void VkmImGuiRendererBase::discardFrame()
    {
        ScopedImGuiContext scoped(_context);
        if (_frameRendered)
        {
            return;
        }
        _frameRendered = true;

        ImGui::EndFrame();
    }

    bool VkmImGuiRendererBase::wantsCaptureMouse() const
    {
        if (_context == nullptr)
        {
            return false;
        }
        ScopedImGuiContext scoped(_context);
        return ImGui::GetIO().WantCaptureMouse;
    }

    bool VkmImGuiRendererBase::wantsCaptureKeyboard() const
    {
        if (_context == nullptr)
        {
            return false;
        }
        ScopedImGuiContext scoped(_context);
        return ImGui::GetIO().WantCaptureKeyboard;
    }

    void VkmImGuiRendererBase::shutdown()
    {
        if (_context == nullptr)
        {
            return;
        }
        ImGuiContext* const context = static_cast<ImGuiContext*>(_context);
        ImGuiContext* const previous = ImGui::GetCurrentContext();
        ImGui::SetCurrentContext(context);
        shutdownInner();
        ImGui::DestroyContext(context);
        _context = nullptr;
        // Not restored when it was this one: that pointer has just been freed.
        ImGui::SetCurrentContext(previous == context ? nullptr : previous);
    }
} // namespace vkm
