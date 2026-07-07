// Copyright (c) 2025 Snowapril

#include <vkm/renderer/imgui/vulkan_imgui_renderer.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_command_queue.h>
#include <vkm/renderer/backend/vulkan/vulkan_command_buffer.h>
#include <vkm/renderer/backend/vulkan/vulkan_util.h>
#include <vkm/renderer/backend/common/renderer_common.h>

#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_vulkan.h>
#include <GLFW/glfw3.h>

namespace vkm
{
    namespace
    {
        constexpr const uint32_t IMGUI_VULKAN_DESCRIPTOR_POOL_SIZE = 64;

        void checkVkResult(VkResult result)
        {
            vkCheckResult(result, "ImGui Vulkan backend error");
        }
    } // namespace

    VkmImGuiRendererVulkan::VkmImGuiRendererVulkan(VkmDriverBase* driver)
        : VkmImGuiRendererBase(driver)
    {
    }

    VkmImGuiRendererVulkan::~VkmImGuiRendererVulkan()
    {
    }

    bool VkmImGuiRendererVulkan::initializeInner(void* windowHandle, VkmFormat backBufferFormat)
    {
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        GLFWwindow* glfwWindow = reinterpret_cast<GLFWwindow*>(windowHandle);

        ImGui_ImplGlfw_InitForVulkan(glfwWindow, true);

        VkmCommandQueueVulkan* graphicsQueue = static_cast<VkmCommandQueueVulkan*>(_driver->getCommandQueue(VkmCommandQueueType::Graphics, 0));
        const VkFormat colorFormat = toVkFormat(backBufferFormat);

        ImGui_ImplVulkan_InitInfo initInfo{};
        initInfo.ApiVersion = VK_API_VERSION_1_3;
        initInfo.Instance = driverVulkan->getInstance();
        initInfo.PhysicalDevice = driverVulkan->getPhysicalDevice();
        initInfo.Device = driverVulkan->getDevice();
        initInfo.QueueFamily = driverVulkan->getQueueFamilyIndex(VkmCommandQueueType::Graphics);
        initInfo.Queue = graphicsQueue->getVkQueue();
        initInfo.DescriptorPoolSize = IMGUI_VULKAN_DESCRIPTOR_POOL_SIZE;
        initInfo.MinImageCount = FRAME_BUFFER_COUNT;
        initInfo.ImageCount = FRAME_BUFFER_COUNT;
        initInfo.UseDynamicRendering = true;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
        initInfo.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &colorFormat;
        initInfo.CheckVkResultFn = checkVkResult;

        return ImGui_ImplVulkan_Init(&initInfo);
    }

    void VkmImGuiRendererVulkan::newFrameInner()
    {
        ImGui_ImplVulkan_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
    }

    void VkmImGuiRendererVulkan::renderDrawDataInner(VkmCommandBufferBase* commandBuffer)
    {
        VkmCommandBufferVulkan* commandBufferVulkan = static_cast<VkmCommandBufferVulkan*>(commandBuffer);
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBufferVulkan->getVkCommandBuffer());
    }

    void VkmImGuiRendererVulkan::shutdownInner()
    {
        ImGui_ImplVulkan_Shutdown();
        ImGui_ImplGlfw_Shutdown();
    }
} // namespace vkm
