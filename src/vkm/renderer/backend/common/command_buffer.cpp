// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/per_pass_resource_table.h>

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
        // Command buffers are pooled/reused across frames -- discard bind history from a
        // previous use before this one starts recording.
        _boundPipelineHistory.clear();
#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
        // Command buffers are pooled/reused across frames -- discard any subgraph IDs recorded
        // during a previous use before this one starts recording.
        _recordedSubgraphIds.clear();
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS

        onBeginCommandBuffer();

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
        _boundPipelineHistory.push_back(pipelineState);
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

    void VkmCommandBufferBase::copyTexture(VkmResourceHandle srcTexture, VkmResourceHandle dstTexture)
    {
        if (!_isRecording || _isInRenderPass)
        {
            VKM_DEBUG_ERROR("copyTexture must be called while recording and outside a render pass");
            return;
        }
        onCopyTexture(srcTexture, dstTexture);
    }

    void VkmCommandBufferBase::copyTextureToBuffer(VkmResourceHandle srcTexture, VkmResourceHandle dstBuffer, uint64_t dstOffset,
                                                  uint32_t arrayLayer)
    {
        if (!_isRecording || _isInRenderPass)
        {
            VKM_DEBUG_ERROR("copyTextureToBuffer must be called while recording and outside a render pass");
            return;
        }
        onCopyTextureToBuffer(srcTexture, dstBuffer, dstOffset, arrayLayer);
    }

    void VkmCommandBufferBase::copyBufferToTexture(VkmResourceHandle srcBuffer, VkmResourceHandle dstTexture,
                                                   uint64_t srcOffset, uint32_t mipLevel, uint32_t arrayLayer)
    {
        if (!_isRecording || _isInRenderPass)
        {
            VKM_DEBUG_ERROR("copyBufferToTexture must be called while recording and outside a render pass");
            return;
        }
        onCopyBufferToTexture(srcBuffer, dstTexture, srcOffset, mipLevel, arrayLayer);
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

    void VkmCommandBufferBase::drawIndirectCount(VkmIndirectArgumentLayout layout,
                                                 VkmResourceHandle argumentBuffer, uint64_t argumentOffset,
                                                 VkmResourceHandle countBuffer, uint64_t countOffset,
                                                 uint32_t maxDrawCount)
    {
        if (!_isRecording || !_isInRenderPass || _boundPipelineState == nullptr)
        {
            VKM_DEBUG_ERROR("drawIndirectCount requires an active render pass with a bound pipeline");
            return;
        }
        if (layout != VkmIndirectArgumentLayout::NonIndexed)
        {
            // Rejected in one place so no backend has to carry an unreachable branch. Lifting this
            // needs an index-buffer binding path first -- the engine has none, because indices are
            // pulled in the vertex shader out of a bindless storage buffer.
            VKM_DEBUG_ERROR("drawIndirectCount only supports NonIndexed arguments: the engine has no bound index buffer");
            return;
        }
        if (maxDrawCount == 0)
        {
            return; // An empty batch is not an error; there is simply nothing to encode.
        }
        onDrawIndirectCount(layout, argumentBuffer, argumentOffset, countBuffer, countOffset, maxDrawCount);
    }

    void VkmCommandBufferBase::dispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ)
    {
        if (!_isRecording || _isInRenderPass || _boundPipelineState == nullptr)
        {
            VKM_DEBUG_ERROR("dispatch requires a bound pipeline and must be recorded outside a render pass");
            return;
        }
        if (!_boundPipelineState->isCompute())
        {
            VKM_DEBUG_ERROR("dispatch requires a compute pipeline to be bound");
            return;
        }
        if (groupCountX == 0 || groupCountY == 0 || groupCountZ == 0)
        {
            return; // Nothing to dispatch; Metal rejects a zero-sized threadgroup grid outright.
        }
        onDispatch(groupCountX, groupCountY, groupCountZ);
    }

    void VkmCommandBufferBase::barrierIndirectArgumentBuffer(VkmResourceHandle buffer)
    {
        if (!_isRecording || _isInRenderPass)
        {
            VKM_DEBUG_ERROR("barrierIndirectArgumentBuffer must be recorded while recording and outside a render pass");
            return;
        }
        onBarrierIndirectArgumentBuffer(buffer);
    }

    void VkmCommandBufferBase::barrierTextureForShaderRead(VkmResourceHandle texture)
    {
        if (!_isRecording || _isInRenderPass)
        {
            VKM_DEBUG_ERROR("barrierTextureForShaderRead must be recorded while recording and outside a render pass");
            return;
        }
        onBarrierTextureForShaderRead(texture);
    }

    void VkmCommandBufferBase::bindPerPassResources(VkmPerPassResourceTableBase* table)
    {
        if (!_isRecording || _boundPipelineState == nullptr)
        {
            VKM_DEBUG_ERROR("bindPerPassResources requires a bound pipeline");
            return;
        }
        if (table == nullptr)
        {
            VKM_DEBUG_ERROR("bindPerPassResources was given a null table");
            return;
        }
        if (table->getPipelineState() != _boundPipelineState)
        {
            // Set 2's layout comes from the pipeline's own declaration, so binding a table built
            // against a different pipeline would describe a different set. Caught here rather than
            // left to each backend, where it surfaces as a layout-compatibility validation error
            // far from its cause.
            VKM_DEBUG_ERROR("bindPerPassResources was given a table built for a different pipeline");
            return;
        }
        onBindPerPassResources(table);
    }

    void VkmCommandBufferBase::setPushConstants(const void* data, uint32_t size, uint32_t offset)
    {
        if (!_isRecording || _boundPipelineState == nullptr)
        {
            VKM_DEBUG_ERROR("setPushConstants requires a bound pipeline");
            return;
        }
        // A compute pipeline pushes from outside a render pass; a graphics one only inside.
        if (!_boundPipelineState->isCompute() && !_isInRenderPass)
        {
            VKM_DEBUG_ERROR("setPushConstants for a graphics pipeline requires an active render pass");
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

    void VkmCommandBufferBase::pushDebugGroup(const char* name)
    {
        if (_driver->getLaunchOptions().enableGpuCapture)
        {
            onPushDebugGroup(name != nullptr ? name : "");
        }
    }

    void VkmCommandBufferBase::popDebugGroup()
    {
        if (_driver->getLaunchOptions().enableGpuCapture)
        {
            onPopDebugGroup();
        }
    }

    void VkmCommandBufferBase::beginGpuZone(const uint32_t beginSlot, const uint32_t endSlot)
    {
        if (_isRecording == false)
        {
            VKM_DEBUG_ERROR("beginGpuZone must be called while the command buffer is recording");
            return;
        }
        onBeginGpuZone(beginSlot, endSlot);
    }

    bool VkmCommandBufferBase::endGpuZone()
    {
        if (_isRecording == false)
        {
            VKM_DEBUG_ERROR("endGpuZone must be called while the command buffer is recording");
            return false;
        }
        return onEndGpuZone();
    }

    void VkmCommandBufferBase::resolveGpuZones(const uint32_t firstSlot, const uint32_t count)
    {
        if (_isRecording == false)
        {
            VKM_DEBUG_ERROR("resolveGpuZones must be called while the command buffer is recording");
            return;
        }
        onResolveGpuZones(firstSlot, count);
    }
}