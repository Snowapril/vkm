#pragma once

#include <doctest/doctest.h>

#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/backend/common/upscaler.h>
#include <vkm/renderer/gbuffer.h>

#include <cstdint>
#include <cstring>
#include <vector>

// Backend-agnostic body of the temporal upscaler test: drive a real upscaler through a real
// render graph for a few jittered frames and read the display-extent output back. The inputs are
// synthetic -- a half-black/half-white color, cleared depth and zero motion from a VkmGBuffer --
// so the assertion is structural (correct extent, output follows the input's brightness split)
// rather than a golden image, which no vendor upscaler would reproduce bit-exactly.

namespace vkmtest
{
    inline float halfToFloat(uint16_t half)
    {
        const uint32_t sign = (half >> 15) & 0x1;
        const uint32_t exponent = (half >> 10) & 0x1F;
        const uint32_t mantissa = half & 0x3FF;
        float value = 0.0f;
        if (exponent == 0)
        {
            value = static_cast<float>(mantissa) / 1024.0f / 16384.0f;
        }
        else if (exponent < 31)
        {
            value = (1.0f + static_cast<float>(mantissa) / 1024.0f) *
                    static_cast<float>(1 << exponent) / 32768.0f;
        }
        else
        {
            value = mantissa == 0 ? 1e30f : 0.0f; // inf / nan; large sentinel keeps CHECKs simple
        }
        return sign != 0 ? -value : value;
    }

    inline void runTemporalUpscalerTest(vkm::VkmDriverBase* driver)
    {
        if ((driver->getDriverCapabilityFlags() & vkm::VkmDriverCapabilityFlags::TemporalUpscaling) == 0)
        {
            MESSAGE("Skipping: this backend reports no TemporalUpscaling capability.");
            return;
        }
        if ((driver->getDriverCapabilityFlags() & vkm::VkmDriverCapabilityFlags::TextureUpload) == 0)
        {
            MESSAGE("Skipping: this backend does not implement texture upload");
            return;
        }

        const glm::uvec2 renderExtent{ 320u, 180u };
        const glm::uvec2 displayExtent{ 640u, 360u };

        // Depth (cleared to 1.0) and motion (cleared to zero) come from a real VkmGBuffer, the
        // exact textures the gi sample feeds the upscaler.
        vkm::VkmGBuffer gbuffer;
        REQUIRE(gbuffer.initialize(driver, renderExtent));

        // Color: left half black, right half white, alpha 1 -- enough structure to verify the
        // output preserves the split.
        vkm::VkmTextureInfo colorInfo{};
        colorInfo._flags = static_cast<vkm::VkmResourceCreateInfo>(
            static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowShaderRead) |
            static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowTransferDst));
        colorInfo._extent = glm::uvec3(renderExtent, 1);
        colorInfo._numMipLevels = 1;
        colorInfo._numArrayLayers = 1;
        colorInfo._format = vkm::VkmFormat::R16G16B16A16_SFLOAT;
        colorInfo._debugName = "UpscalerTestColor";
        vkm::VkmTexture* colorTexture = driver->newTexture(colorInfo);
        REQUIRE(colorTexture != nullptr);

        constexpr uint16_t kHalfZero = 0x0000;
        constexpr uint16_t kHalfOne = 0x3C00;
        std::vector<uint16_t> colorTexels(static_cast<size_t>(renderExtent.x) * renderExtent.y * 4);
        for (uint32_t y = 0; y < renderExtent.y; ++y)
        {
            for (uint32_t x = 0; x < renderExtent.x; ++x)
            {
                const uint16_t value = x < renderExtent.x / 2 ? kHalfZero : kHalfOne;
                uint16_t* texel = &colorTexels[(static_cast<size_t>(y) * renderExtent.x + x) * 4];
                texel[0] = value;
                texel[1] = value;
                texel[2] = value;
                texel[3] = kHalfOne;
            }
        }
        REQUIRE(driver->uploadToTexture(colorTexture->getHandle(), colorTexels.data(),
                                        colorTexels.size() * sizeof(uint16_t)));

        // ColorAttachment as well: MetalFX publishes render-target usage as part of its
        // output-texture requirements.
        vkm::VkmTextureInfo outputInfo{};
        outputInfo._flags = static_cast<vkm::VkmResourceCreateInfo>(
            static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowShaderRead) |
            static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowShaderWrite) |
            static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowColorAttachment) |
            static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowTransferSrc));
        outputInfo._extent = glm::uvec3(displayExtent, 1);
        outputInfo._numMipLevels = 1;
        outputInfo._numArrayLayers = 1;
        outputInfo._format = vkm::VkmFormat::R16G16B16A16_SFLOAT;
        outputInfo._debugName = "UpscalerTestOutput";
        vkm::VkmTexture* outputTexture = driver->newTexture(outputInfo);
        REQUIRE(outputTexture != nullptr);

        // Tiny attachment for the consumer pass below; its only job is to open a real encoder.
        vkm::VkmTextureInfo consumeInfo{};
        consumeInfo._flags = vkm::VkmResourceCreateInfo::AllowColorAttachment;
        consumeInfo._extent = glm::uvec3(4, 4, 1);
        consumeInfo._numMipLevels = 1;
        consumeInfo._numArrayLayers = 1;
        consumeInfo._format = vkm::VkmFormat::R8G8B8A8_UNORM;
        consumeInfo._debugName = "UpscalerTestConsumeTarget";
        vkm::VkmTexture* consumeTexture = driver->newTexture(consumeInfo);
        REQUIRE(consumeTexture != nullptr);

        vkm::VkmUpscalerDescriptor upscalerDesc{};
        upscalerDesc._renderExtent = renderExtent;
        upscalerDesc._displayExtent = displayExtent;
        upscalerDesc._colorFormat = vkm::VkmFormat::R16G16B16A16_SFLOAT;
        upscalerDesc._depthFormat = vkm::VkmGBuffer::getDepthFormat();
        upscalerDesc._motionFormat = vkm::VkmFormat::R16G16B16A16_SFLOAT;
        upscalerDesc._outputFormat = vkm::VkmFormat::R16G16B16A16_SFLOAT;
        upscalerDesc._debugName = "UpscalerTest";
        vkm::VkmUpscalerBase* upscaler = driver->newUpscaler(upscalerDesc);
        REQUIRE(upscaler != nullptr);

        // A still "camera" over several jittered frames: history accumulates exactly the way the
        // engine drives it, with reset on the first frame only.
        const uint32_t phaseCount = vkm::vkmUpscaleJitterPhaseCount(renderExtent.x, displayExtent.x);
        for (uint64_t frame = 0; frame < 4; ++frame)
        {
            vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);

            // Clears depth to 1.0 and every target (motion included) to zero; no draws.
            renderGraph.beginGraphicsSubGraph(gbuffer.makeFrameBufferDescriptor(), "UpscalerTestClear")
                ->setRenderCallback([](vkm::VkmCommandBufferBase*) {});

            vkm::VkmUpscalerDispatchDesc dispatchDesc{};
            dispatchDesc._color = colorTexture->getHandle();
            dispatchDesc._depth = gbuffer.getDepthTexture();
            dispatchDesc._motion = gbuffer.getTexture(vkm::VkmGBuffer::Target::MotionMetallic);
            dispatchDesc._output = outputTexture->getHandle();
            dispatchDesc._jitterPixels = vkm::vkmUpscaleJitterPixels(frame, phaseCount);
            dispatchDesc._deltaTimeSeconds = 1.0f / 60.0f;
            dispatchDesc._reset = frame == 0;
            dispatchDesc._nearZ = 0.1f;
            dispatchDesc._farZ = 100.0f;
            dispatchDesc._fovYRadians = 1.0f;
            upscaler->recordDispatch(&renderGraph, dispatchDesc);

            // A downstream reader that really opens an encoder, so the upscale subgraph
            // publishes through a fence and the consumer waits on it -- exactly the shape the gi
            // sample's tonemap pass has. An inert declaration alone would leave the fence-wait
            // path unexecuted.
            vkm::VkmFrameBufferDescriptor consumeFb{};
            consumeFb._width = 4;
            consumeFb._height = 4;
            consumeFb._renderPass._colorAttachmentCount = 1;
            consumeFb._renderPass._colorAttachments[0]._attachmentId = 0;
            consumeFb._renderPass._colorAttachments[0]._loadAction = vkm::VkmLoadAction::Clear;
            consumeFb._renderPass._colorAttachments[0]._storeAction = vkm::VkmStoreAction::Store;
            consumeFb._colorAttachments[0] = consumeTexture->getHandle();
            vkm::VkmRenderGraphicsSubGraph* consumeSubGraph =
                renderGraph.beginGraphicsSubGraph(consumeFb, "UpscalerTestConsume");
            consumeSubGraph->addReferencedResource(outputTexture->getHandle(),
                                                   vkm::VkmResourceAccess::ShaderSampledRead);
            consumeSubGraph->setRenderCallback([](vkm::VkmCommandBufferBase*) {});

            renderGraph.compile();
            renderGraph.execute();
            renderGraph.ensureCompleted();
        }

        const vkm::VkmTextureReadbackResult readback = driver->readbackTexture(outputTexture->getHandle());
        REQUIRE(readback.width == displayExtent.x);
        REQUIRE(readback.height == displayExtent.y);
        REQUIRE(readback.channels == 8); // bytes per texel for RGBA16F

        // The output must follow the input's brightness split: a dark left, a bright right, and
        // a real difference between them. Sampled away from the seam and the borders, where any
        // upscaler is free to blur.
        const auto redAt = [&](uint32_t x, uint32_t y) {
            const uint8_t* texel =
                &readback.pixels[(static_cast<size_t>(y) * readback.width + x) * readback.channels];
            uint16_t half = 0;
            std::memcpy(&half, texel, sizeof(half));
            return halfToFloat(half);
        };
        const uint32_t sampleY = displayExtent.y / 2;
        const float leftRed = redAt(displayExtent.x / 4, sampleY);
        const float rightRed = redAt(displayExtent.x * 3 / 4, sampleY);
        CHECK(leftRed < 0.25f);
        CHECK(rightRed > 0.5f);

        upscaler->destroy();
        delete upscaler;
        driver->waitIdle();
        gbuffer.destroy();
        driver->getDeferredReclaimer()->requestRelease(colorTexture->getHandle());
        driver->getDeferredReclaimer()->requestRelease(outputTexture->getHandle());
        driver->getDeferredReclaimer()->requestRelease(consumeTexture->getHandle());
        driver->getDeferredReclaimer()->flushBlocking();
    }
} // namespace vkmtest
