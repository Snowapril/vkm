// Copyright (c) 2024 Snowapril

#pragma once

#include "vkm/base/common.h"
#include <glm/vec2.hpp>

namespace vkm
{

    /*
    * @brief SwapChain base class
    */
    class SwapChain
    {
    public:
        SwapChain();
        ~SwapChain();

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
        uint8_t acquireNextImageIndex();

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
        virtual uint8_t acquireNextImageIndexInner() = 0;
        virtual void presentInner() = 0;

    protected:
        glm::uvec2 _extent;
        uint8_t _backBufferCount;
    };
} // namespace vkm