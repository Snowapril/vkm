// Copyright (c) 2025 Snowapril

#include <vkm/renderer/imgui/webgpu_imgui_renderer.h>
#include <vkm/renderer/backend/webgpu/webgpu_driver.h>
#include <vkm/renderer/backend/webgpu/webgpu_command_buffer.h>
#include <vkm/renderer/backend/webgpu/webgpu_util.h>
#include <vkm/renderer/backend/common/renderer_common.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_wgpu.h>
#include <GLFW/glfw3.h>

namespace vkm
{
    VkmImGuiRendererWebGPU::VkmImGuiRendererWebGPU(VkmDriverBase* driver)
        : VkmImGuiRendererBase(driver)
    {
    }

    VkmImGuiRendererWebGPU::~VkmImGuiRendererWebGPU()
    {
    }

    bool VkmImGuiRendererWebGPU::initializeInner(void* windowHandle, VkmFormat backBufferFormat)
    {
        VkmDriverWebGPU* driverWebGPU = static_cast<VkmDriverWebGPU*>(_driver);
        GLFWwindow* glfwWindow = reinterpret_cast<GLFWwindow*>(windowHandle);

        // WebGPU has no dedicated ImGui_ImplGlfw_InitForXXX entry point; InitForOther is
        // upstream's documented choice for renderer backends other than OpenGL/Vulkan.
        ImGui_ImplGlfw_InitForOther(glfwWindow, true);

        ImGui_ImplWGPU_InitInfo initInfo;
        initInfo.Device = driverWebGPU->getDevice();
        initInfo.NumFramesInFlight = FRAME_BUFFER_COUNT;
        initInfo.RenderTargetFormat = toWGPUTextureFormat(backBufferFormat);

        return ImGui_ImplWGPU_Init(&initInfo);
    }

    void VkmImGuiRendererWebGPU::newFrameInner()
    {
        ImGui_ImplWGPU_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }

    void VkmImGuiRendererWebGPU::renderDrawDataInner(VkmCommandBufferBase* commandBuffer)
    {
        VkmCommandBufferWebGPU* commandBufferWebGPU = static_cast<VkmCommandBufferWebGPU*>(commandBuffer);
        ImGui_ImplWGPU_RenderDrawData(ImGui::GetDrawData(), commandBufferWebGPU->getActiveRenderPassEncoder());
    }

    void VkmImGuiRendererWebGPU::shutdownInner()
    {
        ImGui_ImplWGPU_Shutdown();
        ImGui_ImplGlfw_Shutdown();
    }
} // namespace vkm
