// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/imgui/imgui_renderer.h>

namespace vkm
{
    /*
    * @brief Custom Metal4 ImGui renderer.
    * @details Dear ImGui's upstream imgui_impl_metal.mm targets classic MTLCommandBuffer /
    * MTLRenderCommandEncoder / MTLRenderPipelineState, which is incompatible with this engine's
    * Metal4 pipeline (MTL4CommandBuffer / MTL4RenderCommandEncoder / MTL4Compiler-built pipeline
    * states). This class re-implements the same responsibilities (font atlas + user texture
    * upload, dynamic vertex/index buffers, draw command translation) directly against Metal4.
    */
    class VkmImGuiRendererMetal final : public VkmImGuiRendererBase
    {
    public:
        VkmImGuiRendererMetal(VkmDriverBase* driver);
        ~VkmImGuiRendererMetal();

        virtual uint64_t getTextureID(VkmResourceHandle texture) override final;

    protected:
        virtual bool initializeInner(void* windowHandle, VkmFormat backBufferFormat) override final;
        virtual void newFrameInner() override final;
        virtual void renderDrawDataInner(VkmCommandBufferBase* commandBuffer) override final;
        virtual void shutdownInner() override final;

    private:
        class Impl;
        Impl* _impl;
    };
} // namespace vkm
