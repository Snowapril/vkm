// Copyright (c) 2026 Snowapril

#include <vkm/renderer/indirect_pass.h>

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/resource_table.h>
#include <vkm/renderer/gbuffer.h>
#include <vkm/renderer/path_tracer.h>

namespace vkm
{
    namespace
    {
        // Matches [numthreads(8, 8, 1)] in gi_indirect.hlsl.
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

    bool VkmIndirectPass::initialize(VkmDriverBase* driver, VkmPipelineStateManager* pipelineStateManager,
                                     const VkmGBuffer& gbuffer, uint32_t width, uint32_t height,
                                     std::string* outError)
    {
        VKM_ASSERT(driver != nullptr, "VkmIndirectPass::initialize requires a driver");
        VKM_ASSERT(pipelineStateManager != nullptr, "VkmIndirectPass::initialize requires a pipeline state manager");

        if ((driver->getDriverCapabilityFlags() & VkmDriverCapabilityFlags::RayTracing) == 0)
        {
            return fail(outError, "This device reports no ray tracing capability");
        }
        if (width == 0 || height == 0)
        {
            return fail(outError, "VkmIndirectPass needs a non-empty output extent");
        }
        if (gbuffer.getExtent().x != width || gbuffer.getExtent().y != height)
        {
            // A mismatch would silently sample the wrong texels rather than fail, since Load()
            // clamps nothing -- and the result would look like a subtly wrong estimator.
            return fail(outError, "VkmIndirectPass's extent must match the G-buffer's");
        }

        // Resolved, not loaded: see vkmLoadRayTracingPipelineStates for why a second load of the
        // same directory would destroy the pipeline VkmPathTracer is already holding.
        std::string psoError;
        _pipeline = pipelineStateManager->getPipelineState("gi_indirect_pso[default]", VkmPipelineStateOrigin::Engine);
        if (_pipeline == nullptr)
        {
            return fail(outError, "gi_indirect_pso[default] is not loaded; call vkmLoadRayTracingPipelineStates first");
        }

        _width = width;
        _height = height;
        _sampleIndex = 0;

        VkmBufferInfo bufferInfo{};
        bufferInfo._flags = static_cast<VkmResourceCreateInfo>(
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowShaderWrite) |
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowTransferSrc) |
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowTransferDst));
        bufferInfo._size = static_cast<uint64_t>(width) * height * 4 * sizeof(float);
        bufferInfo._debugName = "IndirectAccumulation";
        VkmBuffer* accumulation = driver->newBuffer(bufferInfo);
        if (accumulation == nullptr)
        {
            return fail(outError, "Failed to create the indirect pass's accumulation buffer");
        }
        _accumulationBuffer = accumulation->getHandle();

        // Binding order mirrors gi_indirect.json: normal, base colour, motion (for the camera
        // distance), then the output.
        _resourceTable = driver->newResourceTable(
            _pipeline, VkmResourceSetKind::PerPass,
            {{ 0, gbuffer.getTexture(VkmGBuffer::Target::Normal) },
             { 1, gbuffer.getTexture(VkmGBuffer::Target::BaseColorRoughness) },
             { 2, gbuffer.getTexture(VkmGBuffer::Target::MotionMetallic) },
             { 3, _accumulationBuffer }},
            &psoError);
        if (_resourceTable == nullptr)
        {
            destroy(driver);
            return fail(outError, "Failed to build the indirect pass's per-pass resource table: " + psoError);
        }
        return true;
    }

    void VkmIndirectPass::destroy(VkmDriverBase* driver)
    {
        VKM_ASSERT(driver != nullptr, "VkmIndirectPass::destroy requires a driver");

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

    void VkmIndirectPass::recordAccumulate(VkmCommandBufferBase* commandBuffer, const VkmIndirectOptions& options)
    {
        VKM_ASSERT(commandBuffer != nullptr, "VkmIndirectPass::recordAccumulate requires a command buffer");

        if (_pipeline == nullptr || _resourceTable == nullptr)
        {
            VKM_DEBUG_ERROR("recordAccumulate before a successful VkmIndirectPass::initialize");
            return;
        }

        VkmIndirectConstants constants{};
        constants._width = _width;
        constants._height = _height;
        constants._sampleIndex = _sampleIndex;
        // Zero scatters would trace nothing and record black; one is the least that means anything.
        constants._maxBounces = options._maxBounces > 0 ? options._maxBounces : 1;
        constants._environmentR = options._environmentRadiance.x;
        constants._environmentG = options._environmentRadiance.y;
        constants._environmentB = options._environmentRadiance.z;

        commandBuffer->bindPipeline(_pipeline);
        commandBuffer->bindResourceTable(_resourceTable);
        commandBuffer->setPushConstants(&constants, sizeof(constants));
        commandBuffer->dispatch((_width + kThreadGroupSize - 1) / kThreadGroupSize,
                                (_height + kThreadGroupSize - 1) / kThreadGroupSize,
                                1);
        commandBuffer->unbindPipeline();

        ++_sampleIndex;
    }
} // namespace vkm
