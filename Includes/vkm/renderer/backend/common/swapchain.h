// Copyright (c) 2024 Snowapril

#include "vkm/base/common.h"
#include <glm/vec2.hpp>

namespace vkm
{
    class SwapChain
    {
    public:
        SwapChain();
        ~SwapChain();

        bool initialize(uint32_t width, uint32_t height, const char* title);
        void destroy();

        void resize(uint32_t width, uint32_t height);
        
        uint8_t acquireNextImageIndex();
        void present();

    public:
        inline const glm::uvec2& getExtent() const
        {
            return _extent;
        }

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