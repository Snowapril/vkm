// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>

namespace vkm
{
    class VkmDriverBase;
    class VkmCommandBufferBase;

    /*
    * @brief Base class for the engine's ImGui integration, one subclass per rendering backend.
    * @details Owns the ImGui context and drives NewFrame/Render once per engine frame. Both
    * engine-internal code (memory/perf stats) and external app/sample code just call plain
    * ImGui:: functions during update()/render() -- no separate per-caller API is needed.
    */
    class VkmImGuiRendererBase
    {
    public:
        VkmImGuiRendererBase(VkmDriverBase* driver);
        virtual ~VkmImGuiRendererBase();

        bool initialize(void* windowHandle, VkmFormat backBufferFormat);

        /*
        * @brief Begins an ImGui frame. `windowFocused` says whether the window this renderer is
        * bound to currently holds keyboard focus.
        * @details The engine is the only thing that knows which of its windows is focused, and a
        * backend that polls the mouse itself (Metal) must not poll while another window owns it
        * -- it would read that window's coordinates into this one's display space and raise
        * WantCaptureMouse over the other window's content. Backends whose ImGui platform layer
        * already tracks focus for them (GLFW, i.e. Vulkan/WebGPU) ignore it.
        */
        void newFrame(bool windowFocused);
        void renderDrawData(VkmCommandBufferBase* commandBuffer);

        /*
        * @brief Ends the current ImGui frame without drawing it.
        * @details newFrame() opens an ImGui frame unconditionally, but the ImGui window may end
        * up rendering nothing -- it is suspended during a live resize, minimized, or its image
        * acquire failed. ImGui::NewFrame() asserts if the previous frame was neither Render()ed
        * nor EndFrame()d, so those frames have to be closed explicitly. A no-op once
        * renderDrawData() has run, so the engine can call it unconditionally.
        */
        void discardFrame();

        void shutdown();

        /*
        * @brief Returns an ImTextureID-compatible value for a pooled engine texture, for use
        * with ImGui::Image(). Returns 0 when the backend cannot display engine textures in
        * ImGui (Vulkan/WebGPU keep this default for now -- only Metal overrides it).
        */
        virtual uint64_t getTextureID(VkmResourceHandle texture) { (void)texture; return 0; }

    protected:
        virtual bool initializeInner(void* windowHandle, VkmFormat backBufferFormat) = 0;
        virtual void newFrameInner() = 0;
        virtual void renderDrawDataInner(VkmCommandBufferBase* commandBuffer) = 0;
        virtual void shutdownInner() = 0;

        // Valid inside newFrameInner(); see newFrame().
        inline bool isWindowFocused() const { return _windowFocused; }

    protected:
        VkmDriverBase* _driver;

    private:
        bool _frameRendered = false; // Guards ImGui::Render() from running more than once per frame
        bool _windowFocused = false;
    };
} // namespace vkm
