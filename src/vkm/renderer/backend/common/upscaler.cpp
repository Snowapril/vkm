// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/upscaler.h>

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_graph.h>

#include <glm/common.hpp>

#include <cmath>

namespace vkm
{
    float vkmUpscaleModeScale(const VkmUpscaleMode mode)
    {
        switch (mode)
        {
            // No upscaler runs, so the scene renders at the display extent.
            case VkmUpscaleMode::Off:         return 1.0f;
            case VkmUpscaleMode::Native:      return 1.0f;
            case VkmUpscaleMode::Quality:     return 0.67f;
            case VkmUpscaleMode::Balanced:    return 0.59f;
            case VkmUpscaleMode::Performance: return 0.50f;
            default:                          return 1.0f;
        }
    }

    const char* vkmUpscaleModeName(const VkmUpscaleMode mode)
    {
        switch (mode)
        {
            case VkmUpscaleMode::Off:         return "Off";
            case VkmUpscaleMode::Native:      return "Native AA";
            case VkmUpscaleMode::Quality:     return "Quality";
            case VkmUpscaleMode::Balanced:    return "Balanced";
            case VkmUpscaleMode::Performance: return "Performance";
            default:                          return "Unknown";
        }
    }

    VkmUpscaleMode vkmNextUpscaleMode(const VkmUpscaleMode mode)
    {
        const uint32_t next = static_cast<uint32_t>(mode) + 1u;
        return next < static_cast<uint32_t>(VkmUpscaleMode::Count) ? static_cast<VkmUpscaleMode>(next)
                                                                  : VkmUpscaleMode::Off;
    }

    glm::uvec2 vkmUpscaleRenderExtent(const glm::uvec2& displayExtent, const VkmUpscaleMode mode)
    {
        const float scale = vkmUpscaleModeScale(mode);
        return glm::max(glm::uvec2(glm::vec2(displayExtent) * scale + 0.5f), glm::uvec2(1u));
    }

    float vkmHalton(uint32_t index, uint32_t base)
    {
        float result = 0.0f;
        float fraction = 1.0f;
        while (index > 0)
        {
            fraction /= static_cast<float>(base);
            result += fraction * static_cast<float>(index % base);
            index /= base;
        }
        return result;
    }

    uint32_t vkmUpscaleJitterPhaseCount(uint32_t renderWidth, uint32_t displayWidth)
    {
        const float ratio = static_cast<float>(displayWidth) / static_cast<float>(renderWidth);
        return static_cast<uint32_t>(std::ceil(8.0f * ratio * ratio));
    }

    glm::vec2 vkmUpscaleJitterPixels(uint64_t frameCounter, uint32_t phaseCount)
    {
        // One-based: vkmHalton(0, base) is 0, which would repeat the first sample.
        const uint32_t index = static_cast<uint32_t>(frameCounter % phaseCount) + 1u;
        return glm::vec2(vkmHalton(index, 2u) - 0.5f, vkmHalton(index, 3u) - 0.5f);
    }

    bool VkmUpscalerBase::initialize(VkmDriverBase* driver, const VkmUpscalerDescriptor& descriptor)
    {
        if (driver == nullptr || descriptor._renderExtent.x == 0 || descriptor._renderExtent.y == 0 ||
            descriptor._displayExtent.x == 0 || descriptor._displayExtent.y == 0 ||
            descriptor._renderExtent.x > descriptor._displayExtent.x ||
            descriptor._renderExtent.y > descriptor._displayExtent.y)
        {
            VKM_DEBUG_ERROR("VkmUpscaler: extents must be non-zero and render must not exceed display");
            return false;
        }

        _driver = driver;
        _descriptor = descriptor;
        if (!initializeInner())
        {
            _driver = nullptr;
            return false;
        }
        return true;
    }

    void VkmUpscalerBase::destroy()
    {
        if (_driver == nullptr)
        {
            return;
        }
        // destroyInner() releases the vendor context and its history textures synchronously, and
        // neither can go through the deferred reclaimer, so every submission that could still
        // reference them has to have completed first.
        _driver->waitIdle();
        destroyInner();
        _driver = nullptr;
    }

    void VkmUpscalerBase::recordDispatch(VkmRenderGraph* renderGraph,
                                         const VkmUpscalerDispatchDesc& dispatchDesc)
    {
        if (_driver == nullptr || !dispatchDesc._color.isValid() || !dispatchDesc._depth.isValid() ||
            !dispatchDesc._motion.isValid() || !dispatchDesc._output.isValid())
        {
            VKM_DEBUG_ERROR("VkmUpscaler: recordDispatch needs an initialized upscaler and valid handles");
            return;
        }

        const char* name = _descriptor._debugName != nullptr ? _descriptor._debugName : "TemporalUpscale";
        VkmRenderComputeSubGraph* subGraph = renderGraph->beginComputeSubGraph(name);
        subGraph->addReferencedResource(dispatchDesc._color, VkmResourceAccess::ShaderSampledRead);
        subGraph->addReferencedResource(dispatchDesc._depth, VkmResourceAccess::ShaderSampledRead);
        subGraph->addReferencedResource(dispatchDesc._motion, VkmResourceAccess::ShaderSampledRead);
        subGraph->addReferencedResource(dispatchDesc._output, VkmResourceAccess::ShaderStorageReadWrite);
        subGraph->setComputeCallback([this, dispatchDesc](VkmCommandBufferBase* commandBuffer) {
            encodeInner(commandBuffer, dispatchDesc);
        });
    }
} // namespace vkm
