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
        // Which VkmRenderGraph frame slot this submission came from (VkmRenderGraph::frameIndex()).
        // Read by VkmGpuCrashHandler::recordSubmission() to locate the right marker-buffer slice
        // when reporting per-subgraph completion; unused/0 for submissions outside the render graph.
        uint32_t frameIndex = 0;
        // When non-null, this submit consumes the swapchain's pending acquire semaphore as a
        // wait and its per-image render-finished semaphore as a signal (backend present sync).
        VkmSwapChainBase* presentSwapChain = nullptr;
    };

    /*
    * @brief Command buffer ppol base class
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
    * @brief GPU event timeline object
    * @details This structure is used to represent a GPU event timeline object, which contains a
    * pointer to a GPU event timeline and a timeline value. It is used for synchronization purposes
    * between the CPU and GPU.
    */
    struct VkmGpuEventTimelineObject
    {
        class VkmGpuEventTimelineBase* _gpuEventTimeline = nullptr; // Pointer to the GPU event timeline
        uint64_t _timelineValue = INVALID_VALUE64; // The timeline value associated with this event
    };

    /*
    * @brief Base class for GPU event timeline
    * @details This class is used to manage GPU event timelines, which can be used for
    * synchronization and signaling between the CPU and GPU.
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
            gpuEventTimelineObject._timelineValue = ++_lastAllocatedTimeline; // Increment the timeline value for each allocation
            return gpuEventTimelineObject;
        }

        // Records the highest value a submission has asked the GPU to signal. Every backend's
        // submit() must call this; waitIdle() waits on it rather than on the last ALLOCATED value,
        // and the difference is not cosmetic. beginCommandBuffer() takes a timeline value too, so
        // a command buffer that is begun and then never submitted leaves _lastAllocatedTimeline
        // sitting on a value nothing will ever signal -- after which waiting for the queue to
        // drain either hangs (Metal) or times out and reports nothing (Vulkan ignores
        // vkWaitSemaphores' result), and the caller goes on to destroy resources the GPU is still
        // reading. That is not hypothetical: it presented as VUID-vkDestroy...-02442 followed by a
        // segmentation fault on the first CI run that had a real ray-tracing driver.
        inline void markTimelineSubmitted(const uint64_t timelineValue)
        {
            // Compared rather than std::max'd so this header does not pull in <algorithm>.
            if (timelineValue > _lastSubmittedTimeline)
            {
                _lastSubmittedTimeline = timelineValue;
            }
        }

    protected:
        VkmDriverBase* _driver;
        uint64_t _lastAllocatedTimeline = 0; // This value is incremented each time a new timeline is allocated for command buffer submission
        uint64_t _lastSubmittedTimeline = 0; // The highest value actually handed to the GPU to signal; see markTimelineSubmitted
        uint64_t _lastCompletedCachedTimeline = 0; // This value may not be updated immediately, it is used to cache the last completed timeline for performance reasons
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
