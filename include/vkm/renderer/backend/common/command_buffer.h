// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/render_pass.h>
#include <vkm/renderer/backend/common/driver_resource.h>
#include <vkm/renderer/backend/common/command_queue.h>
#include <vkm/renderer/backend/common/render_graph_barrier.h>

#include <string>
#include <vector>

namespace vkm
{
    class VkmDriverBase;
    class VkmCommandQueueBase;
    class VkmCommandBufferPoolBase;
    class VkmGpuEventTimelineBase;
    class VkmPipelineStateBase;
    class VkmResourceTableBase;
    struct VkmGpuEventTimelineObject;

    /*
    * @brief Command dispatcher
    * @details Records commands into a command stream that the driver later executes.
    */
    class VkmCommandBufferBase
    {
    public:
        VkmCommandBufferBase(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue, VkmCommandBufferPoolBase* commandBufferPool);
        ~VkmCommandBufferBase();

        virtual void setRHICommandBuffer(VKM_COMMAND_BUFFER_HANDLE handle) = 0;

        // Command buffer lifecycle related
        void beginCommandBuffer();
        void endCommandBuffer();

        // Render pass related
        void beginRenderPass(const VkmFrameBufferDescriptor& frameBufferDesc);
        void endRenderPass();

        /*
        * @brief Restricts subsequent draws to a sub-rectangle of the current render pass.
        * @details Must be recorded inside a render pass. beginRenderPass() already covers the whole
        * framebuffer, so this is only for passes that pack several views into one attachment, such
        * as a probe's six cube faces in a single pass. Viewport and scissor are set together
        * because letting them drift apart silently clips geometry the viewport says is visible.
        * @param x Left edge in pixels, origin at the attachment's top-left on every backend.
        * @param y Top edge in pixels.
        * @param width Rectangle width in pixels.
        * @param height Rectangle height in pixels.
        */
        void setViewportAndScissor(int32_t x, int32_t y, uint32_t width, uint32_t height);

        // Pipeline related
        void bindPipeline(VkmPipelineStateBase* pipelineState);
        void unbindPipeline();

        // Buffer-to-buffer copy (e.g. staging -> device-local). Must be recorded while
        // recording but outside a render pass.
        void copyBuffer(VkmResourceHandle srcBuffer, VkmResourceHandle dstBuffer, uint64_t srcOffset, uint64_t dstOffset, uint64_t size);

        // Texture-to-buffer copy (e.g. render target -> readback staging). Copies mip 0 of
        // one array layer of an uncompressed color texture, tightly packed. Must be recorded
        // while recording but outside a render pass.
        void copyTextureToBuffer(VkmResourceHandle srcTexture, VkmResourceHandle dstBuffer, uint64_t dstOffset = 0,
                                 uint32_t arrayLayer = 0);

        /*
        * @brief Buffer-to-texture copy (e.g. staging -> sampled texture).
        * @details Copies one tightly-packed mip level of one array layer of an uncompressed color
        * texture; a cubemap face is arrayLayer 0..5. Must be recorded while recording but outside a
        * render pass. Leaves the destination shader-readable, so there is no separate "transition"
        * entry point. Only backends reporting VkmDriverCapabilityFlags::TextureUpload implement
        * this; the others log an error and record nothing.
        */
        void copyBufferToTexture(VkmResourceHandle srcBuffer, VkmResourceHandle dstTexture,
                                 uint64_t srcOffset = 0, uint32_t mipLevel = 0, uint32_t arrayLayer = 0);

        // Full texture-to-texture copy of mip 0 / layer 0; src and dst must share format and
        // extent. Must be recorded while recording but outside a render pass. Only backends
        // reporting VkmDriverCapabilityFlags::TextureContentCapture implement this (render
        // graph capture snapshots); the others log an error and record nothing.
        void copyTexture(VkmResourceHandle srcTexture, VkmResourceHandle dstTexture);

        // Draw related -- indices, if any, are fetched manually in-shader via a bindless
        // index buffer rather than a bound VkBuffer, so there is no separate "indexed" draw.
        void draw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex = 0, uint32_t firstInstance = 0);

        /*
        * @brief GPU-driven draw over a buffer of indirect argument records.
        * @details Only Vulkan consumes the count buffer (vkCmdDrawIndirectCount). Metal 4 and
        * WebGPU core have neither a GPU-side draw count nor multi-draw, so they encode exactly
        * `maxDrawCount` indirect draws and rely on all-zero records being no-ops. The producing
        * pass must therefore ALWAYS compact surviving draws to the front of the range and leave the
        * rest zeroed, or the backends disagree about what gets drawn.
        * Callers must have recorded barrierIndirectArgumentBuffer() for `argumentBuffer` after the
        * writing dispatch and before beginRenderPass().
        * @param layout Record structure. The stride comes from it via vkmGetIndirectArgumentStride.
        * Only VkmIndirectArgumentLayout::NonIndexed is accepted -- an indexed draw needs a bound
        * index buffer, and this engine has none.
        * @param argumentBuffer Buffer holding the records.
        * @param argumentOffset Byte offset of the first record.
        * @param countBuffer Buffer holding a uint32 live-record count the GPU wrote.
        * @param countOffset Byte offset of that count.
        * @param maxDrawCount Number of consecutive records the range holds.
        */
        void drawIndirectCount(VkmIndirectArgumentLayout layout,
                               VkmResourceHandle argumentBuffer, uint64_t argumentOffset,
                               VkmResourceHandle countBuffer, uint64_t countOffset,
                               uint32_t maxDrawCount);

        /*
        * @brief Compute dispatch of threadgroups.
        * @details Must be recorded outside a render pass with a compute pipeline bound; the backing
        * compute pass is opened by that bindPipeline() and closed by unbindPipeline(). The
        * threadgroup size comes from the bound pipeline's [numthreads(...)] declaration, so callers
        * only supply the group counts.
        * @param groupCountX Threadgroups along X.
        * @param groupCountY Threadgroups along Y.
        * @param groupCountZ Threadgroups along Z.
        */
        void dispatch(uint32_t groupCountX, uint32_t groupCountY = 1, uint32_t groupCountZ = 1);

        /*
        * @brief Orders GPU-driven bookkeeping buffers.
        * @details Transfer and compute writes recorded before this call become visible to compute
        * reads and indirect-argument fetches recorded after it. Must be recorded outside a render
        * pass.
        * @param buffer Buffer whose writes become visible.
        */
        /*
        * @brief Orders the dependencies in `barriers` as one batched barrier: everything each
        * entry's source access did becomes visible to what its destination access is about to do.
        * Must be recorded while recording and outside a render pass.
        *
        * @details Batched rather than one call per resource because that is what the underlying
        * APIs want -- Vulkan takes arrays and issuing one vkCmdPipelineBarrier2 per texture at a
        * subgraph boundary is pure waste -- and because a whole boundary's worth of dependencies
        * is one decision, not N.
        *
        * "Outside a render pass" is not "outside an encoder": a compute encoder may well be open,
        * which is the case VkmScene::recordCull uses this for. Backends that have an encoder-scoped
        * barrier (Metal) put it on that encoder rather than opening one.
        *
        * A pointer and a count rather than a span, matching the rest of this header -- nothing in
        * the engine uses std::span and this header is included nearly everywhere.
        */
        void resourceBarrier(const VkmResourceBarrier* barriers, uint32_t count);
        void resourceBarrier(const std::vector<VkmResourceBarrier>& barriers);

        void barrierIndirectArgumentBuffer(VkmResourceHandle buffer);

        /*
        * @brief Makes earlier writes to a texture visible to shaders that sample it, and leaves it
        * in whatever state that sampling needs.
        * @details The hand-off for a texture written as a render-pass attachment and then sampled
        * by a later pass -- the G-buffer to lighting-pass dependency in particular. Every other
        * texture operation manages its own destination state, which works because each both writes
        * and finishes the texture; a render pass instead leaves an attachment state behind.
        * Takes no source state, because Vulkan already tracks the texture's current layout and the
        * other two backends need no layout at all. Must be recorded outside a render pass, so it
        * sits between the pass that wrote the texture and the pass that reads it.
        * Only Vulkan does real work here: Metal 4 brackets each compute pass with
        * barrierAfterQueueStages:/barrierAfterStages:, and WebGPU orders passes implicitly.
        * @param texture Texture whose writes become visible.
        */
        void barrierTextureForShaderRead(VkmResourceHandle texture);

        /*
        * @brief Rebuilds an acceleration structure in place, from the descriptions it was created
        * with and whatever updateInstances last wrote.
        * @details The entry point dynamic objects need. A structure created without `_allowUpdate`
        * is built once, synchronously, at creation; an updatable one keeps its scratch buffer and
        * can be rebuilt from a render graph pass every frame.
        * Rebuild, not refit: a top-level structure over N instances is cheap to rebuild outright
        * and stays optimal, whereas a refit degrades traversal quality as instances drift. Refit
        * belongs to deforming bottom-level geometry, which nothing here produces.
        * Must be recorded outside a render pass. The build reads the instance buffer, so this
        * frame's updateInstances must precede the submit that runs it.
        * @param accelerationStructure Structure to rebuild.
        */
        void buildAccelerationStructure(VkmResourceHandle accelerationStructure);

        /*
        * @brief Binds a resource table for subsequent draws or dispatches.
        * @details Must be recorded with a pipeline bound, and that pipeline's declaration for the
        * table's set must equal the one the table was built against. Equality rather than pipeline
        * identity, because a PSO's permutations are distinct pipeline objects sharing one
        * declaration. Sets 0 and 1 are engine-global and bound unconditionally by bindPipeline();
        * these two are the ones needing per-PSO knowledge. A pipeline that declares either set must
        * have it bound before it draws or dispatches -- an unbound declared set is a validation
        * error, not a silently empty one.
        * @param table Table to bind. It carries its own set kind, set 2 (per-pass) or set 3
        * (per-draw), so there is nothing to pass alongside it.
        */
        void bindResourceTable(VkmResourceTableBase* table);

        void setPushConstants(const void* data, uint32_t size, uint32_t offset = 0);

        /*
        * @brief Opens a GPU-timed zone. Zones nest.
        * @details Both slots are supplied up front because WebGPU has no encoder-level timestamp
        * write: it can only express a pair as a pass descriptor's beginningOfPassWriteIndex /
        * endOfPassWriteIndex, which must be filled before the pass is opened. A no-op on a backend
        * without timestamp support.
        * @param beginSlot Pool slot the backend writes a timestamp into at this point in the
        * command stream. Slots are indices into VkmDriverBase::initializeGpuTimestampPool's pool,
        * whose allocation VkmGpuProfiler owns.
        * @param endSlot Pool slot remembered for the matching endGpuZone().
        */
        void beginGpuZone(uint32_t beginSlot, uint32_t endSlot);

        /*
        * @brief Closes the innermost open zone.
        * @details Only WebGPU ever fails: its timestamps ride a pass descriptor, so a zone that
        * enclosed no render or compute pass -- a transfer subgraph, or the outer zone wrapping a
        * whole submission -- has nowhere to write. Vulkan and Metal always succeed.
        * @return False when the backend could not place this zone's timestamps anywhere, so the
        * caller drops the zone rather than reporting a span that was never measured.
        */
        bool endGpuZone();

        /*
        * @brief Records whatever this backend needs to make a slot range readable once this command
        * buffer has completed.
        * @details Must be called after the outermost zone closed and before endCommandBuffer(). A
        * no-op on Vulkan and Metal, whose pools are read back directly from the CPU; WebGPU encodes
        * a resolve into the same command buffer, a query set being readable only that way.
        * @param firstSlot First slot in the range.
        * @param count Number of slots.
        */
        void resolveGpuZones(uint32_t firstSlot, uint32_t count);

        inline const VkmGpuEventTimelineObject& getGpuEventTimelineObject() const { return _gpuEventTimelineObject; }

#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
        /*
        * @brief Records the last command of a subgraph, marking that its GPU commands ran.
        * @details Copies 4 bytes from a constant-`1` buffer into the marker buffer, so
        * VkmGpuCrashHandler::reportCrash() can read that offset and know this subgraph completed
        * before a crash. Callers (VkmRenderGraph::execute()) only call this when
        * VkmDriverBase::isGpuCrashDumpEnabled() is true.
        * @param markerBuffer Buffer the marker is written into.
        * @param oneBuffer Small buffer holding the constant 1 that is copied.
        * @param subGraphId Remembered so the crash report can label the offset; see
        * getRecordedSubGraphIds().
        * @param offset Byte offset in the marker buffer to write.
        */
        void writeCompletionMarker(VkmResourceHandle markerBuffer, VkmResourceHandle oneBuffer, uint32_t subGraphId, uint32_t offset);

        /*
        * @brief Subgraph IDs written into this command buffer via writeCompletionMarker() since the
        * last beginCommandBuffer().
        * @details Read by VkmGpuCrashHandler::recordSubmission() to attach per-subgraph completion
        * tracking to this command buffer's breadcrumb entry.
        */
        inline const std::vector<uint32_t>& getRecordedSubGraphIds() const { return _recordedSubgraphIds; }
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS

        /*
        * @brief Sets the in-engine bookkeeping name VkmGpuCrashHandler uses to identify this
        * command buffer's submission in a crash report's breadcrumb trail.
        * @details Also pushes a native label via onSetDebugName() when
        * VkmDriverBase::isDebugNamingEnabled() is true, as resource and queue debug names do.
        * @param name Name to record.
        */
        void setDebugName(const char* name);
        inline const std::string& getDebugName() const { return _debugName; }

        /*
        * @brief Opens and closes a named GPU debug group around the commands recorded between them.
        * @details Brackets each render subgraph so a GPU capture shows named, collapsible scopes
        * such as "TrianglePass". Both are no-ops unless
        * VkmDriverBase::getLaunchOptions().enableGpuCapture is set; gating both on the same flag
        * keeps push and pop balanced. The native call goes to each backend's onPushDebugGroup() /
        * onPopDebugGroup().
        * @param name Group name shown in the capture.
        */
        void pushDebugGroup(const char* name);
        void popDebugGroup();

        /*
        * @brief Every pipeline bound via bindPipeline() since the last beginCommandBuffer(), in
        * bind order.
        * @details Read by VkmRenderGraphCapture to attribute pipelines to subgraphs: it snapshots
        * the size before a subgraph's commit() and takes the delta after. The pointers are only
        * valid while the frame that recorded them is in flight.
        */
        inline const std::vector<VkmPipelineStateBase*>& getBoundPipelineHistory() const { return _boundPipelineHistory; }

    protected:
        /*
        * @brief The pipeline bindPipeline() last published, or null after unbindPipeline().
        * @details Backends that need pipeline state while recording read it here: Metal's
        * onDispatch() takes the compute threadgroup size from it, MTLComputePipelineState not being
        * able to report what [numthreads(...)] its function declared.
        */
        inline VkmPipelineStateBase* getBoundPipelineState() const { return _boundPipelineState; }

        virtual void onBeginRenderPass(const VkmFrameBufferDescriptor& frameBufferDesc) = 0;
        virtual void onEndRenderPass() = 0;
        virtual void onSetViewportAndScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) = 0;
        virtual void onBindPipeline(VkmPipelineStateBase* pipelineState) = 0;
        virtual void onUnbindPipeline() = 0;
        virtual void onCopyBuffer(VkmResourceHandle srcBuffer, VkmResourceHandle dstBuffer, uint64_t srcOffset, uint64_t dstOffset, uint64_t size) = 0;
        virtual void onCopyTextureToBuffer(VkmResourceHandle srcTexture, VkmResourceHandle dstBuffer, uint64_t dstOffset, uint32_t arrayLayer) = 0;
        virtual void onCopyBufferToTexture(VkmResourceHandle srcBuffer, VkmResourceHandle dstTexture, uint64_t srcOffset, uint32_t mipLevel, uint32_t arrayLayer) = 0;
        virtual void onCopyTexture(VkmResourceHandle srcTexture, VkmResourceHandle dstTexture) = 0;
        virtual void onDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) = 0;
        // `layout` is guaranteed to be NonIndexed today (drawIndirectCount rejects the rest), but
        // the record stride must still be taken from it via vkmGetIndirectArgumentStride rather
        // than assumed.
        virtual void onDrawIndirectCount(VkmIndirectArgumentLayout layout,
                                         VkmResourceHandle argumentBuffer, uint64_t argumentOffset,
                                         VkmResourceHandle countBuffer, uint64_t countOffset,
                                         uint32_t maxDrawCount) = 0;
        virtual void onDispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) = 0;
        // `barriers` is non-empty and every entry names a live resource -- the base class drops
        // the degenerate cases before this is reached.
        virtual void onResourceBarrier(const VkmResourceBarrier* barriers, uint32_t count) = 0;
        virtual void onBarrierIndirectArgumentBuffer(VkmResourceHandle buffer) = 0;
        virtual void onBuildAccelerationStructure(VkmResourceHandle accelerationStructure) = 0;
        virtual void onBarrierTextureForShaderRead(VkmResourceHandle texture) = 0;
        virtual void onBindResourceTable(VkmResourceTableBase* table) = 0;
        virtual void onSetPushConstants(const void* data, uint32_t size, uint32_t offset) = 0;
        virtual void onSetDebugName(const char* name) = 0;
        virtual void onPushDebugGroup(const char* name) = 0;
        virtual void onPopDebugGroup() = 0;

        /*
        * @brief Called by beginCommandBuffer() before anything is recorded.
        * @details Backends discard per-use state here, command buffers being pooled and reused
        * across frames.
        */
        virtual void onBeginCommandBuffer() {}

        // Empty defaults rather than pure virtuals: a backend without timestamp support reports
        // that through VkmDriverBase::initializeGpuTimestampPool() returning false, and then
        // never sees these called at all.
        virtual void onBeginGpuZone(uint32_t beginSlot, uint32_t endSlot) { (void)beginSlot; (void)endSlot; }
        virtual bool onEndGpuZone() { return false; }
        virtual void onResolveGpuZones(uint32_t firstSlot, uint32_t count) { (void)firstSlot; (void)count; }

#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
        virtual void onWriteCompletionMarker(VkmResourceHandle markerBuffer, VkmResourceHandle oneBuffer, uint32_t offset) = 0;

        /*
        * @brief Called by endCommandBuffer() before it flips _isRecording off.
        * @details A no-op on Vulkan and WebGPU, where writeCompletionMarker() records its copy
        * immediately. Metal flushes this command buffer's markers here as a single batched compute
        * pass; one short-lived compute encoder per subgraph stalls the command queue with
        * MTL4CommandQueueErrorTimeout.
        */
        virtual void onEndCommandBuffer() = 0;
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS

    protected:
        VkmDriverBase* _driver;
        VkmCommandQueueBase* _commandQueue;
        VkmCommandBufferPoolBase* _commandBufferPool;

        VkmGpuEventTimelineObject _gpuEventTimelineObject;

    protected:
        bool _isRecording;
        bool _isInRenderPass;

        VkmFrameBufferDescriptor _currentFrameBufferDesc;

    private:
        VkmPipelineStateBase* _boundPipelineState = nullptr;
        std::vector<VkmPipelineStateBase*> _boundPipelineHistory;
        std::string _debugName;
#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
        std::vector<uint32_t> _recordedSubgraphIds;
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS
    };
}