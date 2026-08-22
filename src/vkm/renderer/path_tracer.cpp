// Copyright (c) 2026 Snowapril

#include <vkm/renderer/path_tracer.h>

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/resource_table.h>

#include <cmath>

namespace vkm
{
    namespace
    {
        // Matches [numthreads(8, 8, 1)] in path_trace.hlsl. Not read from the compiled shader the
        // way the dispatch path does, because the group count has to be computed on the CPU here.
        constexpr uint32_t kThreadGroupSize = 8;

        bool fail(std::string* outError, const std::string& message)
        {
            if (outError != nullptr)
            {
                *outError = message;
            }
            return false;
        }
    } // namespace

    bool vkmLoadRayTracingPipelineStates(VkmPipelineStateManager* pipelineStateManager, std::string* outError)
    {
        VKM_ASSERT(pipelineStateManager != nullptr,
                   "vkmLoadRayTracingPipelineStates requires a pipeline state manager");

        std::string psoError;
        if (!pipelineStateManager->loadPipelineStatesFromDirectory(
                std::string(RESOURCES_DIR) + "Pipelines/RayTracing/",
                std::string(RESOURCES_DIR) + "Shaders/ShaderCache/",
                VkmPipelineStateOrigin::Engine, &psoError))
        {
            return fail(outError, "Failed to load the ray-tracing pipeline states: " + psoError);
        }
        return true;
    }

    bool VkmPathTracer::initialize(VkmDriverBase* driver, VkmPipelineStateManager* pipelineStateManager,
                                   uint32_t width, uint32_t height, std::string* outError)
    {
        VKM_ASSERT(driver != nullptr, "VkmPathTracer::initialize requires a driver");
        VKM_ASSERT(pipelineStateManager != nullptr, "VkmPathTracer::initialize requires a pipeline state manager");

        if ((driver->getDriverCapabilityFlags() & VkmDriverCapabilityFlags::RayTracing) == 0)
        {
            return fail(outError, "This device reports no ray tracing capability");
        }
        if (width == 0 || height == 0)
        {
            return fail(outError, "VkmPathTracer needs a non-empty output extent");
        }

        std::string psoError;
        _pipeline = pipelineStateManager->getPipelineState("path_trace_pso[default]", VkmPipelineStateOrigin::Engine);
        if (_pipeline == nullptr)
        {
            return fail(outError, "path_trace_pso[default] is not loaded; call vkmLoadRayTracingPipelineStates first");
        }

        _width = width;
        _height = height;
        _sampleIndex = 0;

        VkmBufferInfo bufferInfo{};
        // AllowTransferSrc so a caller can read the average back, which is the whole point of a
        // reference; AllowTransferDst so it can be zeroed without a dispatch.
        bufferInfo._flags = static_cast<VkmResourceCreateInfo>(
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowShaderWrite) |
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowTransferSrc) |
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowTransferDst));
        bufferInfo._size = static_cast<uint64_t>(width) * height * 4 * sizeof(float);
        bufferInfo._debugName = "PathTraceAccumulation";
        VkmBuffer* accumulation = driver->newBuffer(bufferInfo);
        if (accumulation == nullptr)
        {
            return fail(outError, "Failed to create the path tracer's accumulation buffer");
        }
        _accumulationBuffer = accumulation->getHandle();

        // Immutable and built once: the table names only this buffer, which lives as long as the
        // tracer does.
        _resourceTable = driver->newResourceTable(_pipeline, VkmResourceSetKind::PerPass,
                                                  {{ 0, _accumulationBuffer }}, &psoError);
        if (_resourceTable == nullptr)
        {
            destroy(driver);
            return fail(outError, "Failed to build the path tracer's per-pass resource table: " + psoError);
        }
        return true;
    }

    void VkmPathTracer::destroy(VkmDriverBase* driver)
    {
        VKM_ASSERT(driver != nullptr, "VkmPathTracer::destroy requires a driver");

        if (_resourceTable != nullptr)
        {
            _resourceTable->destroy();
            delete _resourceTable;
            _resourceTable = nullptr;
        }

        VkmDeferredResourceReclaimer* reclaimer = driver->getDeferredReclaimer();
        if (reclaimer != nullptr && _accumulationBuffer != VKM_INVALID_RESOURCE_HANDLE)
        {
            reclaimer->requestRelease(_accumulationBuffer);
        }
        _accumulationBuffer = VKM_INVALID_RESOURCE_HANDLE;
        _pipeline = nullptr;
        _width = 0;
        _height = 0;
        _sampleIndex = 0;
    }

    void VkmPathTracer::recordAccumulate(VkmCommandBufferBase* commandBuffer, const VkmPathTraceOptions& options)
    {
        VKM_ASSERT(commandBuffer != nullptr, "VkmPathTracer::recordAccumulate requires a command buffer");

        if (_pipeline == nullptr || _resourceTable == nullptr)
        {
            VKM_DEBUG_ERROR("recordAccumulate before a successful VkmPathTracer::initialize");
            return;
        }

        VkmPathTraceConstants constants{};
        constants._width = _width;
        constants._height = _height;
        constants._sampleIndex = _sampleIndex;
        // Zero would trace no path at all and record black, which is a silent way to produce a
        // reference of nothing; one bounce is the least that means anything.
        constants._maxBounces = options._maxBounces > 0 ? options._maxBounces : 1;
        constants._environmentR = options._environmentRadiance.x;
        constants._environmentG = options._environmentRadiance.y;
        constants._environmentB = options._environmentRadiance.z;
        constants._jitterPrimaryRay = options._jitterPrimaryRay ? 1u : 0u;

        commandBuffer->bindPipeline(_pipeline);
        commandBuffer->bindResourceTable(_resourceTable);
        commandBuffer->setPushConstants(&constants, sizeof(constants));
        commandBuffer->dispatch((_width + kThreadGroupSize - 1) / kThreadGroupSize,
                                (_height + kThreadGroupSize - 1) / kThreadGroupSize,
                                1);
        commandBuffer->unbindPipeline();

        ++_sampleIndex;
    }

    namespace
    {
        // Both images carry a running sum plus its sample count, so this is what makes them
        // comparable at all. A pixel nothing has been accumulated into is not black -- it is
        // unknown, and averaging it as black would bias whichever image had fewer samples.
        bool normalizePixel(const float* rgba, float* outRgb)
        {
            const float samples = rgba[3];
            if (samples <= 0.0f)
            {
                return false;
            }
            const float inverse = 1.0f / samples;
            outRgb[0] = rgba[0] * inverse;
            outRgb[1] = rgba[1] * inverse;
            outRgb[2] = rgba[2] * inverse;
            return true;
        }
    } // namespace

    float vkmComputeImageMse(const float* referenceRgba, const float* candidateRgba, uint32_t pixelCount)
    {
        VKM_ASSERT(referenceRgba != nullptr && candidateRgba != nullptr,
                   "vkmComputeImageMse requires both images");

        double total = 0.0;
        uint32_t counted = 0;
        for (uint32_t pixel = 0; pixel < pixelCount; ++pixel)
        {
            float reference[3];
            float candidate[3];
            if (!normalizePixel(referenceRgba + pixel * 4, reference) ||
                !normalizePixel(candidateRgba + pixel * 4, candidate))
            {
                continue;
            }
            for (uint32_t channel = 0; channel < 3; ++channel)
            {
                const double difference = static_cast<double>(candidate[channel]) - reference[channel];
                total += difference * difference;
            }
            ++counted;
        }
        return counted == 0 ? 0.0f : static_cast<float>(total / (counted * 3.0));
    }

    float vkmComputeImageRelativeMse(const float* referenceRgba, const float* candidateRgba,
                                     uint32_t pixelCount)
    {
        VKM_ASSERT(referenceRgba != nullptr && candidateRgba != nullptr,
                   "vkmComputeImageRelativeMse requires both images");

        // Large enough to keep a near-black reference pixel from dominating, small enough not to
        // flatten genuinely dim regions -- the value the RelMSE literature uses.
        constexpr double kEpsilon = 1.0e-2;

        double total = 0.0;
        uint32_t counted = 0;
        for (uint32_t pixel = 0; pixel < pixelCount; ++pixel)
        {
            float reference[3];
            float candidate[3];
            if (!normalizePixel(referenceRgba + pixel * 4, reference) ||
                !normalizePixel(candidateRgba + pixel * 4, candidate))
            {
                continue;
            }
            for (uint32_t channel = 0; channel < 3; ++channel)
            {
                const double difference = static_cast<double>(candidate[channel]) - reference[channel];
                const double denominator = static_cast<double>(reference[channel]) * reference[channel] + kEpsilon;
                total += (difference * difference) / denominator;
            }
            ++counted;
        }
        return counted == 0 ? 0.0f : static_cast<float>(total / (counted * 3.0));
    }
} // namespace vkm
