// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/render_graph_capture.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/staging_buffer.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>

#include <algorithm>
#include <cstring>

namespace vkm
{
    namespace
    {
        VkmCapturedResourceInfo makeTextureResourceInfo(VkmRenderResourcePool* pool, VkmResourceHandle handle)
        {
            VkmCapturedResourceInfo info{};
            info.handle = handle;
            info.type = VkmResourceType::Texture;
            VkmTexture* texture = pool->getResource<VkmTexture>(handle);
            if (texture != nullptr)
            {
                const VkmTextureInfo& textureInfo = texture->getTextureInfo();
                info.debugName = textureInfo._debugName != nullptr ? textureInfo._debugName : "";
                info.format = textureInfo._format;
                info.extent = textureInfo._extent;
            }
            return info;
        }

        VkmCapturedResourceInfo makeBufferResourceInfo(VkmRenderResourcePool* pool, VkmResourceHandle handle)
        {
            VkmCapturedResourceInfo info{};
            info.handle = handle;
            info.type = handle.type;
            VkmBuffer* buffer = pool->getResource<VkmBuffer>(handle);
            if (buffer != nullptr)
            {
                const VkmBufferInfo& bufferInfo = buffer->getBufferInfo();
                info.debugName = bufferInfo._debugName != nullptr ? bufferInfo._debugName : "";
                info.size = bufferInfo._size;
            }
            return info;
        }
    }

    void VkmRenderGraphCapture::arm()
    {
        if (_state == State::Armed || _state == State::Pending)
        {
            return; // capture already in flight
        }
        _state = State::Armed;
    }

    void VkmRenderGraphCapture::beginCapture(VkmDriverBase* driver, uint32_t frameIndex)
    {
        VKM_ASSERT(_state == State::Armed, "beginCapture requires an armed capture");
        releaseResources(driver);
        _capturedFrameIndex = frameIndex;
        _hasContentCapture =
            (driver->getDriverCapabilityFlags() & VkmDriverCapabilityFlags::TextureContentCapture) != 0;
        _state = State::Pending;
    }

    void VkmRenderGraphCapture::recordSubGraph(VkmDriverBase* driver, VkmCommandBufferBase* commandBuffer,
                                               const VkmRenderSubGraph* subGraph, size_t pipelineHistoryBegin)
    {
        VKM_ASSERT(_state == State::Pending, "recordSubGraph requires a pending capture");
        VkmRenderResourcePool* pool = driver->getRenderResourcePool();

        VkmCapturedPass pass{};
        pass.subGraphId = subGraph->getSubGraphId();
        pass.name = subGraph->getName();
        pass.type = subGraph->getSubGraphType();
        pass.dependencies = subGraph->getDependentSubGraphIds();

        const std::vector<VkmPipelineStateBase*>& pipelineHistory = commandBuffer->getBoundPipelineHistory();
        for (size_t i = pipelineHistoryBegin; i < pipelineHistory.size(); ++i)
        {
            pass.pipelineNames.push_back(pipelineHistory[i]->getName());
        }

        if (pass.type == VkmRenderSubGraphType::Graphics)
        {
            const VkmFrameBufferDescriptor& frameBufferDesc =
                static_cast<const VkmRenderGraphicsSubGraph*>(subGraph)->getFrameBufferDescriptor();
            pass.width = frameBufferDesc._width;
            pass.height = frameBufferDesc._height;

            for (uint32_t i = 0; i < frameBufferDesc._renderPass._colorAttachmentCount; ++i)
            {
                VkmCapturedAttachment attachment{};
                attachment.info = makeTextureResourceInfo(pool, frameBufferDesc._colorAttachments[i]);
                attachment.loadAction = frameBufferDesc._renderPass._colorAttachments[i]._loadAction;
                attachment.storeAction = frameBufferDesc._renderPass._colorAttachments[i]._storeAction;

                VkmTexture* attachmentTexture = pool->getResource<VkmTexture>(frameBufferDesc._colorAttachments[i]);
                attachment.isPresentTarget = attachmentTexture != nullptr &&
                    (attachmentTexture->getTextureInfo()._flags & VkmResourceCreateInfo::AllowPresent) != 0;
                // Swapchain back buffers (AllowPresent) are framebufferOnly drawables on
                // Metal and cannot be copy sources -- metadata only.
                const bool snapshotable = _hasContentCapture && attachmentTexture != nullptr && !attachment.isPresentTarget;
                if (snapshotable)
                {
                    const VkmTextureInfo& srcInfo = attachmentTexture->getTextureInfo();
                    _snapshotNames.push_back("GraphCapture." + pass.name + ".color" + std::to_string(i));

                    VkmTextureInfo snapshotInfo{};
                    snapshotInfo._flags = VkmResourceCreateInfo::AllowTransferSrc |
                                          VkmResourceCreateInfo::AllowTransferDst |
                                          VkmResourceCreateInfo::AllowShaderRead;
                    snapshotInfo._extent = srcInfo._extent;
                    snapshotInfo._numMipLevels = 1;
                    snapshotInfo._numArrayLayers = 1;
                    snapshotInfo._format = srcInfo._format;
                    snapshotInfo._debugName = _snapshotNames.back().c_str();
                    VkmTexture* snapshotTexture = driver->newTexture(snapshotInfo);
                    if (snapshotTexture != nullptr)
                    {
                        commandBuffer->copyTexture(frameBufferDesc._colorAttachments[i], snapshotTexture->getHandle());
                        attachment.snapshotTexture = snapshotTexture->getHandle();
                        _snapshotTextures.push_back(snapshotTexture->getHandle());
                    }
                    else
                    {
                        VKM_DEBUG_ERROR("Render graph capture: failed to allocate snapshot texture");
                    }
                }
                pass.colorAttachments.push_back(std::move(attachment));
            }

            if (frameBufferDesc._depthStencilAttachment.has_value())
            {
                VkmCapturedAttachment depthAttachment{};
                depthAttachment.info = makeTextureResourceInfo(pool, *frameBufferDesc._depthStencilAttachment);
                if (frameBufferDesc._renderPass._depthStencilAttachment.has_value())
                {
                    depthAttachment.loadAction = frameBufferDesc._renderPass._depthStencilAttachment->_loadAction;
                    depthAttachment.storeAction = frameBufferDesc._renderPass._depthStencilAttachment->_storeAction;
                }
                pass.depthStencilAttachment = std::move(depthAttachment);
            }
        }

        for (VkmResourceHandle handle : subGraph->getReferencedResources())
        {
            if (handle.type == VkmResourceType::Buffer)
            {
                VkmCapturedBuffer capturedBuffer{};
                capturedBuffer.info = makeBufferResourceInfo(pool, handle);
                if (_hasContentCapture && capturedBuffer.info.size > 0)
                {
                    const uint64_t cappedSize = std::min(capturedBuffer.info.size, kMaxCapturedBufferBytes);
                    VkmStagingBufferInfo stagingInfo{};
                    stagingInfo._flags = VkmResourceCreateInfo::AllowTransferDst;
                    stagingInfo._size = cappedSize;
                    stagingInfo._debugName = "GraphCapture.BufferReadback";
                    VkmStagingBuffer* stagingBuffer = driver->newStagingBuffer(stagingInfo);
                    if (stagingBuffer != nullptr)
                    {
                        commandBuffer->copyBuffer(handle, stagingBuffer->getHandle(), 0, 0, cappedSize);
                        capturedBuffer.stagingBuffer = stagingBuffer->getHandle();
                    }
                    else
                    {
                        VKM_DEBUG_ERROR("Render graph capture: failed to allocate buffer readback staging");
                    }
                }
                pass.capturedBuffers.push_back(std::move(capturedBuffer));
                pass.referencedResources.push_back(makeBufferResourceInfo(pool, handle));
            }
            else if (handle.type == VkmResourceType::Texture)
            {
                pass.referencedResources.push_back(makeTextureResourceInfo(pool, handle));
            }
            else
            {
                VkmCapturedResourceInfo info{};
                info.handle = handle;
                info.type = handle.type;
                pass.referencedResources.push_back(std::move(info));
            }
        }

        _passes.push_back(std::move(pass));
    }

    void VkmRenderGraphCapture::finalize(VkmDriverBase* driver)
    {
        VKM_ASSERT(_state == State::Pending, "finalize requires a pending capture");
        VkmRenderResourcePool* pool = driver->getRenderResourcePool();

        // The engine guarantees the capture frame's submit completed (ensureCompleted())
        // before calling this, so staging contents are stable and immediate release is safe.
        for (VkmCapturedPass& pass : _passes)
        {
            for (VkmCapturedBuffer& capturedBuffer : pass.capturedBuffers)
            {
                if (!capturedBuffer.stagingBuffer.isValid())
                {
                    continue;
                }
                VkmStagingBuffer* stagingBuffer = pool->getResource<VkmStagingBuffer>(capturedBuffer.stagingBuffer);
                if (stagingBuffer != nullptr)
                {
                    const uint64_t cappedSize = std::min(capturedBuffer.info.size, kMaxCapturedBufferBytes);
                    const void* mapped = stagingBuffer->map();
                    capturedBuffer.data.resize(cappedSize);
                    std::memcpy(capturedBuffer.data.data(), mapped, cappedSize);
                    stagingBuffer->unmap();
                    pool->releaseResource(capturedBuffer.stagingBuffer);
                }
                capturedBuffer.stagingBuffer = VKM_INVALID_RESOURCE_HANDLE;
            }
        }

        _state = State::Ready;
    }

    void VkmRenderGraphCapture::releaseResources(VkmDriverBase* driver)
    {
        // Snapshot textures may still be referenced by in-flight ImGui draws -- the
        // deferred reclaimer waits on their recorded usage before the real release.
        for (VkmResourceHandle handle : _snapshotTextures)
        {
            driver->getDeferredReclaimer()->requestRelease(handle);
        }
        // Staging buffers only survive here if finalize() was never reached (e.g. shutdown
        // between execute() and finalize()); the reclaimer handles those safely too.
        for (VkmCapturedPass& pass : _passes)
        {
            for (VkmCapturedBuffer& capturedBuffer : pass.capturedBuffers)
            {
                if (capturedBuffer.stagingBuffer.isValid())
                {
                    driver->getDeferredReclaimer()->requestRelease(capturedBuffer.stagingBuffer);
                }
            }
        }
        _snapshotTextures.clear();
        _snapshotNames.clear();
        _passes.clear();
        _hasContentCapture = false;
        _state = State::Idle;
    }
}
