
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
#include <vkm/renderer/backend/common/gpu_profiler.h>
#include <vkm/renderer/backend/common/render_graph_capture.h>
#include <vkm/base/cpu_profiler.h>

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
        // No render pass is opened: a compute pass rides on the pipeline bind/unbind inside the
        // callback (see VkmRenderComputeSubGraph::setComputeCallback).
        if (_computeCallback)
        {
            _computeCallback(commandBuffer);
        }
    }

    void VkmRenderTransferSubGraph::commit(VkmCommandBufferBase* commandBuffer)
    {
        // No render pass is opened: the point of a transfer subgraph is to record the copies a
        // render pass forbids.
        if (_transferCallback)
        {
            _transferCallback(commandBuffer);
        }
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
        VKM_PROFILE_SCOPE("RenderGraph::execute");
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
        // Must follow allocate(), which is what binds the native handle every onSetDebugName
        // override labels. Without a name the crash handler falls back to "<queueName>#<index>"
        // (see VkmGpuCrashHandler's breadcrumb assembly), which cannot tell this submit apart
        // from the driver's own upload submits on the same queue; the frame slot is exactly the
        // information that fallback lacks. Per-subgraph identity is already carried by the debug
        // group below and by each subgraph's completion marker.
        commandBuffer->setDebugName(("RenderGraph.Frame" + std::to_string(_frameIndex)).c_str());
        commandBuffer->beginCommandBuffer();

        // One zone for the whole submission plus one per subgraph. The count is exact because
        // the profiler resets precisely the timestamp slots it is told will be written -- see
        // VkmGpuProfiler::beginSubmission.
        VkmGpuProfiler* gpuProfiler = _driver->getGpuProfiler();
        const uint32_t profileSubmission = gpuProfiler->beginSubmission(
            commandQueue, commandBuffer, 1 + static_cast<uint32_t>(_subGraphs.size()));
        // Every zone call below already no-ops on an invalid submission; this is checked anyway so
        // that a backend without timestamp support does not pay internName()'s mutex and string
        // allocation once per subgraph per frame.
        const bool gpuProfiling = profileSubmission != VkmGpuProfiler::kInvalidSubmission;
        gpuProfiler->beginZone(commandBuffer, profileSubmission, "Frame", INVALID_VALUE32, /*depth*/ 0);

        if (options.capture != nullptr)
        {
            options.capture->beginCapture(_driver, _frameIndex);
        }

        for (auto& subGraph : _subGraphs)
        {
            const size_t pipelineHistoryBegin = commandBuffer->getBoundPipelineHistory().size();

            // Bracket each subgraph in a named GPU debug group so a capture shows it as a
            // collapsible scope (e.g. "TrianglePass", "EngineImGuiOverlay"). Self-gated on
            // enableGpuCapture; a no-op otherwise.
            commandBuffer->pushDebugGroup(subGraph->getName().c_str());
            if (gpuProfiling)
            {
                // Interned for the same reason VKM_PROFILE_SCOPE_DYNAMIC below does it: subgraph
                // names come from a small fixed set, and a GPU zone outlives the frame that
                // recorded it by several more (it is only read once the GPU has finished).
                gpuProfiler->beginZone(commandBuffer, profileSubmission,
                                       VkmCpuProfiler::internName(subGraph->getName()),
                                       subGraph->getSubGraphId(), /*depth*/ 1);
            }
            {
                // Named after the subgraph, so the flame chart reads the same way the GPU debug
                // group above does. Subgraph names come from a small fixed set, which is what
                // makes interning them safe (see VkmCpuProfiler::internName).
                VKM_PROFILE_SCOPE_DYNAMIC(subGraph->getName());
                subGraph->commit(commandBuffer);
            }
            gpuProfiler->endZone(commandBuffer, profileSubmission);
            commandBuffer->popDebugGroup();

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

        // Closing the outermost zone is also what lets the backend record its resolve into this
        // same command buffer, so it has to happen before endCommandBuffer().
        gpuProfiler->endZone(commandBuffer, profileSubmission);
        commandBuffer->endCommandBuffer();

        CommandSubmitInfo submitInfo;
        submitInfo.commandBuffers[0] = commandBuffer;
        submitInfo.commandBufferCount = 1;
        submitInfo.frameIndex = _frameIndex;
        submitInfo.presentSwapChain = options.presentSwapChain;

        {
            VKM_PROFILE_SCOPE("CommandQueue::submit");
            _lastSubmitInfo = commandQueue->submit(submitInfo);
        }

        // The profiler will not read this submission's timestamps until this timeline completes.
        gpuProfiler->endSubmission(profileSubmission, _lastSubmitInfo);

        // Hand the command buffer back for reuse. submit() and recordSubmission() have already
        // copied everything they keep, and beginCommandBuffer() resets the per-use state, so the
        // instance is free the moment it is submitted -- without this, allocate() constructs a
        // new command buffer (and a new RHI command buffer with it) every single frame.
        commandBufferPool->release(commandBuffer);

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
