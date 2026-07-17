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
        void newFrame();
        void renderDrawData(VkmCommandBufferBase* commandBuffer);
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

    protected:
        VkmDriverBase* _driver;

    private:
        bool _frameRendered = false; // Guards ImGui::Render() from running more than once per frame
    };
} // namespace vkm
