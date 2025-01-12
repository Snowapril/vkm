// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/backend_util.h>
#include <glm/vec2.hpp>

namespace vkm
{
    class VkmDriverBase;
    /*
    * @brief SwapChain base class
    */
    class SwapChain
    {
    public:
        SwapChain(VkmDriverBase* driver);
        virtual ~SwapChain();

        /*
        * @brief Initialize swapchain
        */
        bool initialize(uint32_t width, uint32_t height, const char* title);

        /*
        * @brief Destroy swapchain
        */
        void destroy();

        /*
        * @brief Resize swapchain
        * @details recreate swapchain with new extent
        */
        void resize(uint32_t width, uint32_t height);
        
        /*
        * @brief Acquire next back buffer image index
        * @details get next back buffer image index for rendering. wait for acquire in cpu time if necessary
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

    protected:
        virtual bool createSwapChain(const char* title) = 0;
        virtual void destroySwapChain() = 0;
        virtual VkmResourceHandle acquireNextImageInner() = 0;
        virtual void presentInner() = 0;

    protected:
        VkmDriverBase* _driver;
        glm::uvec2 _extent;
        uint8_t _backBufferCount;
    };
} // namespace vkm