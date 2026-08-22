// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/platform/common/window.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/driver_resource.h>
#include <glm/vec2.hpp>

namespace vkm
{
    class VkmCommandQueueBase;
    class VkmDriverBase;
    /*
    * @brief VkmSwapChainBase base class
    */
    class VkmSwapChainBase : public VkmDriverResourceBase
    {
    public:
        VkmSwapChainBase(VkmDriverBase* driver);
        virtual ~VkmSwapChainBase();

        /*
        * @brief Set present queue for swapchain
        */
        inline void setPresentQueue(VkmCommandQueueBase* presentQueue)
        {
            _presentQueue = presentQueue;
        }

        /*
        * @brief Initialize swapchain
        */
        bool initialize(const VkmWindowInfo& windowInfo);

        /*
        * @brief Destroy swapchain
        */
        void destroy();

        /*
        * @brief Recreate the swapchain at a new extent.
        * @details A no-op at the same extent unless the swapchain is out of date. A zero extent, a
        * minimized window, tears the swapchain down and creates nothing; the next non-zero resize
        * brings it back. The caller must already have drained every submission that could still
        * reference the back buffers -- this destroys them immediately rather than through the
        * deferred reclaimer. VkmEngine::recreateSwapChain() is that caller.
        * @param width New width in pixels.
        * @param height New height in pixels.
        */
        void resize(uint32_t width, uint32_t height);

        /*
        * @brief True once acquire or present reported the swapchain no longer matches its surface:
        * Vulkan's VK_ERROR_OUT_OF_DATE_KHR, WebGPU's Outdated/Lost.
        * @details The engine recreates on the next frame, which covers resizes the window layer
        * never reported -- tiling window managers, display changes.
        */
        inline bool isOutOfDate() const
        {
            return _outOfDate;
        }

        /*
        * @brief Acquire the next back buffer for rendering.
        * @details Waits on the CPU for the acquire when necessary.
        * @return Handle of the acquired back buffer.
        */
        VkmResourceHandle acquireNextImage();

        /*
        * @brief Present prepared back buffer image
        */
        void present();

    public:
        /*
        * @brief Get swapchain extent
        */
        inline const glm::uvec2& getExtent() const
        {
            return _extent;
        }

        /*
        * @brief Get back buffer count
        */
        inline uint8_t getBackBufferCount() const
        {
            return _backBufferCount;
        }

        /*
        * @brief One of the swapchain's back buffers. The set is replaced wholesale by resize().
        * @param index Back buffer index.
        * @return The handle, or an invalid handle when `index` is past getBackBufferCount().
        */
        inline VkmResourceHandle getBackBuffer(uint8_t index) const
        {
            return (index < _backBufferCount) ? _backBuffers[index] : VKM_INVALID_RESOURCE_HANDLE;
        }

    protected:
        virtual bool createSwapChain(void* windowHandle) = 0;
        // Non-pure so the base destructor can call it safely before the derived vtable
        // is torn down. Derived classes override this to do backend-specific teardown.
        virtual void destroySwapChain() { destroySwapChainCommon(); }
        virtual VkmResourceHandle acquireNextImageInner() = 0;
        virtual void presentInner() = 0;

    protected:
        void destroySwapChainCommon();

    protected:
        // Entries beyond _backBufferCount stay VKM_INVALID_RESOURCE_HANDLE; they must never
        // reach releaseResource() with indeterminate contents.
        std::array<VkmResourceHandle, MAX_BACK_BUFFER_COUNT> _backBuffers = [] {
            std::array<VkmResourceHandle, MAX_BACK_BUFFER_COUNT> handles;
            handles.fill(VKM_INVALID_RESOURCE_HANDLE);
            return handles;
        }();

        VkmDriverBase* _driver;
        VkmCommandQueueBase* _presentQueue = nullptr;
        // The handle initialize() was given (GLFWwindow* / CAMetalLayer*), kept so resize() can
        // hand it back to createSwapChain().
        void* _windowHandle = nullptr;
        glm::uvec2 _extent;
        // Set by the backends from an acquire/present result; cleared by resize(). See isOutOfDate().
        bool _outOfDate = false;
        uint8_t _backBufferCount = 0;
        uint32_t _currentBackBufferIndex = INVALID_VALUE32;
    };
} // namespace vkm