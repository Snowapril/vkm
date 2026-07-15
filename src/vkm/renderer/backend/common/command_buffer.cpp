// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/driver.h>

namespace vkm
{
    VkmCommandBufferBase::VkmCommandBufferBase(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue, VkmCommandBufferPoolBase* commandBufferPool)
        : _driver(driver), _commandQueue(commandQueue), _commandBufferPool(commandBufferPool), _isRecording(false), _isInRenderPass(false)
    {
    }

    VkmCommandBufferBase::~VkmCommandBufferBase()
    {

    }

    void VkmCommandBufferBase::beginCommandBuffer()
    {
        _isRecording = true;
        _isInRenderPass = false;
        // Reset the current frame buffer descriptor
        _currentFrameBufferDesc = {};
#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
        // Command buffers are pooled/reused across frames -- discard any subgraph IDs recorded
        // during a previous use before this one starts recording.
        _recordedSubgraphIds.clear();
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS

        VkmGpuEventTimelineBase* gpuEventTimeline = _commandQueue->getGpuEventTimeline();
        _gpuEventTimelineObject = gpuEventTimeline->allocateGpuEventTimelineObject();
    }
    
    void VkmCommandBufferBase::endCommandBuffer()
    {
#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
        onEndCommandBuffer();
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS
        _isRecording = false;
        _isInRenderPass = false;
    }
    
    void VkmCommandBufferBase::beginRenderPass(const VkmFrameBufferDescriptor& frameBufferDesc)
    {
        if (!_isRecording)
        {
            VKM_DEBUG_ERROR("Cannot begin render pass when command buffer is not recording");
            return;
        }
        _isInRenderPass = true;
        // Here we would typically set up the render pass state and bind the frame buffer
        // For now, we just log the beginning of the render pass
        VKM_DEBUG_LOG("Beginning render pass with frame buffer descriptor");

        _currentFrameBufferDesc = frameBufferDesc;

        onBeginRenderPass(frameBufferDesc);
    }

    void VkmCommandBufferBase::endRenderPass()
    {
        if (!_isInRenderPass)
        {
            VKM_DEBUG_ERROR("Cannot end render pass when not in a render pass");
            return;
        }
        _isInRenderPass = false;
        // Here we would typically finalize the render pass state
        // For now, we just log the end of the render pass
        VKM_DEBUG_LOG("Ending render pass");

        onEndRenderPass();
    }
    
    void VkmCommandBufferBase::bindPipeline(VkmPipelineStateBase* pipelineState)
    {
        _boundPipelineState = pipelineState;
        onBindPipeline(pipelineState);
    }

    void VkmCommandBufferBase::unbindPipeline()
    {
        onUnbindPipeline();
        _boundPipelineState = nullptr;
    }

    void VkmCommandBufferBase::copyBuffer(VkmResourceHandle srcBuffer, VkmResourceHandle dstBuffer, uint64_t srcOffset, uint64_t dstOffset, uint64_t size)
    {
        if (!_isRecording || _isInRenderPass)
        {
            VKM_DEBUG_ERROR("copyBuffer must be called while recording and outside a render pass");
            return;
        }
        onCopyBuffer(srcBuffer, dstBuffer, srcOffset, dstOffset, size);
    }

    void VkmCommandBufferBase::copyTextureToBuffer(VkmResourceHandle srcTexture, VkmResourceHandle dstBuffer, uint64_t dstOffset)
    {
        if (!_isRecording || _isInRenderPass)
        {
            VKM_DEBUG_ERROR("copyTextureToBuffer must be called while recording and outside a render pass");
            return;
        }
        onCopyTextureToBuffer(srcTexture, dstBuffer, dstOffset);
    }

    void VkmCommandBufferBase::draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance)
    {
        if (!_isRecording || !_isInRenderPass || _boundPipelineState == nullptr)
        {
            VKM_DEBUG_ERROR("draw requires an active render pass with a bound pipeline");
            return;
        }
        onDraw(vertexCount, instanceCount, firstVertex, firstInstance);
    }

    void VkmCommandBufferBase::setPushConstants(const void* data, uint32_t size, uint32_t offset)
    {
        if (!_isRecording || !_isInRenderPass || _boundPipelineState == nullptr)
        {
            VKM_DEBUG_ERROR("setPushConstants requires an active render pass with a bound pipeline");
            return;
        }
        onSetPushConstants(data, size, offset);
    }

#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
    void VkmCommandBufferBase::writeCompletionMarker(VkmResourceHandle markerBuffer, VkmResourceHandle oneBuffer, uint32_t subGraphId, uint32_t offset)
    {
        _recordedSubgraphIds.push_back(subGraphId);
        onWriteCompletionMarker(markerBuffer, oneBuffer, offset);
    }
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS

    void VkmCommandBufferBase::setDebugName(const char* name)
    {
        _debugName = name != nullptr ? name : "";

        if (_driver->isDebugNamingEnabled() && name != nullptr)
        {
            onSetDebugName(name);
        }
    }
}