// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_swapchain.h>
#include <vkm/renderer/backend/metal/metal_command_queue.h>
#include <vkm/renderer/backend/metal/metal_texture.h>
#include <QuartzCore/CAMetalLayer.h>
#include <Metal/MTLCommandBuffer.h>
#include <Metal/MTLCommandQueue.h>

namespace vkm
{
    VkmSwapChainMetal::VkmSwapChainMetal(VkmDriverBase* driver)
        : VkmSwapChainBase(driver)
    {

    }

    VkmSwapChainMetal::~VkmSwapChainMetal()
    {

    }

    void VkmSwapChainMetal::overrideCurrentDrawable(id<CAMetalDrawable> currentDrawable)
    {
        _currentDrawable = currentDrawable;
    }

    void VkmSwapChainMetal::setDebugName(const char* name)
    {
        for (std::unique_ptr<VkmTextureMetal>& textureMetal : _backBuffers)
        {
            if (VkmTextureMetal* texture = textureMetal.get())
                texture->setDebugName(name);
        }
    }

    bool VkmSwapChainMetal::createSwapChain(void* windowHandle)
    {
        VkmTextureInfo textureInfo;
        textureInfo._flags = VkmResourceCreateInfo::ExternalHandleOwner | VkmResourceCreateInfo::AllowShaderRead | VkmResourceCreateInfo::AllowPresent | VkmResourceCreateInfo::AllowColorAttachment;
        textureInfo._extent = glm::uvec3(_extent.x, _extent.y, 1);
        textureInfo._format = VkmFormat::R8G8B8A8_UNORM;
        textureInfo._numMipLevels = 1;
        textureInfo._numArrayLayers = 1;

        for (std::unique_ptr<VkmTextureMetal>& textureMetal : _backBuffers)
        {
            textureMetal.reset(new VkmTextureMetal(_driver));
            if (textureMetal->initialize(textureInfo) == false)
            {
                VKM_DEBUG_ERROR("Failed to create swap chain texture");
                destroySwapChain();
                return false;
            }
        }
        return true;
    }

    void VkmSwapChainMetal::destroySwapChain()
    {
        _currentDrawable = nil;
        for (std::unique_ptr<VkmTextureMetal>& textureMetal : _backBuffers)
            textureMetal.release();
    }

    VkmResourceHandle VkmSwapChainMetal::acquireNextImageInner()
    {
        _currentBackBufferIndex = (_currentBackBufferIndex + 1) % BACK_BUFFER_COUNT;
        if (_currentDrawable != nil)
        {
            id<MTLTexture> currentBackBuffer = [_currentDrawable texture];
            VkmTextureMetal* textureMetal = static_cast<VkmTextureMetal*>(_backBuffers[_currentBackBufferIndex].get());
            const bool result = textureMetal->overrideExternalHandle((void*)currentBackBuffer);
            VKM_ASSERT(result, "Failed to override external handle");
            return textureMetal->getHandle();
        }
        
        return VKM_INVALID_RESOURCE_HANDLE;
    }

    void VkmSwapChainMetal::presentInner()
    {
        if (_currentDrawable == nil)
            return;

        VkmCommandQueueMetal* commandQueueMetal = static_cast<VkmCommandQueueMetal*>(_presentQueue);
        id<MTLCommandQueue> mtlCommandQueue = commandQueueMetal->getMTLCommandQueue();

        id<MTLCommandBuffer> presentCommandBuffer = [mtlCommandQueue commandBuffer];
        [presentCommandBuffer setLabel:@"Present Command Buffer"];
        [presentCommandBuffer presentDrawable:_currentDrawable];
        [presentCommandBuffer commit];
    }
} // namespace vkm
