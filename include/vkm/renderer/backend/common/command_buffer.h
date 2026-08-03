// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/render_pass.h>
#include <vkm/renderer/backend/common/driver_resource.h>
#include <vkm/renderer/backend/common/command_queue.h>

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
    * @details Record command to command stream that will be executed by druver
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
        *
        * @details Must be recorded inside a render pass. Coordinates are in pixels with the origin
        * at the attachment's top-left, the same convention on every backend -- Vulkan's +Y-down NDC
        * is already handled upstream by vkm-compiler's -fvk-invert-y, so nothing is compensated
        * here.
        *
        * beginRenderPass() already sets both to cover the whole framebuffer, so a pass that draws
        * to all of it never needs to call these. They exist for passes that pack several views into
        * one attachment -- rendering a probe's six cube faces into one atlas in a single pass
        * rather than six, which is the difference between one render pass per probe and six.
        *
        * Viewport and scissor are set together rather than separately: every current caller wants
        * them to agree, and letting them drift apart silently clips geometry the viewport says is
        * visible.
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
        * @brief Buffer-to-texture copy (e.g. staging -> sampled texture). Copies one
        * tightly-packed mip level of one array layer of an uncompressed color texture; a
        * cubemap face is just arrayLayer 0..5. Must be recorded while recording but outside
        * a render pass.
        * @details Leaves the destination shader-readable, so a texture is ready to sample as
        * soon as its copies have executed -- there is deliberately no separate "transition"
        * entry point, mirroring how copyTextureToBuffer manages layout on the caller's
        * behalf. Only backends reporting VkmDriverCapabilityFlags::TextureUpload implement
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
        * @brief GPU-driven draw: `argumentBuffer` holds `maxDrawCount` consecutive records of
        * `layout` starting at `argumentOffset`, and `countBuffer`/`countOffset` name a uint32 the
        * GPU wrote with how many of them are live.
        *
        * `layout` is what the record stride comes from (vkmGetIndirectArgumentStride), so a caller
        * declares the buffer's structure instead of every backend assuming it.
        *
        * Only Vulkan consumes the count buffer (vkCmdDrawIndirectCount). Metal 4 and WebGPU core
        * have neither a GPU-side draw count nor multi-draw, so they encode exactly `maxDrawCount`
        * indirect draws and rely on all-zero records being no-ops. The producing pass must
        * therefore ALWAYS (a) compact surviving draws to the front of the range and (b) leave the
        * rest zeroed -- otherwise the backends disagree about what gets drawn.
        *
        * Only VkmIndirectArgumentLayout::NonIndexed is accepted today: an indexed draw needs a
        * bound index buffer, and this engine deliberately has none (see draw() above). Indexed is
        * rejected here rather than in each backend so none of them carries a dead branch.
        *
        * Callers must have recorded barrierIndirectArgumentBuffer() for `argumentBuffer` after the
        * writing dispatch and before beginRenderPass().
        */
        void drawIndirectCount(VkmIndirectArgumentLayout layout,
                               VkmResourceHandle argumentBuffer, uint64_t argumentOffset,
                               VkmResourceHandle countBuffer, uint64_t countOffset,
                               uint32_t maxDrawCount);

        /*
        * @brief Compute dispatch of `groupCount*` threadgroups. Must be recorded outside a render
        * pass with a compute pipeline bound; the backing compute pass is opened by that bindPipeline()
        * and closed by unbindPipeline().
        *
        * The threadgroup size is whatever the bound pipeline's shader declared in
        * [numthreads(...)]: Vulkan and WebGPU take it from the shader module, and Metal reads it
        * from the pipeline object, where it arrived via VkmShaderCacheHeader::threadGroupSize.
        * Callers only supply the group counts.
        */
        void dispatch(uint32_t groupCountX, uint32_t groupCountY = 1, uint32_t groupCountZ = 1);

        /*
        * @brief Orders GPU-driven bookkeeping buffers: transfer and compute writes to `buffer`
        * recorded before this call become visible to compute reads and indirect-argument fetches
        * recorded after it. Must be recorded outside a render pass.
        */
        void barrierIndirectArgumentBuffer(VkmResourceHandle buffer);

        /*
        * @brief Makes earlier writes to `texture` visible to shaders that sample it, and leaves it
        * in whatever state that sampling needs.
        *
        * @details Every other texture operation manages its own destination state (see
        * copyBufferToTexture), which works because each one both writes and finishes the texture.
        * A texture written as a render-pass attachment and then *sampled* by a later pass is the
        * case that convention cannot express: the render pass leaves it in an attachment state,
        * and nothing afterwards knows to undo that. This is the entry point for that hand-off --
        * the G-buffer -> lighting-pass dependency in particular.
        *
        * Named for its destination like barrierIndirectArgumentBuffer, and equally coarse: it does
        * not take a source state, because Vulkan already tracks the texture's current layout and
        * the other two backends need no layout at all. Must be recorded outside a render pass, so
        * it sits between the pass that wrote the texture and the pass that reads it.
        *
        * Only Vulkan does real work here. Metal 4 brackets each compute pass with
        * barrierAfterQueueStages:/barrierAfterStages: on open and close, which already orders a
        * render pass's writes against a later pass's reads; WebGPU orders passes implicitly.
        */
        void barrierTextureForShaderRead(VkmResourceHandle texture);

        /*
        * @brief Binds `table` at whichever set it was built for -- set 2 (per-pass) or set 3
        * (per-draw) -- for subsequent draws or dispatches. The table carries its own set kind, so
        * there is nothing to pass alongside it.
        *
        * @details Must be recorded with a pipeline bound, and that pipeline's declaration for the
        * table's set must *equal* the one the table was built against. Equality rather than
        * pipeline identity is deliberate: a PSO's permutations are distinct pipeline objects that
        * share one declaration, and requiring a table per permutation would multiply per-material
        * tables by the permutation count for nothing. Sets 0 and 1 are engine-global and bound
        * unconditionally by bindPipeline(); these two are the ones needing per-PSO knowledge.
        *
        * A pipeline that declares either set must have it bound before it draws or dispatches: an
        * unbound declared set is a validation error, not a silently empty one.
        */
        void bindResourceTable(VkmResourceTableBase* table);

        void setPushConstants(const void* data, uint32_t size, uint32_t offset = 0);

        /*
        * @brief Opens a GPU-timed zone: the backend arranges for a timestamp to be written into
        * `beginSlot` at this point in the command stream, and remembers `endSlot` for the
        * matching endGpuZone(). Zones nest.
        *
        * Both slots are supplied up front because WebGPU has no encoder-level timestamp write at
        * all -- it can only express a pair as a pass descriptor's beginningOfPassWriteIndex /
        * endOfPassWriteIndex, which must be filled before the pass is opened.
        *
        * Slots are indices into the driver's timestamp pool (VkmDriverBase::
        * initializeGpuTimestampPool); VkmGpuProfiler owns their allocation. Both calls no-op on
        * a backend without timestamp support.
        */
        void beginGpuZone(uint32_t beginSlot, uint32_t endSlot);

        /*
        * @brief Closes the innermost open zone. Returns false when the backend could not place
        * this zone's timestamps anywhere, so the caller drops it instead of reporting a span
        * that was never measured.
        *
        * Only WebGPU ever returns false: its timestamps ride a pass descriptor, so a zone that
        * enclosed no render or compute pass (a transfer subgraph, or the outer zone wrapping a
        * whole submission) has nowhere to write. Vulkan and Metal always return true.
        */
        bool endGpuZone();

        /*
        * @brief Records whatever this backend needs in order for slots [firstSlot, firstSlot +
        * count) to be readable once this command buffer has completed. Must be called after the
        * outermost zone closed and before endCommandBuffer().
        *
        * A no-op on Vulkan and Metal, whose pools are read back directly from the CPU. WebGPU
        * has to encode a resolve into the same command buffer, because a query set can only be
        * read through a GPU-side resolve into a buffer.
        */
        void resolveGpuZones(uint32_t firstSlot, uint32_t count);

        inline const VkmGpuEventTimelineObject& getGpuEventTimelineObject() const { return _gpuEventTimelineObject; }

#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
        /*
        * @brief Records the last command of a subgraph: copies 4 bytes from a small constant-
        * `1` buffer (oneBuffer) into markerBuffer at offset (subgraphIndex * 4), so
        * VkmGpuCrashHandler::reportCrash() can later read that offset and know this subgraph's
        * GPU commands definitely executed by the time of a crash. Also remembers subGraphId
        * (see getRecordedSubGraphIds()) so the crash report can label which subgraph each
        * offset corresponds to. Callers (VkmRenderGraph::execute()) are expected to only call
        * this when VkmDriverBase::isGpuCrashDumpEnabled() is true.
        */
        void writeCompletionMarker(VkmResourceHandle markerBuffer, VkmResourceHandle oneBuffer, uint32_t subGraphId, uint32_t offset);

        /*
        * @brief Subgraph IDs (see VkmRenderSubGraph::getSubGraphId()) written into this command
        * buffer via writeCompletionMarker() since the last beginCommandBuffer(). Read by
        * VkmGpuCrashHandler::recordSubmission() to attach per-subgraph completion tracking to
        * this command buffer's breadcrumb entry.
        */
        inline const std::vector<uint32_t>& getRecordedSubGraphIds() const { return _recordedSubgraphIds; }
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS

        /*
        * @brief In-engine bookkeeping name (see getDebugName()), used by VkmGpuCrashHandler to
        * identify this command buffer's submission in a crash report's breadcrumb trail.
        * Additionally pushes a native label via onSetDebugName() when
        * VkmDriverBase::isDebugNamingEnabled() is true, mirroring how resource/queue debug
        * names work (see common/AGENTS.md "Debug Naming").
        */
        void setDebugName(const char* name);
        inline const std::string& getDebugName() const { return _debugName; }

        /*
        * @brief Open/close a named GPU debug group around the commands recorded until the
        * matching popDebugGroup(). Used to bracket each render subgraph so a GPU capture shows
        * named, collapsible scopes (e.g. "TrianglePass", "EngineImGuiOverlay"). Both are no-ops
        * unless VkmDriverBase::getLaunchOptions().enableGpuCapture is set -- gating both on the
        * same flag keeps push/pop balanced. The native call is delegated to onPushDebugGroup()/
        * onPopDebugGroup() in each backend.
        */
        void pushDebugGroup(const char* name);
        void popDebugGroup();

        /*
        * @brief Every pipeline bound via bindPipeline() since the last beginCommandBuffer(),
        * in bind order. Read by VkmRenderGraphCapture to attribute pipelines to subgraphs
        * (it snapshots the size before a subgraph's commit() and takes the delta after).
        * Pointers are only valid while the frame that recorded them is in flight.
        */
        inline const std::vector<VkmPipelineStateBase*>& getBoundPipelineHistory() const { return _boundPipelineHistory; }

    protected:
        // The pipeline bindPipeline() last published, or null after unbindPipeline(). Backends
        // that need pipeline state while recording a command read it here -- Metal's onDispatch()
        // takes the compute threadgroup size from it, since MTLComputePipelineState cannot be
        // asked what [numthreads(...)] its function declared.
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
        virtual void onBarrierIndirectArgumentBuffer(VkmResourceHandle buffer) = 0;
        virtual void onBarrierTextureForShaderRead(VkmResourceHandle texture) = 0;
        virtual void onBindResourceTable(VkmResourceTableBase* table) = 0;
        virtual void onSetPushConstants(const void* data, uint32_t size, uint32_t offset) = 0;
        virtual void onSetDebugName(const char* name) = 0;
        virtual void onPushDebugGroup(const char* name) = 0;
        virtual void onPopDebugGroup() = 0;

        /*
        * @brief Called by beginCommandBuffer() before anything is recorded. Backends discard
        * per-use state here for the same reason the base class clears its bind history: command
        * buffers are pooled and reused across frames.
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
        * @brief Called by endCommandBuffer() before it flips _isRecording off. No-op on
        * Vulkan/WebGPU (writeCompletionMarker() there records its copy immediately). Metal
        * uses this to flush every writeCompletionMarker() call made during this command
        * buffer's recording as a single batched compute pass instead of one short-lived
        * compute encoder per call -- opening/closing a compute encoder per subgraph was
        * observed to cause progressively worsening MTL4CommandQueueErrorTimeout and eventual
        * command-queue stalls under real interactive use.
        */
        virtual void onEndCommandBuffer() = 0;
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS

    protected:
        VkmDriverBase* _driver;
        VkmCommandQueueBase* _commandQueue;
        VkmCommandBufferPoolBase* _commandBufferPool;

        VkmGpuEventTimelineObject _gpuEventTimelineObject; // GPU event timeline object for synchronization

    protected:
        bool _isRecording; // Flag to indicate if the command buffer is currently recording commands
        bool _isInRenderPass; // Flag to indicate if the command buffer is currently in a render pass

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