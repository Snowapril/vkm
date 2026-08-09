// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/command_buffer.h>

#include <vector>

@protocol MTL4CommandBuffer;
@protocol MTL4RenderCommandEncoder;
@protocol MTL4ComputeCommandEncoder;

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

        /*
        * @brief Opens a render encoder, waiting first for the ordering the caller accumulated.
        * @details Metal 4 does no automatic hazard tracking, so an encoder that reads what an
        * earlier one wrote must open with a barrier or it samples memory still being written. The
        * masks come from the render graph's dependency analysis where it named one, and fall back
        * to the conservative whole-queue pair where it did not.
        * @param renderResourcePool Pool the attachments are resolved through.
        * @param frameBufferDesc Attachments and load/store actions for the pass.
        * @param acquireAfterQueueStages MTLStages to wait for; 0 means every stage.
        * @param acquireBeforeStages MTLStages in this encoder that wait; 0 means vertex and
        * fragment. Typed as uint64_t to keep MTLStages out of this header, as _boundPrimitiveType
        * does for MTLPrimitiveType.
        */
        void beginRenderPass(VkmRenderResourcePool* renderResourcePool, const VkmFrameBufferDescriptor& frameBufferDesc,
                             uint64_t acquireAfterQueueStages = 0, uint64_t acquireBeforeStages = 0);
        void beginComputePass();

        /*
        * @brief Publishes this encoder's writes to the queue stages that consume them, then ends it.
        * @details Metal 4 does no automatic hazard tracking, so an encoder that does not publish
        * leaves later encoders reading memory still being written. The mask comes from the render
        * graph's analysis where it named consumers for this subgraph, and falls back to every queue
        * stage where it named none.
        * @param releaseBeforeQueueStages MTLStages that must wait for this encoder; 0 means all of
        * them. Typed as uint64_t to keep MTLStages out of this header.
        */
        void commit(uint64_t releaseBeforeQueueStages = 0);
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
        * Ordering named while no encoder was open, carried to the next one that opens. Metal 4's
        * barriers are encoder-scoped and opening an encoder just to hold one is what causes
        * MTL4CommandQueueErrorTimeout, so a barrier recorded between passes waits here instead.
        */
        uint64_t _pendingAcquireAfterQueueStages = 0;
        uint64_t _pendingAcquireBeforeStages = 0;
        // Which queue stages consume what the upcoming subgraph writes, from the analysis. Held
        // across that subgraph's encoders rather than taken, because a subgraph may close several
        // and every one of them has to publish.
        uint64_t _pendingReleaseBeforeQueueStages = 0;

        // Hands the accumulated masks to the encoder now opening and clears them, so one barrier
        // discharges them rather than every later encoder repeating it.
        void takePendingAcquire(uint64_t* outAfterQueueStages, uint64_t* outBeforeStages)
        {
            *outAfterQueueStages = _pendingAcquireAfterQueueStages;
            *outBeforeStages = _pendingAcquireBeforeStages;
            _pendingAcquireAfterQueueStages = 0;
            _pendingAcquireBeforeStages = 0;
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
