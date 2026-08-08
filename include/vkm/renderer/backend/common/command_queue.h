// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/driver_resource.h>
#include <memory>
#include <array>

namespace vkm
{
    class VkmCommandBufferBase;
    class VkmCommandQueueBase;
    class VkmDriverBase;
    class VkmSwapChainBase;

    static constexpr const uint32_t MAX_NUM_COMMAND_BUFFER_SUBMITS = 8;

    /*
    * @brief Command submit informations
    */
    struct CommandSubmitInfo
    {
        std::array<VkmCommandBufferBase*, MAX_NUM_COMMAND_BUFFER_SUBMITS> commandBuffers;
        uint32_t commandBufferCount;
        uint32_t frameIndex = 0;  // VkmRenderGraph::frameIndex() of this submission; 0 outside the render graph
        // When non-null, this submit waits on the swapchain's pending acquire semaphore and signals its
        // per-image render-finished semaphore.
        VkmSwapChainBase* presentSwapChain = nullptr;
    };

    /*
    * @brief Command buffer pool base class
    */
    class VkmCommandBufferPoolBase
    {
    public:
        VkmCommandBufferPoolBase(VkmDriverBase* driver, VkmCommandQueueBase* commandQueue);
        virtual ~VkmCommandBufferPoolBase();

    public:
        VkmCommandBufferBase* allocate();
        void release(VkmCommandBufferBase* commandBuffer);
    
    protected:
        virtual VkmCommandBufferBase* newCommandBuffer() = 0;
        virtual VKM_COMMAND_BUFFER_HANDLE getOrCreateRHICommandBuffer() = 0;

    protected:
        VkmDriverBase* _driver;
        VkmCommandQueueBase* _commandQueue;

        std::mutex _commandBufferMutex;
        std::vector<VkmCommandBufferBase*> _commandBuffers;

    };

    /*
    * @brief One point on a GPU event timeline, used to synchronize the CPU against the GPU.
    */
    struct VkmGpuEventTimelineObject
    {
        class VkmGpuEventTimelineBase* _gpuEventTimeline = nullptr;
        uint64_t _timelineValue = INVALID_VALUE64;
    };

    /*
    * @brief Base class for a monotonically increasing GPU event timeline.
    */
    class VkmGpuEventTimelineBase
    {
    public:
        VkmGpuEventTimelineBase(VkmDriverBase* driver) : _driver(driver) {}
        virtual ~VkmGpuEventTimelineBase() = default;

        inline uint64_t getLastAllocatedTimeline() const { return _lastAllocatedTimeline; }
        inline uint64_t getLastSubmittedTimeline() const { return _lastSubmittedTimeline; }
        inline uint64_t getLastCompletedCachedTimeline() const { return _lastCompletedCachedTimeline; }

        virtual uint64_t queryLastCompletedTimeline() = 0;
        virtual void waitIdle( const uint64_t timeoutMs ) = 0;

        VkmGpuEventTimelineObject allocateGpuEventTimelineObject()
        {
            VkmGpuEventTimelineObject gpuEventTimelineObject;
            gpuEventTimelineObject._gpuEventTimeline = this;
            gpuEventTimelineObject._timelineValue = ++_lastAllocatedTimeline;
            return gpuEventTimelineObject;
        }

        /*
        * @brief Records the highest timeline value a submission has asked the GPU to signal.
        * @details Every backend's submit() must call this. waitIdle() waits on this value rather
        * than on the last allocated one, so a command buffer that is begun but never submitted
        * cannot leave the queue waiting on a value nothing will signal.
        * @param timelineValue Value handed to the GPU to signal; ignored when lower than the current one.
        */
        inline void markTimelineSubmitted(const uint64_t timelineValue)
        {
            // Not std::max: this header must not pull in <algorithm>.
            if (timelineValue > _lastSubmittedTimeline)
            {
                _lastSubmittedTimeline = timelineValue;
            }
        }

    protected:
        VkmDriverBase* _driver;
        uint64_t _lastAllocatedTimeline = 0;
        uint64_t _lastSubmittedTimeline = 0;  // Highest value actually handed to the GPU to signal
        uint64_t _lastCompletedCachedTimeline = 0;  // Cached; may lag the GPU's real progress
    };

    /*
    * @brief Command queue base class
    */
    class VkmCommandQueueBase : public VkmDriverResourceBase
    {
    public:
        VkmCommandQueueBase(VkmDriverBase* driver);
        ~VkmCommandQueueBase();

        bool initialize(VkmCommandQueueType queueType, uint32_t queueIndex, const char* queueName);
        void waitIdle( const uint64_t timeoutMs );

        inline VkmCommandQueueType getQueueType() const { return _queueType; }
        inline const char* getQueueName() const { return _queueName; }
        inline uint32_t getQueueIndex() const { return _queueIndex; }
        inline VkmCommandBufferPoolBase* getCommandBufferPool() const { return _commandBufferPool.get(); }
        inline VkmGpuEventTimelineBase* getGpuEventTimeline() const { return _gpuEventTimeline.get(); }
        
    public:
        virtual VkmGpuEventTimelineObject submit(const CommandSubmitInfo& submitInfos) = 0;

    protected:
        virtual bool initializeInner() = 0;

    protected:
        // Note(snowapril) : at now, each command queue maintain exact one command buffer pool
        std::unique_ptr<VkmCommandBufferPoolBase> _commandBufferPool;
        std::unique_ptr<VkmGpuEventTimelineBase> _gpuEventTimeline;

        VkmDriverBase* _driver;
        VkmCommandQueueType _queueType;
        const char* _queueName;
        uint32_t _queueIndex;
    };
}
