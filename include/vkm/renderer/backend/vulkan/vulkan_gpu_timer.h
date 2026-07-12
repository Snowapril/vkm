// Copyright (c) 2025 Snowapril

#pragma once

#include <volk.h>

namespace vkm
{
    class VkmDriverVulkan;

    // Measures GPU frame time via a small ring of VkQueryPool timestamp pairs (one begin/end
    // pair per in-flight frame, see FRAME_COUNT). Backs the engine debug overlay's "GPU"
    // stat. Vulkan-only -- Metal/WebGPU report 0 via VkmCommandBufferBase's default no-op
    // writeGpuTimestampBegin()/writeGpuTimestampEnd().
    class VkmGpuTimerVulkan
    {
    public:
        explicit VkmGpuTimerVulkan(VkmDriverVulkan* driver);
        ~VkmGpuTimerVulkan();

        bool initialize();
        void destroy();

        // Must be called at the very start/end of a frame's command buffer recording
        // (before any render pass begins / after the last one ends).
        void writeBeginTimestamp(VkCommandBuffer commandBuffer);
        void writeEndTimestamp(VkCommandBuffer commandBuffer);

        // Reads back an older, already-completed slot in the ring (not the one just
        // recorded) to avoid stalling on the current in-flight frame. Returns the
        // last-known-good value if the read isn't ready yet.
        double getLastGpuFrameTimeMs();

    private:
        VkmDriverVulkan* _driver;
        VkQueryPool _queryPool{VK_NULL_HANDLE};
        bool _supported{false};
        float _timestampPeriodNs{1.0f};
        uint32_t _currentSlot{0};
        uint32_t _framesRecorded{0}; // guards against reading an un-reset slot before the ring has cycled once
        double _lastFrameTimeMs{0.0};
    };
} // namespace vkm
