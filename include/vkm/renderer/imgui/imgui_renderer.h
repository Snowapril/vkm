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
