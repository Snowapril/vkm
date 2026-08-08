
// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/aliased_memory_heap.h>
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
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/base/cpu_profiler.h>

#include <algorithm>

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

    namespace
    {
        // Answers "how many subresources does this handle have" out of the resource pool. Anything
        // that is not a texture has exactly one.
        class RenderResourcePoolSubresourceLookup : public VkmResourceSubresourceLookup
        {
        public:
            explicit RenderResourcePoolSubresourceLookup(VkmRenderResourcePool* pool) : _pool(pool) {}

            bool getSubresourceCounts(VkmResourceHandle handle, uint32_t* outMipLevels,
                                      uint32_t* outArrayLayers) const override
            {
                *outMipLevels = 1;
                *outArrayLayers = 1;
                if (handle.type != VkmResourceType::Texture)
                {
                    // Still has to exist: a stale handle in a declaration is worth reporting, and
                    // nothing else checks it.
                    return _pool->getResource<VkmRenderResource>(handle) != nullptr;
                }

                VkmTexture* texture = _pool->getResource<VkmTexture>(handle);
                if (texture == nullptr)
                {
                    return false;
                }
                *outMipLevels = std::max(1u, texture->getTextureInfo()._numMipLevels);
                *outArrayLayers = std::max(1u, texture->getTextureInfo()._numArrayLayers);
                return true;
            }

        private:
            VkmRenderResourcePool* _pool;
        };
    } // namespace

    void VkmRenderGraph::compile(const VkmRenderGraphCompileOptions& options)
    {
        // The analysis takes plain data rather than the subgraphs themselves, so it can be unit
        // tested without a driver -- see vkmBuildRenderGraphBarrierPlan.
        std::vector<VkmSubGraphAccess> accesses;
        std::vector<VkmSubGraphAccessView> views;
        views.reserve(_subGraphs.size());

        // Two passes: the first sizes and fills one flat access array so the views can point into
        // storage that will not move under them.
        std::vector<uint32_t> accessOffsets;
        accessOffsets.reserve(_subGraphs.size() + 1);
        for (const auto& subGraph : _subGraphs)
        {
            accessOffsets.push_back(static_cast<uint32_t>(accesses.size()));
            for (const VkmResourceAccessDeclaration& declaration : subGraph->getReferencedResources())
            {
                accesses.push_back(
                    VkmSubGraphAccess{ declaration._handle, declaration._access, declaration._range });
            }

            /*
            * A graphics subgraph's attachments are derived from its frame buffer descriptor rather
            * than declared. They are the one thing the graph can read directly, and requiring them
            * to be declared as well would mean every render target were written down twice and the
            * analysis would silently lose a writer whenever one of the two drifted.
            *
            * Appended after the explicit declarations, so a caller that also declares its target
            * (agreeing with what is derived here) costs a duplicate the analysis folds away rather
            * than a conflict.
            */
            if (subGraph->getSubGraphType() != VkmRenderSubGraphType::Graphics)
            {
                continue;
            }
            const VkmFrameBufferDescriptor& frameBufferDesc =
                static_cast<VkmRenderGraphicsSubGraph*>(subGraph.get())->getFrameBufferDescriptor();
            for (uint32_t i = 0; i < frameBufferDesc._renderPass._colorAttachmentCount; ++i)
            {
                accesses.push_back(VkmSubGraphAccess{ frameBufferDesc._colorAttachments[i],
                                                      VkmResourceAccess::ColorAttachmentWrite, {} });
            }
            if (frameBufferDesc._depthStencilAttachment.has_value() &&
                frameBufferDesc._renderPass._depthStencilAttachment.has_value())
            {
                accesses.push_back(VkmSubGraphAccess{ frameBufferDesc._depthStencilAttachment.value(),
                                                      VkmResourceAccess::DepthStencilAttachmentWrite, {} });
            }
        }
        accessOffsets.push_back(static_cast<uint32_t>(accesses.size()));

        for (size_t i = 0; i < _subGraphs.size(); ++i)
        {
            VkmSubGraphAccessView view{};
            view._subGraphId = _subGraphs[i]->getSubGraphId();
            view._scope = vkmPipelineScopeOfSubGraph(_subGraphs[i]->getSubGraphType());
            view._accesses = accesses.data() + accessOffsets[i];
            view._accessCount = accessOffsets[i + 1] - accessOffsets[i];
            views.push_back(view);
        }

        RenderResourcePoolSubresourceLookup lookup(_driver->getRenderResourcePool());
        _barrierPlan = vkmBuildRenderGraphBarrierPlan(views.data(), static_cast<uint32_t>(views.size()),
                                                      lookup, options.optimize, options.validate);

        for (const std::string& error : _barrierPlan._validationErrors)
        {
            VKM_DEBUG_ERROR(error.c_str());
        }

        // Fills in what the render graph capture and its ImGui inspector have always displayed and
        // never had data for -- nothing called addDependentSubGraphId before this.
        for (size_t i = 0; i < _subGraphs.size(); ++i)
        {
            _subGraphs[i]->setDependentSubGraphIds(_barrierPlan._dependencies[i]);
        }

        compileAliasedResources();
    }

    /*
    * Decides where every Aliasable texture's bytes live, and where the discard-and-fence has to
    * happen so two textures sharing bytes can never coexist on the GPU.
    *
    * This runs after every subgraph is declared and before any command is recorded, which is the
    * only point where both facts are available. Subgraphs commit in declaration order into one
    * command buffer, so a lifetime is just the closed interval of subgraph indices that declared
    * the resource, and "these two never coexist" is interval non-intersection.
    */
    void VkmRenderGraph::compileAliasedResources()
    {
        VkmRenderResourcePool* resourcePool = _driver->getRenderResourcePool();
        // Nothing aliasable has ever been created: one relaxed load and every existing graph is
        // untouched by this pass.
        if (resourcePool == nullptr || !resourcePool->hasAliasableTextures())
        {
            return;
        }

        VkmAliasedMemoryHeap* heap = _driver->getAliasedMemoryHeap();
        if (heap == nullptr)
        {
            return;
        }

        const uint32_t subGraphCount = (uint32_t)_subGraphs.size();
        std::vector<VkmAliasLifetime> lifetimes;

        const auto mergeLifetime = [&lifetimes](VkmResourceHandle handle, uint32_t subGraphIndex) {
            const auto it = std::find_if(lifetimes.begin(), lifetimes.end(),
                                         [handle](const VkmAliasLifetime& l) { return l._handle == handle; });
            if (it == lifetimes.end())
            {
                lifetimes.push_back(VkmAliasLifetime{handle, subGraphIndex, subGraphIndex});
                return;
            }
            it->_first = std::min(it->_first, subGraphIndex);
            it->_last = std::max(it->_last, subGraphIndex);
        };

        const auto isAliasableTexture = [resourcePool](VkmResourceHandle handle) {
            VkmTexture* texture = resourcePool->getResource<VkmTexture>(handle);
            return texture != nullptr && texture->isAliasable();
        };

        for (uint32_t subGraphIndex = 0; subGraphIndex < subGraphCount; ++subGraphIndex)
        {
            VkmRenderSubGraph* subGraph = _subGraphs[subGraphIndex].get();
            const std::vector<VkmResourceHandle>& declared = subGraph->getAliasedResources();
            for (VkmResourceHandle handle : declared)
            {
                if (!isAliasableTexture(handle))
                {
                    VKM_DEBUG_WARN(fmt::format("Subgraph '{}' declared resource {} as aliased, but it is not an "
                                               "Aliasable texture; the declaration is ignored",
                                               subGraph->getName(), handle.id).c_str());
                    continue;
                }
                mergeLifetime(handle, subGraphIndex);
            }

            // Attachments are the only use the graph can see for itself. An aliasable texture
            // attached without being declared is a caller bug, but the lifetime is widened rather
            // than trusted: an over-wide lifetime costs memory, an under-wide one corrupts pixels.
            if (subGraph->getSubGraphType() != VkmRenderSubGraphType::Graphics)
            {
                continue;
            }
            VkmRenderGraphicsSubGraph* graphicsSubGraph = static_cast<VkmRenderGraphicsSubGraph*>(subGraph);
            const VkmFrameBufferDescriptor& frameBufferDesc = graphicsSubGraph->getFrameBufferDescriptor();

            const auto checkAttachment = [&](VkmResourceHandle handle, const char* slot) {
                if (!handle.isValid() || !isAliasableTexture(handle))
                {
                    return;
                }
                if (std::find(declared.begin(), declared.end(), handle) == declared.end())
                {
                    VKM_DEBUG_ERROR(fmt::format("Subgraph '{}' attaches aliasable texture {} as its {} without "
                                                "declaring it via addAliasedResource; its lifetime is widened to "
                                                "cover this subgraph", subGraph->getName(), handle.id, slot).c_str());
                }
                mergeLifetime(handle, subGraphIndex);
            };

            for (uint32_t i = 0; i < frameBufferDesc._renderPass._colorAttachmentCount; ++i)
            {
                checkAttachment(frameBufferDesc._colorAttachments[i], "color attachment");
            }
            if (frameBufferDesc._depthStencilAttachment.has_value())
            {
                checkAttachment(frameBufferDesc._depthStencilAttachment.value(), "depth/stencil attachment");
            }
        }

        // Place anything that has no memory yet; everything else keeps the placement it was
        // given, and only has to still be justified by this frame's lifetimes.
        std::vector<VkmResourceHandle> newlyPlaced;
        heap->place(lifetimes, &newlyPlaced);
        for (VkmResourceHandle handle : newlyPlaced)
        {
            VkmTexture* texture = resourcePool->getResource<VkmTexture>(handle);
            const std::optional<VkmAliasPlacement> placement = heap->getPlacement(handle);
            if (texture != nullptr && placement.has_value())
            {
                texture->finalizeAliasPlacement(*placement);
            }
        }

        std::string validationError;
        if (!heap->validate(lifetimes, &validationError))
        {
            // Placement is frozen, so this cannot be repaired -- say exactly what changed and let
            // the frame run rather than aborting, matching how the transient coercion degrades.
            VKM_DEBUG_ERROR(fmt::format("Render graph aliasing conflict: {}", validationError).c_str());
        }

        _aliasAcquisitions.assign(subGraphCount, {});
        for (const VkmAliasLifetime& lifetime : lifetimes)
        {
            // A texture whose bytes nobody else shares needs no discard and no fence.
            if (!heap->isAliased(lifetime._handle))
            {
                continue;
            }
            _aliasAcquisitions[lifetime._first].push_back(lifetime._handle);

            // Its bytes belonged to another texture a moment ago, so there is nothing to load.
            VkmRenderSubGraph* firstSubGraph = _subGraphs[lifetime._first].get();
            if (firstSubGraph->getSubGraphType() != VkmRenderSubGraphType::Graphics)
            {
                continue;
            }
            VkmFrameBufferDescriptor& frameBufferDesc =
                static_cast<VkmRenderGraphicsSubGraph*>(firstSubGraph)->getFrameBufferDescriptorForCompile();
            const auto coerceLoad = [&](VkmResourceHandle handle, VkmLoadAction& loadAction, const char* slot) {
                if (handle == lifetime._handle && loadAction == VkmLoadAction::Load)
                {
                    VKM_DEBUG_ERROR(fmt::format("Aliased texture {} is loaded on its first use as the {} of '{}'; "
                                                "its previous contents belong to another texture, so the load is "
                                                "forced to DontCare", handle.id, slot,
                                                firstSubGraph->getName()).c_str());
                    loadAction = VkmLoadAction::DontCare;
                }
            };
            for (uint32_t i = 0; i < frameBufferDesc._renderPass._colorAttachmentCount; ++i)
            {
                coerceLoad(frameBufferDesc._colorAttachments[i],
                           frameBufferDesc._renderPass._colorAttachments[i]._loadAction, "color attachment");
            }
            if (frameBufferDesc._depthStencilAttachment.has_value() &&
                frameBufferDesc._renderPass._depthStencilAttachment.has_value())
            {
                coerceLoad(frameBufferDesc._depthStencilAttachment.value(),
                           frameBufferDesc._renderPass._depthStencilAttachment->_loadAction,
                           "depth/stencil attachment");
            }
        }
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

        for (size_t subGraphIndex = 0; subGraphIndex < _subGraphs.size(); ++subGraphIndex)
        {
            auto& subGraph = _subGraphs[subGraphIndex];
            const size_t pipelineHistoryBegin = commandBuffer->getBoundPipelineHistory().size();

            // Discard the bytes the previous alias left here and fence this frame's first use
            // against every read of that alias, including the previous frame's. Before the plan's
            // own acquire, so the layout tracker it reads is already where the discard put it.
            // Correctly outside a render pass: commit() is what opens one.
            if (subGraphIndex < _aliasAcquisitions.size())
            {
                for (VkmResourceHandle handle : _aliasAcquisitions[subGraphIndex])
                {
                    commandBuffer->acquireAliasedTexture(handle);
                }
            }

            /*
            * Both halves before commit(), never one on each side. Metal has to record its release
            * inside the producing encoder, and that encoder is closed by the time commit() returns
            * -- so declaring what this subgraph publishes has to happen while the encoder it will
            * open is still ahead of us. Each backend places them where its own API needs them.
            *
            * Empty for a subgraph nothing depends on, and the call returns immediately for that.
            */
            if (subGraphIndex < _barrierPlan._acquire.size())
            {
                const std::vector<VkmResourceBarrier>& acquire = _barrierPlan._acquire[subGraphIndex];
                commandBuffer->barrierAcquire(acquire.data(), static_cast<uint32_t>(acquire.size()));
                const std::vector<VkmResourceBarrier>& release = _barrierPlan._release[subGraphIndex];
                commandBuffer->barrierRelease(release.data(), static_cast<uint32_t>(release.size()));
            }

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
            for (const VkmResourceAccessDeclaration& declaration : subGraph->getReferencedResources())
            {
                VkmRenderResource* resource = resourcePool->getResource<VkmRenderResource>(declaration._handle);
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
        // The plan indexes the subgraphs that just went away, so it cannot outlive them.
        _barrierPlan = VkmRenderGraphBarrierPlan{};
        // Indexed by subgraph, so it cannot outlive the subgraphs it indexes.
        _aliasAcquisitions.clear();
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
