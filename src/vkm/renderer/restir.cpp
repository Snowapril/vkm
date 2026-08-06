// Copyright (c) 2025 Snowapril

#include <vkm/renderer/restir.h>

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
        // Matches [numthreads(8, 8, 1)] in both reservoir shaders.
        constexpr uint32_t kThreadGroupSize = 8;

        bool fail(std::string* outError, const std::string& message)
        {
            if (outError != nullptr)
            {
                *outError = message;
            }
            return false;
        }

        VkmBuffer* createStorageBuffer(VkmDriverBase* driver, uint64_t size, const char* debugName)
        {
            VkmBufferInfo bufferInfo{};
            bufferInfo._flags = static_cast<VkmResourceCreateInfo>(
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowShaderWrite) |
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowTransferSrc) |
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowTransferDst));
            bufferInfo._size = size;
            bufferInfo._debugName = debugName;
            return driver->newBuffer(bufferInfo);
        }
    } // namespace

    bool VkmRestirPass::initialize(VkmDriverBase* driver, VkmPipelineStateManager* pipelineStateManager,
                                   const VkmGBuffer& gbuffer, uint32_t width, uint32_t height,
                                   std::string* outError)
    {
        VKM_ASSERT(driver != nullptr, "VkmRestirPass::initialize requires a driver");
        VKM_ASSERT(pipelineStateManager != nullptr, "VkmRestirPass::initialize requires a pipeline state manager");

        if ((driver->getDriverCapabilityFlags() & VkmDriverCapabilityFlags::RayTracing) == 0)
        {
            return fail(outError, "This device reports no ray tracing capability");
        }
        if (width == 0 || height == 0)
        {
            return fail(outError, "VkmRestirPass needs a non-empty output extent");
        }
        if (gbuffer.getExtent().x != width || gbuffer.getExtent().y != height)
        {
            return fail(outError, "VkmRestirPass's extent must match the G-buffer's");
        }

        std::string psoError;
        _generatePipeline = pipelineStateManager->getPipelineState("gi_reservoir_generate_pso[default]",
                                                                   VkmPipelineStateOrigin::Engine);
        _resolvePipeline = pipelineStateManager->getPipelineState("gi_reservoir_resolve_pso[default]",
                                                                  VkmPipelineStateOrigin::Engine);
        if (_generatePipeline == nullptr || _resolvePipeline == nullptr)
        {
            return fail(outError, "The reservoir pipelines are not loaded; call vkmLoadRayTracingPipelineStates first");
        }

        _width = width;
        _height = height;
        _sampleIndex = 0;

        const uint64_t pixelCount = static_cast<uint64_t>(width) * height;
        VkmBuffer* reservoirs = createStorageBuffer(
            driver, pixelCount * kVkmReservoirSliceCount * kVkmReservoirByteStride, "RestirReservoirs");
        if (reservoirs == nullptr)
        {
            return fail(outError, "Failed to create the reservoir buffer");
        }
        _reservoirBuffer = reservoirs->getHandle();

        VkmBuffer* accumulation =
            createStorageBuffer(driver, pixelCount * 4 * sizeof(float), "RestirAccumulation");
        if (accumulation == nullptr)
        {
            destroy(driver);
            return fail(outError, "Failed to create the ReSTIR accumulation buffer");
        }
        _accumulationBuffer = accumulation->getHandle();

        // Binding orders mirror the two PSO jsons.
        _generateTable = driver->newResourceTable(
            _generatePipeline, VkmResourceSetKind::PerPass,
            {{ 0, gbuffer.getTexture(VkmGBuffer::Target::Normal) },
             { 1, gbuffer.getTexture(VkmGBuffer::Target::MotionMetallic) },
             { 2, _reservoirBuffer }},
            &psoError);
        if (_generateTable == nullptr)
        {
            destroy(driver);
            return fail(outError, "Failed to build the reservoir generation table: " + psoError);
        }

        _resolveTable = driver->newResourceTable(
            _resolvePipeline, VkmResourceSetKind::PerPass,
            {{ 0, gbuffer.getTexture(VkmGBuffer::Target::Normal) },
             { 1, gbuffer.getTexture(VkmGBuffer::Target::BaseColorRoughness) },
             { 2, gbuffer.getTexture(VkmGBuffer::Target::MotionMetallic) },
             { 3, _reservoirBuffer },
             { 4, _accumulationBuffer }},
            &psoError);
        if (_resolveTable == nullptr)
        {
            destroy(driver);
            return fail(outError, "Failed to build the reservoir resolve table: " + psoError);
        }
        return true;
    }

    void VkmRestirPass::destroy(VkmDriverBase* driver)
    {
        VKM_ASSERT(driver != nullptr, "VkmRestirPass::destroy requires a driver");

        const auto releaseTable = [](VkmResourceTableBase*& table) {
            if (table != nullptr)
            {
                table->destroy();
                delete table;
                table = nullptr;
            }
        };
        releaseTable(_generateTable);
        releaseTable(_resolveTable);

        VkmDeferredResourceReclaimer* reclaimer = driver->getDeferredReclaimer();
        const auto release = [reclaimer](VkmResourceHandle& handle) {
            if (reclaimer != nullptr && handle != VKM_INVALID_RESOURCE_HANDLE)
            {
                reclaimer->requestRelease(handle);
            }
            handle = VKM_INVALID_RESOURCE_HANDLE;
        };
        release(_reservoirBuffer);
        release(_accumulationBuffer);

        _generatePipeline = nullptr;
        _resolvePipeline = nullptr;
        _width = 0;
        _height = 0;
        _sampleIndex = 0;
    }

    void VkmRestirPass::recordAccumulate(VkmCommandBufferBase* commandBuffer, const VkmRestirOptions& options)
    {
        VKM_ASSERT(commandBuffer != nullptr, "VkmRestirPass::recordAccumulate requires a command buffer");

        if (_generatePipeline == nullptr || _resolvePipeline == nullptr)
        {
            VKM_DEBUG_ERROR("recordAccumulate before a successful VkmRestirPass::initialize");
            return;
        }
        if (options._outputSlice >= kVkmReservoirSliceCount || options._inputSlice >= kVkmReservoirSliceCount)
        {
            VKM_DEBUG_ERROR("VkmRestirOptions names a slice outside the reservoir buffer");
            return;
        }

        VkmRestirConstants constants{};
        constants._width = _width;
        constants._height = _height;
        constants._sampleIndex = _sampleIndex;
        constants._maxBounces = options._maxBounces > 0 ? options._maxBounces : 1;
        constants._environmentR = options._environmentRadiance.x;
        constants._environmentG = options._environmentRadiance.y;
        constants._environmentB = options._environmentRadiance.z;
        constants._outputSlice = options._outputSlice;
        constants._inputSlice = options._inputSlice;

        const uint32_t groupsX = (_width + kThreadGroupSize - 1) / kThreadGroupSize;
        const uint32_t groupsY = (_height + kThreadGroupSize - 1) / kThreadGroupSize;

        commandBuffer->bindPipeline(_generatePipeline);
        commandBuffer->bindResourceTable(_generateTable);
        commandBuffer->setPushConstants(&constants, sizeof(constants));
        commandBuffer->dispatch(groupsX, groupsY, 1);
        // Closing the compute pass here is what orders the resolve's reads after generation's
        // writes -- the same mechanism VkmScene::recordCull relies on between its two dispatches.
        commandBuffer->unbindPipeline();

        commandBuffer->bindPipeline(_resolvePipeline);
        commandBuffer->bindResourceTable(_resolveTable);
        commandBuffer->setPushConstants(&constants, sizeof(constants));
        commandBuffer->dispatch(groupsX, groupsY, 1);
        commandBuffer->unbindPipeline();

        ++_sampleIndex;
    }
} // namespace vkm
