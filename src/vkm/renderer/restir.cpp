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

#include <cmath>

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

    void vkmBuildNeighbourOffsets(float* outOffsets, uint32_t offsetCount)
    {
        VKM_ASSERT(outOffsets != nullptr, "vkmBuildNeighbourOffsets requires an output array");

        // The plastic number's reciprocals: the R2 sequence's two irrational increments. Chosen
        // over a Halton or a spiral because the spatial pass reads a *run* of consecutive entries,
        // and R2's consecutive points are well separated in both dimensions at every scale.
        constexpr double kPlastic = 1.32471795724474602596;
        const double alphaX = 1.0 / kPlastic;
        const double alphaY = 1.0 / (kPlastic * kPlastic);

        for (uint32_t index = 0; index < offsetCount; ++index)
        {
            const double u = std::fmod(0.5 + alphaX * (index + 1), 1.0);
            const double v = std::fmod(0.5 + alphaY * (index + 1), 1.0);

            // Shirley-Chiu concentric square-to-disk. The naive polar map (r = sqrt(u)) bunches
            // points towards the centre, which is where a neighbour carries the least new
            // information; concentric keeps the distribution even out to the rim.
            const double a = 2.0 * u - 1.0;
            const double b = 2.0 * v - 1.0;
            double radius = 0.0;
            double angle = 0.0;
            if (a != 0.0 || b != 0.0)
            {
                if (std::abs(a) > std::abs(b))
                {
                    radius = a;
                    angle = (3.14159265358979323846 / 4.0) * (b / a);
                }
                else
                {
                    radius = b;
                    angle = (3.14159265358979323846 / 2.0) - (3.14159265358979323846 / 4.0) * (a / b);
                }
            }
            outOffsets[index * 2 + 0] = static_cast<float>(radius * std::cos(angle));
            outOffsets[index * 2 + 1] = static_cast<float>(radius * std::sin(angle));
        }
    }

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
        _temporalPipeline = pipelineStateManager->getPipelineState("gi_reservoir_temporal_pso[default]",
                                                                   VkmPipelineStateOrigin::Engine);
        _spatialPipeline = pipelineStateManager->getPipelineState("gi_reservoir_spatial_pso[default]",
                                                                  VkmPipelineStateOrigin::Engine);
        _resolvePipeline = pipelineStateManager->getPipelineState("gi_reservoir_resolve_pso[default]",
                                                                  VkmPipelineStateOrigin::Engine);
        if (_generatePipeline == nullptr || _temporalPipeline == nullptr || _spatialPipeline == nullptr ||
            _resolvePipeline == nullptr)
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

        // Phase 8.2. Uploaded once at setup: the pattern is fixed, and computing it per pixel is
        // exactly what a lookup table exists to avoid.
        float offsets[kVkmNeighbourOffsetCount * 2];
        vkmBuildNeighbourOffsets(offsets, kVkmNeighbourOffsetCount);
        VkmBuffer* neighbourOffsets =
            createStorageBuffer(driver, sizeof(offsets), "RestirNeighbourOffsets");
        if (neighbourOffsets == nullptr ||
            !driver->uploadToBuffer(neighbourOffsets->getHandle(), offsets, sizeof(offsets)))
        {
            if (neighbourOffsets != nullptr)
            {
                _neighbourOffsetBuffer = neighbourOffsets->getHandle();
            }
            destroy(driver);
            return fail(outError, "Failed to upload the neighbour offset table");
        }
        _neighbourOffsetBuffer = neighbourOffsets->getHandle();

        // Binding orders mirror the PSO jsons.
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

        // History-tap validation reads last frame's G-buffer, so this is the one table that binds
        // the previous set. Before the first advanceFrame() those textures exist but hold nothing
        // rendered; the temporal pass's per-tap validation reads their zero camera distance as a
        // disocclusion, which is the right answer for "there is no history yet".
        _temporalTable = driver->newResourceTable(
            _temporalPipeline, VkmResourceSetKind::PerPass,
            {{ 0, gbuffer.getTexture(VkmGBuffer::Target::Normal) },
             { 1, gbuffer.getTexture(VkmGBuffer::Target::MotionMetallic) },
             { 2, gbuffer.getPrevTexture(VkmGBuffer::Target::Normal) },
             { 3, gbuffer.getPrevTexture(VkmGBuffer::Target::MotionMetallic) },
             { 4, _reservoirBuffer }},
            &psoError);
        if (_temporalTable == nullptr)
        {
            destroy(driver);
            return fail(outError, "Failed to build the temporal resampling table: " + psoError);
        }

        _spatialTable = driver->newResourceTable(
            _spatialPipeline, VkmResourceSetKind::PerPass,
            {{ 0, gbuffer.getTexture(VkmGBuffer::Target::Normal) },
             { 1, gbuffer.getTexture(VkmGBuffer::Target::MotionMetallic) },
             { 2, _neighbourOffsetBuffer },
             { 3, _reservoirBuffer }},
            &psoError);
        if (_spatialTable == nullptr)
        {
            destroy(driver);
            return fail(outError, "Failed to build the spatial resampling table: " + psoError);
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
        releaseTable(_temporalTable);
        releaseTable(_spatialTable);
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
        release(_neighbourOffsetBuffer);
        release(_accumulationBuffer);

        _generatePipeline = nullptr;
        _temporalPipeline = nullptr;
        _spatialPipeline = nullptr;
        _resolvePipeline = nullptr;
        _width = 0;
        _height = 0;
        _sampleIndex = 0;
    }

    void VkmRestirPass::recordAccumulate(VkmCommandBufferBase* commandBuffer, const VkmRestirOptions& options)
    {
        VKM_ASSERT(commandBuffer != nullptr, "VkmRestirPass::recordAccumulate requires a command buffer");

        if (_generatePipeline == nullptr || _temporalPipeline == nullptr || _spatialPipeline == nullptr ||
            _resolvePipeline == nullptr)
        {
            VKM_DEBUG_ERROR("recordAccumulate before a successful VkmRestirPass::initialize");
            return;
        }
        if (options._outputSlice >= kVkmReservoirSliceCount || options._inputSlice >= kVkmReservoirSliceCount)
        {
            VKM_DEBUG_ERROR("VkmRestirOptions names a slice outside the reservoir buffer");
            return;
        }
        if (options._spatialResampling && options._outputSlice == options._inputSlice)
        {
            // Spatial reuse reads its neighbours from the slice it is writing, so a pass that
            // aliased them would resample results it had already produced this frame -- a feedback
            // loop rather than a bias, and one that looks plausible until it does not.
            VKM_DEBUG_ERROR("Spatial resampling needs distinct input and output reservoir slices");
            return;
        }

        /*
        * Slice routing. Without temporal reuse the options drive it: generation writes
        * `_inputSlice`, spatial moves the result to `_outputSlice`, the resolve reads whichever
        * the last writer used. With temporal reuse the routing is fixed by the pass: generation
        * writes the fresh slice (2), temporal merges it with last frame's history slice `p` into
        * `1 - p` where `p = _sampleIndex & 1`, and spatial — when on — moves that into `p`, the
        * slice just consumed as history and therefore free. Only the temporal output is ever
        * re-ingested as history, never the spatial output: feeding spatial results back into the
        * temporal loop drives correlation and detail erosion (restir.md section 9).
        */
        const uint32_t parity = _sampleIndex & 1u;
        constexpr uint32_t kFreshSlice = 2;
        const uint32_t historySlice = parity;
        const uint32_t temporalOutSlice = 1u - parity;

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
        constants._neighbourCount = options._neighbourCount;
        constants._neighbourRadius = options._neighbourRadius;
        constants._normalThreshold = options._normalThreshold;
        constants._depthThreshold = options._depthThreshold;
        constants._historySlice = historySlice;
        constants._confidenceCap = options._confidenceCap;
        constants._maxSampleAge = options._maxSampleAge;

        const uint32_t groupsX = (_width + kThreadGroupSize - 1) / kThreadGroupSize;
        const uint32_t groupsY = (_height + kThreadGroupSize - 1) / kThreadGroupSize;

        VkmRestirConstants generateConstants = constants;
        generateConstants._outputSlice = options._temporalResampling ? kFreshSlice : options._inputSlice;

        commandBuffer->bindPipeline(_generatePipeline);
        commandBuffer->bindResourceTable(_generateTable);
        commandBuffer->setPushConstants(&generateConstants, sizeof(generateConstants));
        commandBuffer->dispatch(groupsX, groupsY, 1);
        // Closing the compute pass here is what orders the next pass's reads after this one's
        // writes -- the same mechanism VkmScene::recordCull relies on between its two dispatches.
        commandBuffer->unbindPipeline();

        VkmRestirConstants resolveConstants = constants;
        resolveConstants._inputSlice = options._inputSlice;

        if (options._temporalResampling)
        {
            VkmRestirConstants temporalConstants = constants;
            temporalConstants._inputSlice = kFreshSlice;
            temporalConstants._outputSlice = temporalOutSlice;

            commandBuffer->bindPipeline(_temporalPipeline);
            commandBuffer->bindResourceTable(_temporalTable);
            commandBuffer->setPushConstants(&temporalConstants, sizeof(temporalConstants));
            commandBuffer->dispatch(groupsX, groupsY, 1);
            commandBuffer->unbindPipeline();
            resolveConstants._inputSlice = temporalOutSlice;
        }

        if (options._spatialResampling)
        {
            VkmRestirConstants spatialConstants = constants;
            if (options._temporalResampling)
            {
                spatialConstants._inputSlice = temporalOutSlice;
                spatialConstants._outputSlice = historySlice;
            }

            commandBuffer->bindPipeline(_spatialPipeline);
            commandBuffer->bindResourceTable(_spatialTable);
            commandBuffer->setPushConstants(&spatialConstants, sizeof(spatialConstants));
            commandBuffer->dispatch(groupsX, groupsY, 1);
            commandBuffer->unbindPipeline();
            resolveConstants._inputSlice = spatialConstants._outputSlice;
        }

        commandBuffer->bindPipeline(_resolvePipeline);
        commandBuffer->bindResourceTable(_resolveTable);
        commandBuffer->setPushConstants(&resolveConstants, sizeof(resolveConstants));
        commandBuffer->dispatch(groupsX, groupsY, 1);
        commandBuffer->unbindPipeline();

        ++_sampleIndex;
    }
} // namespace vkm
