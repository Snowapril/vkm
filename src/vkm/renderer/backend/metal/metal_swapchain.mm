// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_swapchain.h>
#include <vkm/renderer/backend/metal/metal_command_queue.h>
#include <vkm/renderer/backend/metal/metal_render_resource_pool.h>
#include <vkm/renderer/backend/metal/metal_texture.h>
#include <vkm/renderer/backend/metal/metal_util.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <QuartzCore/CAMetalLayer.h>
#import <Metal/MTL4CommandQueue.h>
#import <Metal/MTLResidencySet.h>
#import <CoreGraphics/CoreGraphics.h>

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

    VkmFormat VkmSwapChainMetal::getBackBufferFormat() const
    {
        return _driver->getSwapChainColorFormat();
    }

    void VkmSwapChainMetal::setDebugName(const char* name)
    {
        VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();
        for (const VkmResourceHandle& handle : _backBuffers)
        {
            if (handle.isValid())
            {
                VkmTexture* texture = renderResourcePool->getResource<VkmTexture>(handle);
                texture->setDebugName(name);
            }
        }
    }

    bool VkmSwapChainMetal::createSwapChain(void* windowHandle)
    {
        // The engine picks the swapchain color format once at driver init (non-HDR vs HDR, gated
        // on display EDR support); apply it to the layer here so the layer, the backbuffer texture,
        // and any pipeline that resolved a "swapchain" color format all agree on one format.
        const VkmFormat colorFormat = _driver->getSwapChainColorFormat();
        const bool isHdr = (colorFormat == VkmFormat::R16G16B16A16_SFLOAT);

        CAMetalLayer* metalLayer = (__bridge CAMetalLayer*)windowHandle;
        metalLayer.pixelFormat = getMTLPixelFormat(colorFormat);
        metalLayer.wantsExtendedDynamicRangeContent = isHdr ? YES : NO;
        CGColorSpaceRef colorSpace = CGColorSpaceCreateWithName(
            isHdr ? kCGColorSpaceExtendedLinearDisplayP3 : kCGColorSpaceDisplayP3);
        metalLayer.colorspace = colorSpace;
        CGColorSpaceRelease(colorSpace); // the layer retains it

        VkmTextureInfo textureInfo;
        textureInfo._flags = VkmResourceCreateInfo::ExternalHandleOwner | VkmResourceCreateInfo::AllowShaderRead | VkmResourceCreateInfo::AllowPresent | VkmResourceCreateInfo::AllowColorAttachment;
        textureInfo._extent = glm::uvec3(_extent.x, _extent.y, 1);
        textureInfo._format = colorFormat;
        textureInfo._numMipLevels = 1;
        textureInfo._numArrayLayers = 1;

        for (VkmResourceHandle& handle : _backBuffers)
        {
            VkmTextureMetal* newTextureMetal = new VkmTextureMetal(_driver);

            VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();
            handle = renderResourcePool->allocateTexture(newTextureMetal);
            if (newTextureMetal->initialize(handle, textureInfo) == false)
            {
                VKM_DEBUG_ERROR("Failed to create swap chain texture");
                if (handle.isValid())
                    renderResourcePool->releaseResource(handle);
                else
                    delete newTextureMetal;

                destroySwapChain();
                return false;
            }
        }

        // Metal 4 command buffers do not implicitly make referenced resources resident, so the
        // drawable textures must live in a residency set attached to every queue that renders
        // into or presents the backbuffer (today only the present queue). CAMetalLayer exposes a
        // ready-made, auto-updating set for exactly this; track it in the manager and attach it.
        _layerResidencySet = metalLayer.residencySet;
        if (_layerResidencySet != nil)
        {
            VkmCommandQueueMetal* presentQueueMetal = static_cast<VkmCommandQueueMetal*>(_presentQueue);
            [presentQueueMetal->getMTLCommandQueue() addResidencySet:_layerResidencySet];
        }
        else
        {
            // Nil when the layer's device is unset or the GPU lacks residency-set support; the
            // implicit waitForDrawable:/signalDrawable: path in acquire/present still functions.
            VKM_DEBUG_WARN("CAMetalLayer.residencySet unavailable; drawable residency falls back to implicit synchronization");
        }
        return true;
    }

    void VkmSwapChainMetal::destroySwapChain()
    {
        if (_layerResidencySet != nil && _presentQueue != nullptr)
        {
            VkmCommandQueueMetal* presentQueueMetal = static_cast<VkmCommandQueueMetal*>(_presentQueue);
            [presentQueueMetal->getMTLCommandQueue() removeResidencySet:_layerResidencySet];
            _layerResidencySet = nil;
        }

        _currentDrawable = nil;

        destroySwapChainCommon();
    }

    VkmResourceHandle VkmSwapChainMetal::acquireNextImageInner()
    {
        _currentBackBufferIndex = (_currentBackBufferIndex + 1) % BACK_BUFFER_COUNT;
        if (_currentDrawable != nil)
        {
            VkmCommandQueueMetal* commandQueueMetal = static_cast<VkmCommandQueueMetal*>(_presentQueue);
            [commandQueueMetal->getMTLCommandQueue() waitForDrawable:_currentDrawable];

            id<MTLTexture> currentBackBuffer = [_currentDrawable texture];

            VkmRenderResourcePool* renderResourcePool = _driver->getRenderResourcePool();
            VkmTextureMetal* textureMetal = static_cast<VkmTextureMetal*>(renderResourcePool->getResource<VkmTexture>(_backBuffers[_currentBackBufferIndex]));
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
        [commandQueueMetal->getMTLCommandQueue() signalDrawable:_currentDrawable];
        [_currentDrawable present];
    }
} // namespace vkm
