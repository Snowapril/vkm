// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_upscaler.h>

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/backend/metal/metal_command_buffer.h>
#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/metal/metal_texture.h>
#include <vkm/renderer/backend/metal/metal_util.h>

#import <Metal/Metal.h>
#import <MetalFX/MetalFX.h>

namespace vkm
{
    namespace
    {
        id<MTLTexture> resolveTexture(VkmDriverBase* driver, VkmResourceHandle handle)
        {
            VkmTexture* texture = driver->getRenderResourcePool()->getResource<VkmTexture>(handle);
            if (texture == nullptr)
            {
                return nil;
            }
            return static_cast<VkmTextureMetal*>(texture)->getInternalHandle();
        }
    } // namespace

    bool VkmUpscalerMetal::initializeInner()
    {
        id<MTLDevice> device = static_cast<VkmDriverMetal*>(_driver)->getMTLDevice();

        MTLFXTemporalScalerDescriptor* scalerDescriptor = [[MTLFXTemporalScalerDescriptor alloc] init];
        scalerDescriptor.colorTextureFormat = getMTLPixelFormat(_descriptor._colorFormat);
        scalerDescriptor.depthTextureFormat = getMTLPixelFormat(_descriptor._depthFormat);
        scalerDescriptor.motionTextureFormat = getMTLPixelFormat(_descriptor._motionFormat);
        scalerDescriptor.outputTextureFormat = getMTLPixelFormat(_descriptor._outputFormat);
        scalerDescriptor.inputWidth = _descriptor._renderExtent.x;
        scalerDescriptor.inputHeight = _descriptor._renderExtent.y;
        scalerDescriptor.outputWidth = _descriptor._displayExtent.x;
        scalerDescriptor.outputHeight = _descriptor._displayExtent.y;
        scalerDescriptor.autoExposureEnabled = _descriptor._autoExposure ? YES : NO;
        // Synchronous so the scaler upscales at full quality from its first encode; the one-time
        // compile cost sits inside initialize(), which the caller already treats as heavyweight.
        scalerDescriptor.requiresSynchronousInitialization = YES;

        // The MTL4 compiler carries no state the scaler outlives; the PSO path creates one per
        // build the same way.
        MTL4CompilerDescriptor* compilerDescriptor = [[MTL4CompilerDescriptor alloc] init];
        NSError* compilerError = nil;
        id<MTL4Compiler> compiler = [device newCompilerWithDescriptor:compilerDescriptor
                                                                error:&compilerError];
        if (!VKM_MTL_CHECK(compiler, compilerError, "MetalFX upscaler: MTL4Compiler creation failed"))
        {
            return false;
        }

        _scaler = [scalerDescriptor newTemporalScalerWithDevice:device compiler:compiler];
        if (!VKM_MTL_CHECK(_scaler, nil, "MetalFX upscaler: MTL4FXTemporalScaler creation failed"))
        {
            return false;
        }

        // The scaler publishes the usage bits each texture must carry. vkm's flags cover them
        // (render targets are ShaderRead-capable, the output is ShaderWrite); logged so a future
        // MetalFX revision raising its requirements names itself instead of failing downstream.
        VKM_DEBUG_INFO(("MetalFX upscaler created: required usages color=" +
                        std::to_string([_scaler colorTextureUsage]) + " depth=" +
                        std::to_string([_scaler depthTextureUsage]) + " motion=" +
                        std::to_string([_scaler motionTextureUsage]) + " output=" +
                        std::to_string([_scaler outputTextureUsage]))
                           .c_str());
        return true;
    }

    void VkmUpscalerMetal::destroyInner()
    {
        _scaler = nullptr;
    }

    void VkmUpscalerMetal::encodeInner(VkmCommandBufferBase* commandBuffer,
                                       const VkmUpscalerDispatchDesc& dispatchDesc)
    {
        id<MTLTexture> color = resolveTexture(_driver, dispatchDesc._color);
        id<MTLTexture> depth = resolveTexture(_driver, dispatchDesc._depth);
        id<MTLTexture> motion = resolveTexture(_driver, dispatchDesc._motion);
        id<MTLTexture> output = resolveTexture(_driver, dispatchDesc._output);
        if (color == nil || depth == nil || motion == nil || output == nil)
        {
            VKM_DEBUG_ERROR("MetalFX upscaler: a dispatch texture handle no longer resolves");
            return;
        }

        VkmCommandBufferMetal* commandBufferMetal = static_cast<VkmCommandBufferMetal*>(commandBuffer);

        _scaler.colorTexture = color;
        _scaler.depthTexture = depth;
        _scaler.motionTexture = motion;
        _scaler.outputTexture = output;
        _scaler.inputContentWidth = _descriptor._renderExtent.x;
        _scaler.inputContentHeight = _descriptor._renderExtent.y;
        _scaler.jitterOffsetX = dispatchDesc._jitterPixels.x;
        _scaler.jitterOffsetY = dispatchDesc._jitterPixels.y;
        // vkm motion is current-to-previous in UV units (+y down); MetalFX wants pixels pointing
        // at the pixel's previous location, so the UV-to-pixel scale is negated on both axes.
        _scaler.motionVectorScaleX = -static_cast<float>(_descriptor._renderExtent.x);
        _scaler.motionVectorScaleY = -static_cast<float>(_descriptor._renderExtent.y);
        _scaler.reset = dispatchDesc._reset ? YES : NO;
        // glm::perspectiveRH_ZO maps near to 0 and far to 1.
        _scaler.depthReversed = _descriptor._depthInverted ? YES : NO;

        // MetalFX records through its own internal encoders, which the render graph's fences
        // cannot reach. The bridge consumes this subgraph's producer waits and publishes its
        // fence from an empty encoder; the scaler then waits on and updates that same fence, so
        // the pass keeps the exact ordering the graph planned.
        id<MTLFence> subGraphFence = commandBufferMetal->bridgeExternalEncode();
        _scaler.fence = subGraphFence;

        [_scaler encodeToCommandBuffer:commandBufferMetal->getMTLCommandBuffer()];
    }
} // namespace vkm
