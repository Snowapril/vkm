// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/split_barrier_tracker.h>

#include <unordered_map>
#include <unordered_set>
#include <vector>

@protocol MTL4CommandBuffer;
@protocol MTL4RenderCommandEncoder;
@protocol MTL4ComputeCommandEncoder;
@protocol MTLFence;

namespace vkm
{
    class VkmRenderResourcePool;
    enum class VkmCommandEncoderType : uint8_t
    {
        None = 0,
        Graphics,
        Compute,
    };

    class VkmCommandEncoderMetal
    {
    public:
        VkmCommandEncoderMetal() = default;
        ~VkmCommandEncoderMetal() = default;

        void setMTLCommandBuffer(id<MTL4CommandBuffer> mtlCommandBuffer)
        {
            _mtlCommandBuffer = mtlCommandBuffer;
        }

        void beginRenderPass(VkmRenderResourcePool* renderResourcePool, const VkmFrameBufferDescriptor& frameBufferDesc);
        void beginComputePass();
        void commit();

        /*
        * @brief Waits for the producers this encoder consumes, before the stages that consume them.
        * @details Metal 4 does no automatic hazard tracking, so an encoder reading what an earlier
        * one wrote must wait or it samples memory still being written. Must be called while an
        * encoder is open. The fences come from the render graph's analysis, one per producing
        * subgraph, so a consumer waits for its own producers rather than for everything recorded
        * before it at a pipeline stage.
        * @param fences Fences to wait on. Empty means this encoder consumes nothing in this graph.
        * @param beforeEncoderStages MTLStages in this encoder that wait. Typed as uint64_t to keep
        * MTLStages out of this header, as _boundPrimitiveType does for MTLPrimitiveType.
        */
        void waitForProducers(const std::vector<id<MTLFence>>& fences, uint64_t beforeEncoderStages);

        /*
        * @brief Publishes this encoder's writes through the fence its subgraph owns.
        * @details Must be called while an encoder is open, before it ends. A subgraph that opens
        * several encoders updates the same fence from each; a wait covers every update recorded
        * before it, so one fence per producing subgraph is enough.
        * @param fence Fence to update, or nil when nothing in this graph consumes this subgraph.
        * @param afterEncoderStages MTLStages whose completion the update waits for.
        * @return True when the update was encoded. False means no encoder was open or the fence was
        * nil, and the caller must not record the fence as produced -- a wait on it would not clear.
        */
        bool publishThroughFence(id<MTLFence> fence, uint64_t afterEncoderStages);

        void reset();

        inline id<MTL4RenderCommandEncoder> getActiveRenderCommandEncoder() const { return _mtlRenderCommandEncoder; }
        inline id<MTL4ComputeCommandEncoder> getActiveComputeCommandEncoder() const { return _mtlComputeCommandEncoder; }

    private:
        id<MTL4CommandBuffer> _mtlCommandBuffer;

        id<MTL4RenderCommandEncoder> _mtlRenderCommandEncoder = nullptr;
        id<MTL4ComputeCommandEncoder> _mtlComputeCommandEncoder = nullptr;

        VkmCommandEncoderType _currentEncoderType = VkmCommandEncoderType::None;
    };

    class VkmCommandBufferMetal : public VkmCommandBufferBase
    {
        friend class VkmCommandBufferPoolMetal;
    public:
        VkmCommandBufferMetal(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue, VkmCommandBufferPoolBase* commandBufferPool);
        ~VkmCommandBufferMetal();

        virtual void setRHICommandBuffer(VKM_COMMAND_BUFFER_HANDLE handle) override final;

        virtual void onBeginRenderPass(const VkmFrameBufferDescriptor& frameBufferDesc) override final;
        virtual void onEndRenderPass() override final;
        virtual void onSetViewportAndScissor(int32_t x, int32_t y, uint32_t width, uint32_t height) override final;
        virtual void onBindPipeline(VkmPipelineStateBase* pipelineState) override final;
        virtual void onUnbindPipeline() override final;
        virtual void onCopyBuffer(VkmResourceHandle srcBuffer, VkmResourceHandle dstBuffer, uint64_t srcOffset, uint64_t dstOffset, uint64_t size) override final;
        virtual void onCopyTextureToBuffer(VkmResourceHandle srcTexture, VkmResourceHandle dstBuffer, uint64_t dstOffset, uint32_t arrayLayer) override final;
        virtual void onCopyTexture(VkmResourceHandle srcTexture, VkmResourceHandle dstTexture) override final;
        virtual void onCopyBufferToTexture(VkmResourceHandle srcBuffer, VkmResourceHandle dstTexture, uint64_t srcOffset, uint32_t mipLevel, uint32_t arrayLayer) override final;
        virtual void onDraw(uint32_t vertexCount, uint32_t instanceCount, uint32_t firstVertex, uint32_t firstInstance) override final;
        virtual void onDrawIndirectCount(VkmIndirectArgumentLayout layout,
                                         VkmResourceHandle argumentBuffer, uint64_t argumentOffset,
                                         VkmResourceHandle countBuffer, uint64_t countOffset,
                                         uint32_t maxDrawCount) override final;
        virtual void onDispatch(uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) override final;
        virtual void onResourceBarrier(const VkmResourceBarrier* barriers, uint32_t count) override final;
        virtual void onBarrierAcquire(const VkmResourceBarrier* barriers, uint32_t count) override final;
        virtual void onBarrierRelease(const VkmResourceBarrier* barriers, uint32_t count) override final;
        virtual void onBuildAccelerationStructure(VkmResourceHandle accelerationStructure) override final;
        virtual void onBindResourceTable(VkmResourceTableBase* table) override final;
        virtual void onSetPushConstants(const void* data, uint32_t size, uint32_t offset) override final;
        virtual void onSetDebugName(const char* name) override final;
        virtual void onPushDebugGroup(const char* name) override final;
        virtual void onPopDebugGroup() override final;
        virtual void onBeginCommandBuffer() override final;
        virtual void onBeginGpuZone(uint32_t beginSlot, uint32_t endSlot) override final;
        virtual bool onEndGpuZone() override final;
#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
        virtual void onWriteCompletionMarker(VkmResourceHandle markerBuffer, VkmResourceHandle oneBuffer, uint32_t offset) override final;
        virtual void onEndCommandBuffer() override final;
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS

        inline id<MTL4CommandBuffer> getMTLCommandBuffer() const { return _mtlCommandBuffer; }
        inline id<MTL4RenderCommandEncoder> getActiveRenderCommandEncoder() const { return _commandEncoder.getActiveRenderCommandEncoder(); }
        inline id<MTL4ComputeCommandEncoder> getActiveComputeCommandEncoder() const { return _commandEncoder.getActiveComputeCommandEncoder(); }

    private:
        // Owned: setRHICommandBuffer() adopts the +1 reference handed over by
        // VkmCommandBufferPoolMetal::getOrCreateRHICommandBuffer().
        id<MTL4CommandBuffer> _mtlCommandBuffer = nullptr;
        VkmCommandEncoderMetal _commandEncoder;

        /*
        * Cross-encoder ordering is fences, not queue-scoped barriers: a fence pairs a consumer with
        * its own producers, where a queue-stage barrier waits for everything recorded before it at
        * those stages. Metal 4's fence calls are encoder-scoped, and opening an encoder just to
        * hold one is what causes MTL4CommandQueueErrorTimeout, so a dependency named between passes
        * waits here until the next encoder opens.
        */
        std::vector<id<MTLFence>> _pendingWaitFences;
        uint64_t _pendingWaitBeforeStages = 0;

        /*
        * The fence the running subgraph publishes through, and the stages whose completion its
        * updates wait for. Held rather than taken: a subgraph that records several copies closes
        * several encoders, and every one of them has to update the fence.
        */
        id<MTLFence> _currentSubGraphFence = nullptr;
        uint32_t _currentSubGraphId = INVALID_VALUE32;
        uint64_t _currentSubGraphReleaseStages = 0;

        /*
        * Subgraphs whose fence has actually been updated in this command buffer. A producer that
        * records no GPU work opens no encoder and so updates nothing -- VkmScene::recordUpdate
        * returns early on an empty scene -- and waiting on a fence no encoder ever signals hangs
        * the queue. A consumer's waits are resolved after its producers have run, so this is known
        * by then.
        */
        std::unordered_set<uint32_t> _publishedSubGraphs;

        // Reports the two mistakes that turn a split barrier into a hang.
        VkmSplitBarrierTracker _splitBarrierTracker;

        // One fence per producing subgraph, reused across frames.
        std::unordered_map<uint32_t, id<MTLFence>> _subGraphFences;
        id<MTLFence> fenceForSubGraph(uint32_t subGraphId);
        void releaseFences();

        /*
        * Orders one encoder against the encoders already closed before it, for a hazard the render
        * graph cannot name a producer for: a barrier recorded inside a subgraph callback, between
        * two of that subgraph's own encoders. Every encoder updates it on close, so a wait covers
        * everything recorded earlier in this command buffer -- the honest answer when the producer
        * is unnamed, and the only case that coarse. Cross-subgraph dependencies keep their own
        * per-producer fences. `_encoderBoundaryPublished` guards the first encoder, which has
        * nothing before it and would otherwise wait on a fence no encoder has updated.
        */
        id<MTLFence> _encoderBoundaryFence = nullptr;
        bool _encoderBoundaryPublished = false;
        id<MTLFence> fenceForEncoderBoundary();

        // Every encoder opens by waiting for the producers this subgraph consumes and closes by
        // publishing through its fence, so no call site has to remember either.
        void openComputeEncoder(uint64_t waitBeforeStages);
        void closeEncoder(uint64_t publishAfterStages);

        // Hands the accumulated waits to the encoder now opening and clears them, so one wait
        // discharges them rather than every later encoder repeating it.
        void takePendingWaits(std::vector<id<MTLFence>>* outFences, uint64_t* outBeforeStages)
        {
            *outFences = _pendingWaitFences;
            *outBeforeStages = _pendingWaitBeforeStages;
            _pendingWaitFences.clear();
            _pendingWaitBeforeStages = 0;
        }

        // Primitive type of the currently bound graphics pipeline, captured at
        // onBindPipeline() time for onDraw() (Metal passes it per draw call, not in the
        // pipeline state object). uint32_t to keep MTLPrimitiveType out of this header.
        uint32_t _boundPrimitiveType = 0;

        // End slots of the GPU zones currently open, innermost last (onBeginGpuZone is handed
        // both slots up front for WebGPU's sake; Metal only needs the end one at close time).
        std::vector<uint32_t> _openGpuZoneEndSlots;

#if defined(VKM_ENABLE_GPU_BREAD_CRUMBS)
        // onWriteCompletionMarker() queues here instead of opening and closing its own compute
        // encoder; onEndCommandBuffer() flushes all of them as one batched compute pass.
        struct PendingMarkerWrite
        {
            VkmResourceHandle markerBuffer;
            VkmResourceHandle oneBuffer;
            uint32_t offset;
        };
        std::vector<PendingMarkerWrite> _pendingMarkerWrites;
#endif // VKM_ENABLE_GPU_BREAD_CRUMBS
    };
}
