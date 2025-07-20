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

    static constexpr const uint32_t MAX_NUM_COMMAND_BUFFER_SUBMITS = 8;

    /*
    * @brief Command submit informations
    */
    struct CommandSubmitInfo
    {
        std::array<VkmCommandBufferBase*, MAX_NUM_COMMAND_BUFFER_SUBMITS> commandBuffers;
        uint32_t commandBufferCount;
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

    protected:
        VkmDriverBase* _driver;
        uint64_t _lastAllocatedTimeline = 0; // This value is incremented each time a new timeline is allocated for command buffer submission
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
