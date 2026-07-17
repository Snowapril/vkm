
// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/command_queue.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/gpu_crash_handler.h>
#include <vkm/renderer/backend/common/render_graph_capture.h>

namespace vkm
{
    void VkmRenderGraphicsSubGraph::commit(VkmCommandBufferBase* commandBuffer)
    {
        commandBuffer->beginRenderPass(_frameBufferDesc);
        if (_renderCallback)
        {
            _renderCallback(commandBuffer);
        }
        commandBuffer->endRenderPass();
    }

    void VkmRenderComputeSubGraph::commit(VkmCommandBufferBase* commandBuffer)
    {
    }

    void VkmRenderTransferSubGraph::commit(VkmCommandBufferBase* commandBuffer)
    {
    }

    VkmRenderGraphicsSubGraph* VkmRenderGraph::beginGraphicsSubGraph(const VkmFrameBufferDescriptor& desc, const char* name)
    {
        // Create a new graphics subgraph with the provided framebuffer descriptor
        VkmRenderGraphicsSubGraph* subGraph = beginSubGraph<VkmRenderGraphicsSubGraph>(desc);
        if (name != nullptr)
        {
            subGraph->setName(name);
        }
        return subGraph;
    }

    VkmRenderComputeSubGraph* VkmRenderGraph::beginComputeSubGraph(const char* name)
    {
        VkmRenderComputeSubGraph* subGraph = beginSubGraph<VkmRenderComputeSubGraph>();
        if (name != nullptr)
        {
            subGraph->setName(name);
        }
        return subGraph;
    }

    VkmRenderTransferSubGraph* VkmRenderGraph::beginTransferSubGraph(const char* name)
    {
        VkmRenderTransferSubGraph* subGraph = beginSubGraph<VkmRenderTransferSubGraph>();
        if (name != nullptr)
        {
            subGraph->setName(name);
        }
        return subGraph;
    }

    void VkmRenderGraph::compile(const VkmRenderGraphCompileOptions& options)
    {
        // Compile the render graph by processing all subgraphs
        // for (const auto& subGraph : _subGraphs)
        // {
        // }
        // Reset the current subgraph ID for the next frame

        (void)options; // Suppress unused variable warning for now
    }

    void VkmRenderGraph::execute(const VkmRenderGraphCommitOptions& options)
    {
#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
        VkmGpuCrashHandler* gpuCrashHandler = _driver->getGpuCrashHandler();
        const bool gpuCrashDumpEnabled = _driver->isGpuCrashDumpEnabled();
        if (gpuCrashDumpEnabled)
        {
            // Must happen before this frame slot's subgraphs record any completion markers --
            // see VkmGpuCrashHandler::clearFrameMarkers() for why this itself needs to block.
            gpuCrashHandler->clearFrameMarkers(_frameIndex);
        }
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS

        VkmCommandQueueBase* commandQueue = _driver->getCommandQueue(VkmCommandQueueType::Graphics, 0);
        VkmCommandBufferPoolBase* commandBufferPool = commandQueue->getCommandBufferPool();

        VkmCommandBufferBase* commandBuffer = commandBufferPool->allocate();
        commandBuffer->beginCommandBuffer();
        commandBuffer->writeGpuTimestampBegin();

        if (options.capture != nullptr)
        {
            options.capture->beginCapture(_driver, _frameIndex);
        }

        for (auto& subGraph : _subGraphs)
        {
            // Execute each subgraph's commands
            if (subGraph->getSubGraphType() == VkmRenderSubGraphType::Graphics)
            {

            }
            const size_t pipelineHistoryBegin = commandBuffer->getBoundPipelineHistory().size();
            subGraph->commit(commandBuffer);

            if (options.capture != nullptr)
            {
                // Recorded after commit() returns, i.e. outside any render pass -- the same
                // guarantee the completion-marker writes below rely on.
                options.capture->recordSubGraph(_driver, commandBuffer, subGraph.get(), pipelineHistoryBegin);
            }

#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
            if (gpuCrashDumpEnabled)
            {
                const uint32_t subGraphId = subGraph->getSubGraphId();
                const uint32_t offset = gpuCrashHandler->getMarkerOffset(_frameIndex, subGraphId);
                if (offset != INVALID_VALUE32)
                {
                    commandBuffer->writeCompletionMarker(gpuCrashHandler->getOrCreateMarkerBuffer(), gpuCrashHandler->getOrCreateOneBuffer(), subGraphId, offset);
                }
            }
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS
        }

        commandBuffer->writeGpuTimestampEnd();
        commandBuffer->endCommandBuffer();

        CommandSubmitInfo submitInfo;
        submitInfo.commandBuffers[0] = commandBuffer;
        submitInfo.commandBufferCount = 1;
        submitInfo.frameIndex = _frameIndex;
        submitInfo.presentSwapChain = options.presentSwapChain;

        _lastSubmitInfo = commandQueue->submit(submitInfo);

        // TODO(snowapril) : execute() itself only ever submits to the Graphics queue (see
        // getCommandQueue(Graphics, 0) above) -- once it dispatches subgraphs to multiple queue
        // types, this loop will automatically record the correct per-instance usage regardless,
        // since recordUsage() is now keyed by timeline identity rather than queue type.
        VkmRenderResourcePool* resourcePool = _driver->getRenderResourcePool();
        for (auto& subGraph : _subGraphs)
        {
            for (VkmResourceHandle handle : subGraph->getReferencedResources())
            {
                VkmRenderResource* resource = resourcePool->getResource<VkmRenderResource>(handle);
                if (resource != nullptr)
                {
                    resource->recordUsage(_lastSubmitInfo);
                }
            }
        }

        // On WASM (no dedicated reclaimer thread) this is the only mechanism driving
        // deferred destruction; on Vulkan/Metal it's a harmless redundant sweep alongside
        // the background thread that already does the real work.
        _driver->getDeferredReclaimer()->pollOnce();
    }

    void VkmRenderGraph::reset()
    {
        // Reset the render graph state for the next frame
        _subGraphs.clear();
        _currentSubGraphId = 0;
    }

    void VkmRenderGraph::ensureCompleted()
    {
        // Ensure that the last submitted command buffer has completed execution
        if (_lastSubmitInfo._gpuEventTimeline)
        {
            _lastSubmitInfo._gpuEventTimeline->waitIdle(MAX_GPU_TIMEOUT_PER_FRAME); // Wait for 1000 ms
        }
    }
}
