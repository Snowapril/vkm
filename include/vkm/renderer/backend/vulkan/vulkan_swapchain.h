// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/swapchain.h>
#include <volk.h>

namespace vkm
{
    class VkmSwapChainVulkan final : public VkmSwapChainBase
    {
    public:
        VkmSwapChainVulkan(VkmDriverBase* driver);
        ~VkmSwapChainVulkan();

        virtual void setDebugName(const char* name) override final;

        inline VkFormat getImageFormat() const { return _imageFormat; }

        // Returns the pending acquire semaphore and clears it, so a submit consumes it exactly
        // once. Returns VK_NULL_HANDLE if no acquire is pending.
        VkSemaphore takePendingAcquireSemaphore();
        // Returns the render-finished semaphore for the currently acquired image and marks a
        // present-wait as pending. Must be signaled by the frame's submit.
        VkSemaphore takeRenderFinishedSemaphoreForSignal();

    protected:
        virtual bool createSwapChain(void* windowHandle) override final;
        virtual void destroySwapChain() override final;
        virtual VkmResourceHandle acquireNextImageInner() override final;
        virtual void presentInner() override final;

    private:
        VkSwapchainKHR  _swapChain{VK_NULL_HANDLE};
        VkFormat        _imageFormat;
        VkSurfaceKHR    _surface{VK_NULL_HANDLE};

        // Per frame-in-flight, indexed by _frameRingIndex (advanced once per successful
        // acquire). Reuse of slot i is safe because the engine's ensureCompleted() timeline
        // wait precedes the next acquire on that slot, so the previous acquire that used this
        // semaphore has already been consumed by a completed submit (guards the
        // acquire-semaphore-reuse pitfall / VUID-vkAcquireNextImageKHR-semaphore-01286).
        std::array<VkSemaphore, FRAME_COUNT>        _imageAvailableSemaphores{};
        // Per swapchain image, indexed by the acquired imageIndex; signaled by the frame's
        // submit and waited by vkQueuePresentKHR. Sized to MAX_BACK_BUFFER_COUNT because
        // the driver may create more images than the requested FRAME_BUFFER_COUNT.
        std::array<VkSemaphore, MAX_BACK_BUFFER_COUNT> _renderFinishedSemaphores{};
        uint32_t    _frameRingIndex{0};
        // Handoff state between acquire, submit and present.
        VkSemaphore _pendingAcquireSemaphore{VK_NULL_HANDLE};
        bool        _renderFinishedPending{false};
    };
} // namespace vkm