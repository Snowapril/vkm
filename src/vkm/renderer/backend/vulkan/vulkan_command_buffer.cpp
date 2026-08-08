// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_command_buffer.h>
#include <vkm/renderer/backend/vulkan/vulkan_barrier.h>
#include <vkm/renderer/backend/vulkan/vulkan_command_queue.h>
#include <vkm/renderer/backend/vulkan/vulkan_texture.h>
#include <vkm/renderer/backend/vulkan/vulkan_buffer.h>
#include <vkm/renderer/backend/vulkan/vulkan_staging_buffer.h>
#include <vkm/renderer/backend/vulkan/vulkan_pipeline_state.h>
#include <vkm/renderer/backend/vulkan/vulkan_resource_table.h>
#include <vkm/renderer/backend/vulkan/vulkan_acceleration_structure.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_bindless_resource_manager.h>
#include <vkm/renderer/backend/vulkan/vulkan_util.h>
#include <vkm/renderer/backend/vulkan/vulkan_frame_constant_manager.h>
#include <vkm/renderer/backend/common/render_pass.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/common/driver.h>
#include <array>
#include <algorithm>

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
    static void transitionImageLayout(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
                                      VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                      const VkImageSubresourceRange* subresourceRange = nullptr)
    {
        const VkImageSubresourceRange wholeImage{ aspectMask, 0, VK_REMAINING_MIP_LEVELS, 0,
                                                  VK_REMAINING_ARRAY_LAYERS };
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
            .subresourceRange    = (subresourceRange != nullptr) ? *subresourceRange : wholeImage,
        };
        const VkDependencyInfo dependencyInfo{
            .sType                   = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .imageMemoryBarrierCount = 1,
            .pImageMemoryBarriers    = &barrier,
        };
        vkCmdPipelineBarrier2(commandBuffer, &dependencyInfo);
    }

    /*
    * @brief Moves an entire texture to `newLayout`, whatever its subresources are in now, and
    * records the result.
    *
    * @details One barrier while the texture's layout is uniform, which is the overwhelmingly
    * common case. A texture that some earlier partial transition left non-uniform -- a cubemap
    * mid-upload, a probe atlas cell rewritten on its own -- needs one barrier per subresource
    * instead, because `oldLayout` has to name what that subresource really is: naming the wrong
    * one is undefined behaviour and a validation error, not a missed optimisation.
    */
    static void transitionWholeTexture(VkCommandBuffer commandBuffer, VkmTextureVulkan* textureVulkan,
                                       VkImageLayout newLayout, VkImageAspectFlags aspectMask)
    {
        const VkmSubresourceRange wholeRange{};
        if (textureVulkan->isLayoutUniform())
        {
            const VkImageLayout oldLayout = textureVulkan->getSubresourceLayout(0, 0);
            if (oldLayout != newLayout)
            {
                transitionImageLayout(commandBuffer, textureVulkan->getImage(), oldLayout, newLayout, aspectMask);
            }
            textureVulkan->setSubresourceLayout(wholeRange, newLayout);
            return;
        }

        const VkmTextureInfo& info = textureVulkan->getTextureInfo();
        const uint32_t mipCount = std::max(1u, info._numMipLevels);
        const uint32_t layerCount = std::max(1u, info._numArrayLayers);
        for (uint32_t layer = 0; layer < layerCount; ++layer)
        {
            for (uint32_t mip = 0; mip < mipCount; ++mip)
            {
                const VkImageLayout oldLayout = textureVulkan->getSubresourceLayout(mip, layer);
                if (oldLayout == newLayout)
                {
                    continue;
                }
                const VkImageSubresourceRange subresource{ aspectMask, mip, 1, layer, 1 };
                transitionImageLayout(commandBuffer, textureVulkan->getImage(), oldLayout, newLayout,
                                      aspectMask, &subresource);
            }
        }
        textureVulkan->setSubresourceLayout(wholeRange, newLayout);
    }

    // Moves one subresource, leaving every other one alone -- what a per-face cubemap upload and
    // a single-cell atlas rewrite need.
    static void transitionTextureSubresource(VkCommandBuffer commandBuffer, VkmTextureVulkan* textureVulkan,
                                             uint32_t mipLevel, uint32_t arrayLayer,
                                             VkImageLayout newLayout, VkImageAspectFlags aspectMask)
    {
        const VkImageLayout oldLayout = textureVulkan->getSubresourceLayout(mipLevel, arrayLayer);
        if (oldLayout != newLayout)
        {
            const VkImageSubresourceRange subresource{ aspectMask, mipLevel, 1, arrayLayer, 1 };
            transitionImageLayout(commandBuffer, textureVulkan->getImage(), oldLayout, newLayout, aspectMask,
                                  &subresource);
        }
        textureVulkan->setSubresourceLayout(VkmSubresourceRange{ mipLevel, 1, arrayLayer, 1 }, newLayout);
    }

    // copyBuffer() is a generic buffer-to-buffer copy usable for either a Buffer or a
    // StagingBuffer resource on either side (e.g. VkmDriverBase::uploadToBuffer() copies
    // staging -> device-local) -- these are unrelated classes (VkmStagingBuffer doesn't
    // derive from VkmBuffer), so resolve via the handle's own recorded type rather than
    // assuming one.
    static VkBuffer resolveVkBufferAndOffset(VkmRenderResourcePool* renderResourcePool, VkmResourceHandle handle, uint64_t* outOffset)
    {
        if (handle.type == VkmResourceType::StagingBuffer)
        {
            VkmStagingBufferVulkan* stagingBufferVulkan = static_cast<VkmStagingBufferVulkan*>(renderResourcePool->getResource<VkmStagingBuffer>(handle));
            *outOffset = 0; // staging buffers are always dedicated allocations, never pooled
            return stagingBufferVulkan->getBuffer();
        }
        VkmBufferVulkan* bufferVulkan = static_cast<VkmBufferVulkan*>(renderResourcePool->getResource<VkmBuffer>(handle));
        *outOffset = bufferVulkan->getBufferOffset();
        return bufferVulkan->getBuffer();
    }

    VkmCommandBufferVulkan::VkmCommandBufferVulkan(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue, VkmCommandBufferPoolBase* commandBufferPool)
        : VkmCommandBufferBase(driver, commandQueue, commandBufferPool)
    {
    }

    VkmCommandBufferVulkan::~VkmCommandBufferVulkan()
    {
        // The pool owns the native lifetime: any handle still held here is freed when
        // VkmCommandBufferPoolVulkan destroys its VkCommandPool. Freeing it here would be
        // wrong anyway -- this destructor, if it ever ran, would run after that pool.
    }

    void VkmCommandBufferVulkan::setRHICommandBuffer(VKM_COMMAND_BUFFER_HANDLE handle)
    {
        // Hand the outgoing handle back so the pool can free it once its submission
        // completes; without this every acquire allocated a fresh VkCommandBuffer and none
        // was ever freed before device teardown. _gpuEventTimelineObject still refers to the
        // previous use's submission here -- beginCommandBuffer() allocates the next one only
        // after this returns.
        static_cast<VkmCommandBufferPoolVulkan*>(_commandBufferPool)
            ->retireRHICommandBuffer(_vkCommandBuffer, getGpuEventTimelineObject());
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

            transitionWholeTexture(_vkCommandBuffer, colorTextureVulkan,
                                   VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

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

        VkRenderingAttachmentInfo depthAttachment{};
        VkRenderingAttachmentInfo stencilAttachment{};
        bool hasDepthAttachment = false;
        bool hasStencilAttachment = false;
        if (frameBufferDesc._depthStencilAttachment.has_value() &&
            frameBufferDesc._renderPass._depthStencilAttachment.has_value())
        {
            VkmTextureVulkan* depthTextureVulkan = static_cast<VkmTextureVulkan*>(
                renderResourcePool->getResource<VkmTexture>(frameBufferDesc._depthStencilAttachment.value()));
            const VkmDepthStencilAttachmentDescriptor& depthAttachmentDesc =
                frameBufferDesc._renderPass._depthStencilAttachment.value();
            const VkmFormat depthFormat = depthTextureVulkan->getTextureInfo()._format;

            hasDepthAttachment = hasDepth(depthFormat);
            hasStencilAttachment = hasStencil(depthFormat);

            const VkImageAspectFlags aspectMask =
                (hasDepthAttachment ? VK_IMAGE_ASPECT_DEPTH_BIT : 0u) |
                (hasStencilAttachment ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);

            transitionWholeTexture(_vkCommandBuffer, depthTextureVulkan,
                                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, aspectMask);

            const VkRenderingAttachmentInfo attachmentInfo{
                .sType       = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                .imageView   = depthTextureVulkan->getImageView(),
                .imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                .loadOp      = toVkAttachmentLoadOp(depthAttachmentDesc._loadAction),
                .storeOp     = toVkAttachmentStoreOp(depthAttachmentDesc._storeAction),
                .clearValue  = { .depthStencil = { depthAttachmentDesc._clearDepth, depthAttachmentDesc._clearStencil } },
            };
            depthAttachment = attachmentInfo;
            stencilAttachment = attachmentInfo;
        }

        const VkRenderingInfo renderingInfo{
            .sType                = VK_STRUCTURE_TYPE_RENDERING_INFO,
            .renderArea           = { {0, 0}, {frameBufferDesc._width, frameBufferDesc._height} },
            .layerCount           = 1,
            .colorAttachmentCount = (uint32_t)colorAttachments.size(),
            .pColorAttachments    = colorAttachments.data(),
            .pDepthAttachment     = hasDepthAttachment ? &depthAttachment : nullptr,
            .pStencilAttachment   = hasStencilAttachment ? &stencilAttachment : nullptr,
        };
        vkCmdBeginRendering(_vkCommandBuffer, &renderingInfo);

        // Every pipeline created by VkmPipelineStateVulkan marks viewport and scissor as dynamic
        // state, extents not being known at pipeline-creation time, so they must be set here every
        // render pass or drawing has undefined viewport/scissor state.
        //
        // Plain positive-height viewport: vkm-compiler's -fvk-invert-y already normalizes the
        // engine's +Y-up clip space to Vulkan's +Y-down NDC, so nothing is compensated here.
        //
        // The pass-wide default; setViewportAndScissor() narrows it afterwards if a caller wants
        // to pack several views into this attachment.
        onSetViewportAndScissor(0, 0, frameBufferDesc._width, frameBufferDesc._height);
    }

    void VkmCommandBufferVulkan::onSetViewportAndScissor(int32_t x, int32_t y, uint32_t width, uint32_t height)
    {
        // A plain positive-height viewport: the engine's +Y-up clip space reaches Vulkan's +Y-down
        // NDC through vkm-compiler's -fvk-invert-y, so there is nothing to compensate here (see
        // onBeginRenderPass).
        const VkViewport viewport{
            .x = static_cast<float>(x), .y = static_cast<float>(y),
            .width = static_cast<float>(width), .height = static_cast<float>(height),
            .minDepth = 0.0f, .maxDepth = 1.0f,
        };
        const VkRect2D scissor{ {x, y}, {width, height} };
        vkCmdSetViewport(_vkCommandBuffer, 0, 1, &viewport);
        vkCmdSetScissor(_vkCommandBuffer, 0, 1, &scissor);
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
                transitionWholeTexture(_vkCommandBuffer, colorTextureVulkan,
                                       VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_ASPECT_COLOR_BIT);
            }
        }
    }

    void VkmCommandBufferVulkan::onBindPipeline(VkmPipelineStateBase* pipelineState)
    {
        VkmPipelineStateVulkan* pipelineStateVulkan = static_cast<VkmPipelineStateVulkan*>(pipelineState);
        const VkPipelineBindPoint bindPoint = pipelineState->isCompute() ? VK_PIPELINE_BIND_POINT_COMPUTE : VK_PIPELINE_BIND_POINT_GRAPHICS;
        vkCmdBindPipeline(_vkCommandBuffer, bindPoint, pipelineStateVulkan->getHandle());

        _boundPipelineLayout = pipelineStateVulkan->getPipelineLayout();

        // Sets 0 and 1 are engine-global for every pipeline in this convention (see
        // VkmPipelineStateVulkan::createInner), so bind them here rather than asking every
        // draw call site to do so explicitly. Set 1 resolves to the frame slot the engine
        // wrote this frame.
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        const std::array<VkDescriptorSet, 2> descriptorSets{
            driverVulkan->getBindlessResourceManager()->getDescriptorSet(),
            driverVulkan->getFrameConstantManager()->getActiveDescriptorSet(),
        };
        vkCmdBindDescriptorSets(_vkCommandBuffer, bindPoint, _boundPipelineLayout, 0,
                                static_cast<uint32_t>(descriptorSets.size()), descriptorSets.data(), 0, nullptr);
    }

    void VkmCommandBufferVulkan::onUnbindPipeline()
    {
        // Vulkan has no explicit unbind concept -- nothing to do here.
        _boundPipelineLayout = VK_NULL_HANDLE;
    }

    void VkmCommandBufferVulkan::onCopyBuffer(VkmResourceHandle srcBuffer, VkmResourceHandle dstBuffer, uint64_t srcOffset, uint64_t dstOffset, uint64_t size)
    {
        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();
        uint64_t srcBaseOffset = 0, dstBaseOffset = 0;
        VkBuffer srcVkBuffer = resolveVkBufferAndOffset(renderResourcePool, srcBuffer, &srcBaseOffset);
        VkBuffer dstVkBuffer = resolveVkBufferAndOffset(renderResourcePool, dstBuffer, &dstBaseOffset);

        const VkBufferCopy copyRegion{
            .srcOffset = srcOffset + srcBaseOffset,
            .dstOffset = dstOffset + dstBaseOffset,
            .size      = size,
        };
        vkCmdCopyBuffer(_vkCommandBuffer, srcVkBuffer, dstVkBuffer, 1, &copyRegion);
    }

    void VkmCommandBufferVulkan::onCopyTexture(VkmResourceHandle srcTexture, VkmResourceHandle dstTexture)
    {
        // Texture-to-texture copies (render graph capture snapshots) are Metal-only for
        // now -- see VkmDriverCapabilityFlags::TextureContentCapture.
        VKM_DEBUG_ERROR("copyTexture is not implemented on the Vulkan backend");
    }

    void VkmCommandBufferVulkan::onCopyTextureToBuffer(VkmResourceHandle srcTexture, VkmResourceHandle dstBuffer, uint64_t dstOffset, uint32_t arrayLayer)
    {
        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();
        VkmTextureVulkan* textureVulkan = static_cast<VkmTextureVulkan*>(renderResourcePool->getResource<VkmTexture>(srcTexture));
        uint64_t dstBaseOffset = 0;
        VkBuffer dstVkBuffer = resolveVkBufferAndOffset(renderResourcePool, dstBuffer, &dstBaseOffset);

        const VkmTextureInfo& textureInfo = textureVulkan->getTextureInfo();
        // Only the layer being read back, so a readback of one face of a cubemap does not move
        // the other five.
        const VkImageLayout previousLayout = textureVulkan->getSubresourceLayout(0, arrayLayer);
        transitionTextureSubresource(_vkCommandBuffer, textureVulkan, 0, arrayLayer,
                                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

        const VkBufferImageCopy region{
            .bufferOffset      = dstOffset + dstBaseOffset,
            .bufferRowLength   = 0, // tightly packed
            .bufferImageHeight = 0,
            .imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, 0, arrayLayer, 1 },
            .imageOffset       = { 0, 0, 0 },
            .imageExtent       = { textureInfo._extent.x, textureInfo._extent.y, 1 },
        };
        vkCmdCopyImageToBuffer(_vkCommandBuffer, textureVulkan->getImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstVkBuffer, 1, &region);

        // Leave the texture as we found it so later render passes/present see the layout
        // the tracker says it is in. UNDEFINED is not a valid transition target (and means
        // the texture was never rendered to anyway) -- keep TRANSFER_SRC and track it.
        if (previousLayout != VK_IMAGE_LAYOUT_UNDEFINED)
        {
            transitionTextureSubresource(_vkCommandBuffer, textureVulkan, 0, arrayLayer, previousLayout,
                                         VK_IMAGE_ASPECT_COLOR_BIT);
        }
    }

    void VkmCommandBufferVulkan::onCopyBufferToTexture(VkmResourceHandle srcBuffer, VkmResourceHandle dstTexture,
                                                       uint64_t srcOffset, uint32_t mipLevel, uint32_t arrayLayer)
    {
        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();
        VkmTextureVulkan* textureVulkan = static_cast<VkmTextureVulkan*>(renderResourcePool->getResource<VkmTexture>(dstTexture));
        uint64_t srcBaseOffset = 0;
        VkBuffer srcVkBuffer = resolveVkBufferAndOffset(renderResourcePool, srcBuffer, &srcBaseOffset);

        const VkmTextureInfo& textureInfo = textureVulkan->getTextureInfo();
        // One subresource, not the whole image. This is the call a cubemap upload makes six times,
        // and it used to move all six faces on every one of them -- so five of the six were left
        // recorded as being in a layout their contents had not been written in yet.
        transitionTextureSubresource(_vkCommandBuffer, textureVulkan, mipLevel, arrayLayer,
                                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);

        const VkBufferImageCopy region{
            .bufferOffset      = srcOffset + srcBaseOffset,
            .bufferRowLength   = 0, // tightly packed
            .bufferImageHeight = 0,
            .imageSubresource  = { VK_IMAGE_ASPECT_COLOR_BIT, mipLevel, arrayLayer, 1 },
            .imageOffset       = { 0, 0, 0 },
            .imageExtent       = {
                std::max(1u, textureInfo._extent.x >> mipLevel),
                std::max(1u, textureInfo._extent.y >> mipLevel),
                std::max(1u, textureInfo._extent.z >> mipLevel),
            },
        };
        vkCmdCopyBufferToImage(_vkCommandBuffer, srcVkBuffer, textureVulkan->getImage(),
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        // Leave it sampleable, which is what every caller wants next (see the contract on
        // VkmCommandBufferBase::copyBufferToTexture).
        transitionTextureSubresource(_vkCommandBuffer, textureVulkan, mipLevel, arrayLayer,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_ASPECT_COLOR_BIT);
    }

    void VkmCommandBufferVulkan::onDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
    {
        vkCmdDraw(_vkCommandBuffer, vertexCount, instanceCount, firstVertex, firstInstance);
    }

    void VkmCommandBufferVulkan::onDrawIndirectCount(VkmIndirectArgumentLayout layout,
                                                     VkmResourceHandle argumentBuffer, uint64_t argumentOffset,
                                                     VkmResourceHandle countBuffer, uint64_t countOffset,
                                                     uint32_t maxDrawCount)
    {
        const uint32_t argumentStride = vkmGetIndirectArgumentStride(layout);

        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();
        VkmBufferVulkan* argumentBufferVulkan =
            static_cast<VkmBufferVulkan*>(renderResourcePool->getResource<VkmBuffer>(argumentBuffer));
        if (argumentBufferVulkan == nullptr)
        {
            VKM_DEBUG_ERROR("drawIndirectCount was given a handle that is not a live buffer");
            return;
        }
        // Small buffers may be sub-allocated into a shared VkBuffer, so the pool offset counts.
        const VkDeviceSize argumentBase = argumentBufferVulkan->getBufferOffset() + argumentOffset;

        if (driverVulkan->isDrawIndirectCountSupported())
        {
            VkmBufferVulkan* countBufferVulkan =
                static_cast<VkmBufferVulkan*>(renderResourcePool->getResource<VkmBuffer>(countBuffer));
            if (countBufferVulkan != nullptr)
            {
                vkCmdDrawIndirectCount(_vkCommandBuffer,
                                       argumentBufferVulkan->getBuffer(), argumentBase,
                                       countBufferVulkan->getBuffer(),
                                       countBufferVulkan->getBufferOffset() + countOffset,
                                       maxDrawCount, argumentStride);
                return;
            }
        }

        // Fallback for drivers without the drawIndirectCount feature (MoltenVK): issue the whole
        // range. This is correct rather than merely close, because the producing pass compacts
        // survivors to the front and zeroes the tail, and an all-zero record draws nothing.
        vkCmdDrawIndirect(_vkCommandBuffer, argumentBufferVulkan->getBuffer(), argumentBase,
                          maxDrawCount, argumentStride);
    }

    void VkmCommandBufferVulkan::onDispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
    {
        vkCmdDispatch(_vkCommandBuffer, groupCountX, groupCountY, groupCountZ);
    }

    void VkmCommandBufferVulkan::onBuildAccelerationStructure(VkmResourceHandle accelerationStructure)
    {
        VkmAccelerationStructure* structure =
            _driver->getRenderResourcePool()->getResource<VkmAccelerationStructure>(accelerationStructure);
        if (structure == nullptr)
        {
            VKM_DEBUG_ERROR("buildAccelerationStructure: invalid acceleration structure handle");
            return;
        }

        VkmAccelerationStructureVulkan* structureVulkan = static_cast<VkmAccelerationStructureVulkan*>(structure);
        if (structureVulkan->getAccelerationStructure() == VK_NULL_HANDLE || !structureVulkan->isRebuildable())
        {
            // No scratch means the structure was built without _allowUpdate and its scratch was
            // freed; rebuilding would read whatever now occupies that memory.
            VKM_DEBUG_ERROR("buildAccelerationStructure on a structure that was not created with _allowUpdate");
            return;
        }

        VkAccelerationStructureBuildGeometryInfoKHR buildInfo{
            .sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR,
            .type  = structureVulkan->getType() == VkmAccelerationStructureType::TopLevel
                         ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR
                         : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
            .flags = static_cast<VkBuildAccelerationStructureFlagsKHR>(
                         VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR |
                         (structureVulkan->allowsUpdate() ? VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR : 0)),
            // BUILD rather than UPDATE. A top-level rebuild over its instances is cheap and stays
            // optimal; an update is faster still but degrades traversal as instances drift from
            // where the structure was built, and the dynamic case here is a rigid body moving a
            // long way. Refit belongs to deforming geometry, which nothing produces yet.
            .mode                     = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
            .dstAccelerationStructure = structureVulkan->getAccelerationStructure(),
            .geometryCount            = static_cast<uint32_t>(structureVulkan->getGeometries().size()),
            .pGeometries              = structureVulkan->getGeometries().data(),
        };
        buildInfo.scratchData.deviceAddress = structureVulkan->getScratchAddress();

        std::vector<VkAccelerationStructureBuildRangeInfoKHR> ranges;
        ranges.reserve(structureVulkan->getPrimitiveCounts().size());
        for (uint32_t count : structureVulkan->getPrimitiveCounts())
        {
            ranges.push_back(VkAccelerationStructureBuildRangeInfoKHR{ .primitiveCount = count });
        }
        const VkAccelerationStructureBuildRangeInfoKHR* rangePointer = ranges.data();
        vkCmdBuildAccelerationStructuresKHR(_vkCommandBuffer, 1, &buildInfo, &rangePointer);
    }

    void VkmCommandBufferVulkan::onResourceBarrier(const VkmResourceBarrier* barriers, uint32_t count)
    {
        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();

        // Members rather than locals: this runs at every subgraph boundary of every frame, and the
        // vectors keep their capacity across calls.
        _imageBarrierScratch.clear();
        _bufferBarrierScratch.clear();
        VkMemoryBarrier2 memoryBarrier{ .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
        bool hasMemoryBarrier = false;

        for (uint32_t i = 0; i < count; ++i)
        {
            const VkmResourceBarrier& barrier = barriers[i];
            const VkPipelineStageFlags2 srcStage = vkmToVkStageMask(barrier._srcAccess, barrier._srcScope);
            const VkPipelineStageFlags2 dstStage = vkmToVkStageMask(barrier._dstAccess, barrier._dstScope);
            // A write-after-read needs the two sides ordered, not the caches flushed: the source
            // only read, so it published nothing.
            const VkAccessFlags2 srcAccess =
                barrier._executionOnly ? VK_ACCESS_2_NONE : vkmToVkAccessMask(barrier._srcAccess);
            const VkAccessFlags2 dstAccess = vkmToVkAccessMask(barrier._dstAccess);

            if (barrier._handle.type == VkmResourceType::Texture)
            {
                VkmTextureVulkan* textureVulkan =
                    static_cast<VkmTextureVulkan*>(renderResourcePool->getResource<VkmTexture>(barrier._handle));
                if (textureVulkan == nullptr)
                {
                    VKM_DEBUG_ERROR("resourceBarrier was given a handle that is not a live texture");
                    continue;
                }

                const VkmTextureInfo& info = textureVulkan->getTextureInfo();
                const VkImageAspectFlags aspectMask = vkmToVkAspectMask(info._format);
                const VkImageLayout newLayout = vkmToVkImageLayout(barrier._dstAccess, info._format);
                if (newLayout == VK_IMAGE_LAYOUT_UNDEFINED)
                {
                    // An access that names no layout on a texture: order it, but do not pretend to
                    // know where the image should end up.
                    hasMemoryBarrier = true;
                    memoryBarrier.srcStageMask |= srcStage;
                    memoryBarrier.dstStageMask |= dstStage;
                    memoryBarrier.srcAccessMask |= srcAccess;
                    memoryBarrier.dstAccessMask |= dstAccess;
                    continue;
                }

                const uint32_t mipCount = std::max(1u, info._numMipLevels);
                const uint32_t layerCount = std::max(1u, info._numArrayLayers);
                const uint32_t firstMip = std::min(barrier._range._baseMipLevel, mipCount - 1u);
                const uint32_t firstLayer = std::min(barrier._range._baseArrayLayer, layerCount - 1u);
                const uint32_t lastMip =
                    (barrier._range._mipLevelCount == VKM_ALL_REMAINING_SUBRESOURCES)
                        ? mipCount - 1u
                        : std::min(firstMip + barrier._range._mipLevelCount - 1u, mipCount - 1u);
                const uint32_t lastLayer =
                    (barrier._range._arrayLayerCount == VKM_ALL_REMAINING_SUBRESOURCES)
                        ? layerCount - 1u
                        : std::min(firstLayer + barrier._range._arrayLayerCount - 1u, layerCount - 1u);

                /*
                * oldLayout comes from the texture's own tracker, never from the plan. The graph
                * knows the hazards inside one frame; what a texture carried in from a host upload,
                * a previous frame, or a swapchain acquire is backend knowledge, and naming the
                * wrong oldLayout is undefined behaviour rather than a missed optimisation.
                *
                * One barrier per subresource when the range's layouts disagree, one for the whole
                * range when they do not -- which is the usual case and the one that has to stay
                * cheap.
                */
                const VkmSubresourceRange resolvedRange{ firstMip, lastMip - firstMip + 1u, firstLayer,
                                                         lastLayer - firstLayer + 1u };
                const VkImageLayout sharedOldLayout = textureVulkan->getUniformLayout(resolvedRange);
                const auto pushImageBarrier = [&](VkImageLayout oldLayout, uint32_t baseMip, uint32_t mips,
                                                  uint32_t baseLayer, uint32_t layers) {
                    if (oldLayout == newLayout && !vkmIsWriteAccess(barrier._srcAccess) &&
                        !vkmIsWriteAccess(barrier._dstAccess))
                    {
                        return; // nothing to transition and nothing to publish
                    }
                    _imageBarrierScratch.push_back(VkImageMemoryBarrier2{
                        .sType               = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                        .srcStageMask        = srcStage,
                        .srcAccessMask       = srcAccess,
                        .dstStageMask        = dstStage,
                        .dstAccessMask       = dstAccess,
                        .oldLayout           = oldLayout,
                        .newLayout           = newLayout,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image               = textureVulkan->getImage(),
                        .subresourceRange    = { aspectMask, baseMip, mips, baseLayer, layers },
                    });
                };

                if (sharedOldLayout != VK_IMAGE_LAYOUT_MAX_ENUM)
                {
                    pushImageBarrier(sharedOldLayout, firstMip, lastMip - firstMip + 1u, firstLayer,
                                     lastLayer - firstLayer + 1u);
                }
                else
                {
                    for (uint32_t layer = firstLayer; layer <= lastLayer; ++layer)
                    {
                        for (uint32_t mip = firstMip; mip <= lastMip; ++mip)
                        {
                            pushImageBarrier(textureVulkan->getSubresourceLayout(mip, layer), mip, 1, layer, 1);
                        }
                    }
                }
                textureVulkan->setSubresourceLayout(resolvedRange, newLayout);
                continue;
            }

            if (barrier._handle.type == VkmResourceType::Buffer ||
                barrier._handle.type == VkmResourceType::StagingBuffer)
            {
                uint64_t offset = 0;
                VkBuffer vkBuffer = resolveVkBufferAndOffset(renderResourcePool, barrier._handle, &offset);
                if (vkBuffer == VK_NULL_HANDLE)
                {
                    VKM_DEBUG_ERROR("resourceBarrier was given a handle that is not a live buffer");
                    continue;
                }
                _bufferBarrierScratch.push_back(VkBufferMemoryBarrier2{
                    .sType               = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2,
                    .srcStageMask        = srcStage,
                    .srcAccessMask       = srcAccess,
                    .dstStageMask        = dstStage,
                    .dstAccessMask       = dstAccess,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .buffer              = vkBuffer,
                    .offset              = offset,
                    .size                = VK_WHOLE_SIZE,
                });
                continue;
            }

            // An acceleration structure exposes no VkBuffer of its own here, so its dependency
            // rides a global memory barrier instead.
            hasMemoryBarrier = true;
            memoryBarrier.srcStageMask |= srcStage;
            memoryBarrier.dstStageMask |= dstStage;
            memoryBarrier.srcAccessMask |= srcAccess;
            memoryBarrier.dstAccessMask |= dstAccess;
        }

        if (_imageBarrierScratch.empty() && _bufferBarrierScratch.empty() && !hasMemoryBarrier)
        {
            return;
        }

        // One vkCmdPipelineBarrier2 for the whole boundary, which is the point of the batched API.
        const VkDependencyInfo dependencyInfo{
            .sType                    = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
            .memoryBarrierCount       = hasMemoryBarrier ? 1u : 0u,
            .pMemoryBarriers          = hasMemoryBarrier ? &memoryBarrier : nullptr,
            .bufferMemoryBarrierCount = static_cast<uint32_t>(_bufferBarrierScratch.size()),
            .pBufferMemoryBarriers    = _bufferBarrierScratch.data(),
            .imageMemoryBarrierCount  = static_cast<uint32_t>(_imageBarrierScratch.size()),
            .pImageMemoryBarriers     = _imageBarrierScratch.data(),
        };
        vkCmdPipelineBarrier2(_vkCommandBuffer, &dependencyInfo);
    }

    void VkmCommandBufferVulkan::onAcquireAliasedTexture(VkmResourceHandle texture)
    {
        VkmTextureVulkan* textureVulkan =
            static_cast<VkmTextureVulkan*>(_driver->getRenderResourcePool()->getResource<VkmTexture>(texture));
        if (textureVulkan == nullptr)
        {
            VKM_DEBUG_ERROR("acquireAliasedTexture was given a handle that is not a live texture");
            return;
        }

        /*
        * UNDEFINED as the *source* is the discard: it tells the driver the previous contents --
        * which belong to whichever texture last held these bytes -- need not be preserved. It is
        * illegal as a destination (VUID-VkImageMemoryBarrier2-newLayout-01198), so this goes
        * straight to the attachment layout the first use needs, and the tracker is updated so the
        * barrier plan's own acquire sees where the image actually is.
        *
        * transitionImageLayout is ALL_COMMANDS + MEMORY_READ|WRITE on both sides over the whole
        * image, which is exactly the hazard scope aliasing needs: it orders this write after every
        * read of the previous alias, including ones submitted in earlier frames, because a
        * barrier's first scope covers everything earlier in submission order on the queue -- and
        * everything in this engine submits to the graphics queue.
        */
        const VkmFormat format = textureVulkan->getTextureInfo()._format;
        const VkImageAspectFlags aspectMask = vkmToVkAspectMask(format);
        const bool isDepth = (aspectMask & (VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)) != 0;
        const VkImageLayout acquiredLayout =
            isDepth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        transitionImageLayout(_vkCommandBuffer, textureVulkan->getImage(), VK_IMAGE_LAYOUT_UNDEFINED,
                              acquiredLayout, aspectMask);
        textureVulkan->setSubresourceLayout(VkmSubresourceRange{}, acquiredLayout);
    }

    void VkmCommandBufferVulkan::onBindResourceTable(VkmResourceTableBase* table)
    {
        VkmResourceTableVulkan* tableVulkan = static_cast<VkmResourceTableVulkan*>(table);
        VkDescriptorSet descriptorSet = tableVulkan->getDescriptorSet();

        // The bound pipeline's declaration matches the table's (checked in the base class), so its
        // layout is the right one to bind against; the table carries which set index it fills.
        const VkmPipelineStateVulkan* pipelineStateVulkan =
            static_cast<const VkmPipelineStateVulkan*>(getBoundPipelineState());
        const VkPipelineBindPoint bindPoint = pipelineStateVulkan->isCompute()
                                                  ? VK_PIPELINE_BIND_POINT_COMPUTE
                                                  : VK_PIPELINE_BIND_POINT_GRAPHICS;
        vkCmdBindDescriptorSets(_vkCommandBuffer, bindPoint, pipelineStateVulkan->getPipelineLayout(),
                                table->getSetIndex(), 1, &descriptorSet, 0, nullptr);
    }

    void VkmCommandBufferVulkan::onSetPushConstants(const void* data, uint32_t size, uint32_t offset)
    {
        // Must name the same stages the range was declared with (VUID-vkCmdPushConstants-offset-01795).
        vkCmdPushConstants(_vkCommandBuffer, _boundPipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_COMPUTE_BIT, offset, size, data);
    }

    void VkmCommandBufferVulkan::onBeginCommandBuffer()
    {
        _openGpuZoneEndSlots.clear();
    }

    void VkmCommandBufferVulkan::onBeginGpuZone(const uint32_t beginSlot, const uint32_t endSlot)
    {
        const VkQueryPool queryPool = static_cast<VkmDriverVulkan*>(_driver)->getGpuTimestampQueryPool();
        if (queryPool == VK_NULL_HANDLE)
        {
            return;
        }

        // TOP_OF_PIPE at the open and BOTTOM_OF_PIPE at the close, so the pair brackets every
        // stage of the work inside rather than only the stage the timestamp itself sits at.
        // vkCmdWriteTimestamp2 is legal inside a render pass, which is what lets a zone wrap a
        // whole subgraph without caring whether it opened one.
        vkCmdWriteTimestamp2(_vkCommandBuffer, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, queryPool, beginSlot);
        _openGpuZoneEndSlots.push_back(endSlot);
    }

    bool VkmCommandBufferVulkan::onEndGpuZone()
    {
        const VkQueryPool queryPool = static_cast<VkmDriverVulkan*>(_driver)->getGpuTimestampQueryPool();
        if (queryPool == VK_NULL_HANDLE || _openGpuZoneEndSlots.empty())
        {
            return false;
        }

        vkCmdWriteTimestamp2(_vkCommandBuffer, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, queryPool,
                             _openGpuZoneEndSlots.back());
        _openGpuZoneEndSlots.pop_back();
        return true;
    }

#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
    void VkmCommandBufferVulkan::onWriteCompletionMarker(VkmResourceHandle markerBuffer, VkmResourceHandle oneBuffer, uint32_t offset)
    {
        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();
        VkBuffer vkMarkerBuffer = static_cast<VkmStagingBufferVulkan*>(renderResourcePool->getResource<VkmStagingBuffer>(markerBuffer))->getBuffer();
        VkBuffer vkOneBuffer = static_cast<VkmStagingBufferVulkan*>(renderResourcePool->getResource<VkmStagingBuffer>(oneBuffer))->getBuffer();

        // Legal outside a render pass -- dynamic rendering has already ended by the time a
        // subgraph's commit() returns, which is the only place this is called from.
        const VkBufferCopy region{
            .srcOffset = 0,
            .dstOffset = offset,
            .size      = sizeof(uint32_t),
        };
        vkCmdCopyBuffer(_vkCommandBuffer, vkOneBuffer, vkMarkerBuffer, 1, &region);
    }

    void VkmCommandBufferVulkan::onEndCommandBuffer()
    {
        // No-op: onWriteCompletionMarker() already records its vkCmdCopyBuffer immediately,
        // no batching needed outside a render pass.
    }
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS

    void VkmCommandBufferVulkan::onSetDebugName(const char* name)
    {
#ifdef VKM_DEBUG_NAME_ENABLED
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        const VkDebugUtilsObjectNameInfoEXT nameInfo{
            .sType        = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT,
            .objectType   = VK_OBJECT_TYPE_COMMAND_BUFFER,
            .objectHandle = reinterpret_cast<uint64_t>(_vkCommandBuffer),
            .pObjectName  = name,
        };
        VKM_VK_CHECK_RESULT_MSG(vkSetDebugUtilsObjectNameEXT(driverVulkan->getDevice(), &nameInfo),
            "Failed to set debug name on command buffer");
#else
        (void)name;
#endif
    }

    void VkmCommandBufferVulkan::onPushDebugGroup(const char* name)
    {
#ifdef VKM_DEBUG_NAME_ENABLED
        // Requires VK_EXT_debug_utils (present with validation / a capturing tool like RenderDoc);
        // the function pointer is null otherwise, so guard on it.
        if (vkCmdBeginDebugUtilsLabelEXT != nullptr)
        {
            const VkDebugUtilsLabelEXT label{
                .sType      = VK_STRUCTURE_TYPE_DEBUG_UTILS_LABEL_EXT,
                .pLabelName = name,
            };
            vkCmdBeginDebugUtilsLabelEXT(_vkCommandBuffer, &label);
        }
#else
        (void)name;
#endif
    }

    void VkmCommandBufferVulkan::onPopDebugGroup()
    {
#ifdef VKM_DEBUG_NAME_ENABLED
        if (vkCmdEndDebugUtilsLabelEXT != nullptr)
        {
            vkCmdEndDebugUtilsLabelEXT(_vkCommandBuffer);
        }
#endif
    }
} // namespace vkm
