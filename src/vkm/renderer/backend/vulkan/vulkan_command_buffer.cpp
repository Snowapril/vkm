// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_command_buffer.h>
#include <vkm/renderer/backend/vulkan/vulkan_texture.h>
#include <vkm/renderer/backend/vulkan/vulkan_pipeline_state.h>
#include <vkm/renderer/backend/common/render_pass.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/common/driver.h>

namespace vkm
{
    static VkAttachmentLoadOp toVkAttachmentLoadOp(VkmLoadAction loadAction)
    {
        switch (loadAction)
        {
            case VkmLoadAction::Load:     return VK_ATTACHMENT_LOAD_OP_LOAD;
            case VkmLoadAction::Clear:    return VK_ATTACHMENT_LOAD_OP_CLEAR;
            case VkmLoadAction::DontCare: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        }
        return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }

    static VkAttachmentStoreOp toVkAttachmentStoreOp(VkmStoreAction storeAction)
    {
        switch (storeAction)
        {
            case VkmStoreAction::Store:   return VK_ATTACHMENT_STORE_OP_STORE;
            case VkmStoreAction::DontCare: return VK_ATTACHMENT_STORE_OP_DONT_CARE;
        }
        return VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }

    // Dynamic rendering (vkCmdBeginRendering/vkCmdEndRendering) has no subpass dependencies to
    // transition image layouts implicitly, unlike a legacy VkRenderPass -- so every attachment
    // needs an explicit barrier between whatever it was doing before and how this render pass
    // (or present) is about to use it.
    static void transitionImageLayout(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout)
    {
        const VkImageMemoryBarrier2 barrier{
            .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
            .srcStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .srcAccessMask       = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
            .dstStageMask        = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
            .dstAccessMask       = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
            .oldLayout           = oldLayout,
            .newLayout           = newLayout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image               = image,
            .subresourceRange    = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        const VkDependencyInfo dependencyInfo{
            .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers    = &barrier,
        };
        vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
    }

    VkmCommandBufferVulkan::VkmCommandBufferVulkan(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue, VkmCommandBufferPoolBase* commandBufferPool)
        : VkmCommandBufferBase(driver, commandQueue, commandBufferPool)
    {
    }

    VkmCommandBufferVulkan::~VkmCommandBufferVulkan()
    {
    }

    void VkmCommandBufferVulkan::setRHICommandBuffer(VKM_COMMAND_BUFFER_HANDLE handle)
    {
        _vkCommandBuffer = static_cast<VkCommandBuffer>(handle);
    }

    void VkmCommandBufferVulkan::onBeginRenderPass(const VkmFrameBufferDescriptor& frameBufferDesc)
    {
        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();

        std::vector<VkRenderingAttachmentInfo> colorAttachments;
        colorAttachments.reserve(frameBufferDesc._renderPass._colorAttachmentCount);
        for (uint32_t i = 0; i < frameBufferDesc._renderPass._colorAttachmentCount; ++i)
        {
            VkmTextureVulkan* colorTextureVulkan = static_cast<VkmTextureVulkan*>(renderResourcePool->getResource<VkmTexture>(frameBufferDesc._colorAttachments[i]));
            const VkmColorAttachmentDescriptor& colorAttachmentDesc = frameBufferDesc._renderPass._colorAttachments[i];

            const VkImageLayout previousLayout = colorTextureVulkan->getCurrentLayout();
            if (previousLayout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL)
            {
                transitionImageLayout(_vkCommandBuffer, colorTextureVulkan->getImage(), previousLayout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
                colorTextureVulkan->setCurrentLayout(VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            }

            colorAttachments.push_back(VkRenderingAttachmentInfo{
                .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView   = colorTextureVulkan->getImageView(),
                .imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .loadOp      = toVkAttachmentLoadOp(colorAttachmentDesc._loadAction),
                .storeOp     = toVkAttachmentStoreOp(colorAttachmentDesc._storeAction),
                .clearValue  = { .color = { .float32 = {
                    colorAttachmentDesc._clearColors[0],
                    colorAttachmentDesc._clearColors[1],
                    colorAttachmentDesc._clearColors[2],
                    colorAttachmentDesc._clearColors[3] } } },
            });
        }

        const VkRenderingInfo renderingInfo{
            .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea           = { {0, 0}, {frameBufferDesc._width, frameBufferDesc._height} },
            .layerCount           = 1,
            .colorAttachmentCount = (uint32_t)colorAttachments.size(),
            .pColorAttachments    = colorAttachments.data(),
        };
        vkCmdBeginRendering(_vkCommandBuffer, &renderingInfo);
    }

    void VkmCommandBufferVulkan::onEndRenderPass()
    {
        vkCmdEndRendering(_vkCommandBuffer);

        // Transition presentable attachments back to PRESENT_SRC_KHR so the swapchain can
        // present them; non-presentable render targets (no AllowPresent flag) are left in
        // COLOR_ATTACHMENT_OPTIMAL for whatever consumes them next.
        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();
        for (uint32_t i = 0; i < _currentFrameBufferDesc._renderPass._colorAttachmentCount; ++i)
        {
            VkmTextureVulkan* colorTextureVulkan = static_cast<VkmTextureVulkan*>(renderResourcePool->getResource<VkmTexture>(_currentFrameBufferDesc._colorAttachments[i]));
            if ((colorTextureVulkan->getTextureInfo()._flags & VkmResourceCreateInfo::AllowPresent) != 0)
            {
                transitionImageLayout(_vkCommandBuffer, colorTextureVulkan->getImage(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
                colorTextureVulkan->setCurrentLayout(VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
            }
        }
    }

    void VkmCommandBufferVulkan::onBindPipeline(VkmPipelineStateBase* pipelineState)
    {
        VkmPipelineStateVulkan* pipelineStateVulkan = static_cast<VkmPipelineStateVulkan*>(pipelineState);
        const VkPipelineBindPoint bindPoint = pipelineState->isCompute() ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
        vkCmdBindPipeline(_vkCommandBuffer, bindPoint, pipelineStateVulkan->getHandle());
    }

    void VkmCommandBufferVulkan::onUnbindPipeline()
    {
        // Vulkan has no explicit unbind concept -- nothing to do here.
    }
} // namespace vkm
