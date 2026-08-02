// Copyright (c) 2025 Snowapril

#include <vkm/renderer/probe_volume_updater.h>

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/resource_table.h>
#include <vkm/renderer/backend/common/pipeline_state.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/sampler.h>
#include <vkm/renderer/backend/common/texture.h>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtx/component_wise.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace vkm
{
    namespace
    {
        bool fail(std::string* outError, const char* message)
        {
            if (outError != nullptr)
            {
                *outError = message;
            }
            VKM_DEBUG_ERROR(message);
            return false;
        }

        /*
        * @brief The six axis-aligned planes of `min`..`max`, in the engine's inward-facing form.
        *
        * A probe sees in every direction and all six of its faces share one cull result, so the
        * capture cannot be culled by any single frustum -- a box around the probes being refreshed
        * is the tightest correct test available.
        */
        void buildBoxPlanes(const glm::vec3& boxMin, const glm::vec3& boxMax, glm::vec4* outPlanes)
        {
            // Same sign convention scene_cull.hlsl tests with: dot(n, c) + w < -radius rejects.
            outPlanes[0] = glm::vec4(1.0f, 0.0f, 0.0f, -boxMin.x);
            outPlanes[1] = glm::vec4(-1.0f, 0.0f, 0.0f, boxMax.x);
            outPlanes[2] = glm::vec4(0.0f, 1.0f, 0.0f, -boxMin.y);
            outPlanes[3] = glm::vec4(0.0f, -1.0f, 0.0f, boxMax.y);
            outPlanes[4] = glm::vec4(0.0f, 0.0f, 1.0f, -boxMin.z);
            outPlanes[5] = glm::vec4(0.0f, 0.0f, -1.0f, boxMax.z);
        }
    } // namespace

    VkmProbeVolumeUpdater::~VkmProbeVolumeUpdater()
    {
        destroy();
    }

    uint32_t VkmProbeVolumeUpdater::getRoundLengthInFrames() const
    {
        const uint32_t probeCount = _volume != nullptr ? _volume->getProbeCount() : 0u;
        const uint32_t budget = std::max(1u, _descriptor._budget);
        return (probeCount + budget - 1u) / budget;
    }

    glm::uvec2 VkmProbeVolumeUpdater::getCaptureAtlasExtent() const
    {
        const uint32_t tiles = _descriptor._budget * 6u;
        const uint32_t rows = (tiles + kCaptureFacesPerRow - 1u) / kCaptureFacesPerRow;
        return glm::uvec2(kCaptureFacesPerRow * _descriptor._captureFaceSize,
                          rows * _descriptor._captureFaceSize);
    }

    uint32_t VkmProbeVolumeUpdater::framesToConverge(uint32_t probeCount, uint32_t budget,
                                                     float hysteresis, float errorFraction)
    {
        VKM_ASSERT(budget > 0u, "framesToConverge needs a non-zero budget");
        VKM_ASSERT(errorFraction > 0.0f && errorFraction < 1.0f,
                   "framesToConverge needs an error fraction in (0, 1)");

        const uint32_t roundLength = (probeCount + budget - 1u) / budget;
        if (hysteresis <= 0.0f)
        {
            return roundLength; // one refresh lands exactly on the new value
        }

        const uint32_t refreshes =
            static_cast<uint32_t>(std::ceil(std::log(errorFraction) / std::log(hysteresis)));
        return roundLength * refreshes;
    }

    bool VkmProbeVolumeUpdater::initialize(VkmDriverBase* driver,
                                           VkmPipelineStateManager* pipelineStateManager,
                                           VkmProbeVolume* volume, const Descriptor& descriptor,
                                           std::string* outError)
    {
        VKM_ASSERT(driver != nullptr, "VkmProbeVolumeUpdater needs a driver");
        VKM_ASSERT(pipelineStateManager != nullptr, "VkmProbeVolumeUpdater needs a pipeline state manager");

        if (volume == nullptr || !volume->isValid())
        {
            return fail(outError, "VkmProbeVolumeUpdater needs an initialized probe volume");
        }
        if (descriptor._budget == 0u || descriptor._budget > kMaxBudget)
        {
            return fail(outError, "A probe budget must be between 1 and kMaxBudget");
        }
        if (descriptor._captureFaceSize == 0u)
        {
            return fail(outError, "A probe capture face needs a non-zero size");
        }
        if (descriptor._cullViewIndex >= kVkmSceneMaxCullViews)
        {
            return fail(outError, "A probe updater's cull view index is out of range");
        }

        _driver = driver;
        _volume = volume;
        _descriptor = descriptor;
        _descriptor._budget = std::min(descriptor._budget, volume->getProbeCount());

        // Derive the probe range from the volume unless the caller pinned it. These are world-space
        // distances, so any fixed default is a guess about scene scale -- and the guess that a
        // 100-unit far plane suits every scene is how a centimetre-scale model ends up with probes
        // that cannot see past the nearest column.
        const VkmProbeVolume::Descriptor& volumeDescriptor = volume->getDescriptor();
        const glm::vec3 gridExtent =
            glm::vec3(volumeDescriptor._probeCounts - glm::uvec3(1u)) * volumeDescriptor._spacing;
        const float gridDiagonal = glm::length(gridExtent);
        const float minSpacing = glm::compMin(volumeDescriptor._spacing);
        if (_descriptor._farZ <= 0.0f)
        {
            // Across the volume, plus a margin: light also arrives from geometry just outside it.
            _descriptor._farZ = std::max(gridDiagonal * 1.5f, 1.0f);
        }
        if (_descriptor._nearZ <= 0.0f)
        {
            // Well inside one cell -- anything nearer than that the grid cannot resolve regardless.
            _descriptor._nearZ = std::max(minSpacing * 0.01f, 1e-3f);
        }
        _everRefreshed.assign(volume->getProbeCount(), false);
        _slice.reserve(_descriptor._budget);

        if (!createCaptureTargets(outError) || !createConstantBuffers(outError) ||
            !createTables(pipelineStateManager, outError))
        {
            destroy();
            return false;
        }
        return true;
    }

    bool VkmProbeVolumeUpdater::createCaptureTargets(std::string* outError)
    {
        const glm::uvec2 extent = getCaptureAtlasExtent();

        VkmTextureInfo colorInfo{};
        colorInfo._flags = static_cast<VkmResourceCreateInfo>(
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowColorAttachment) |
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowShaderRead) |
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowTransferSrc));
        colorInfo._extent = glm::uvec3(extent, 1);
        colorInfo._numMipLevels = 1;
        colorInfo._numArrayLayers = 1;
        // Same format as the atlases: this carries radiance and, in alpha, the distance the
        // Chebyshev moments are built from, neither of which survives 8 bits.
        colorInfo._format = VkmProbeVolume::getIrradianceFormat();
        colorInfo._debugName = "VkmProbeCaptureColor";

        VkmTexture* color = _driver->newTexture(colorInfo);
        if (color == nullptr)
        {
            return fail(outError, "Failed to create the probe capture colour target");
        }
        _captureColor = color->getHandle();

        VkmTextureInfo depthInfo{};
        depthInfo._flags = VkmResourceCreateInfo::AllowDepthStencilAttachment;
        depthInfo._extent = glm::uvec3(extent, 1);
        depthInfo._numMipLevels = 1;
        depthInfo._numArrayLayers = 1;
        depthInfo._format = VkmFormat::D32_SFLOAT;
        depthInfo._debugName = "VkmProbeCaptureDepth";

        VkmTexture* depth = _driver->newTexture(depthInfo);
        if (depth == nullptr)
        {
            return fail(outError, "Failed to create the probe capture depth target");
        }
        _captureDepth = depth->getHandle();

        VkmSamplerInfo samplerInfo{};
        samplerInfo._debugName = "VkmProbeCaptureSampler";
        VkmSampler* sampler = _driver->newSampler(samplerInfo);
        if (sampler == nullptr)
        {
            return fail(outError, "Failed to create the probe capture sampler");
        }
        _sampler = sampler->getHandle();
        return true;
    }

    bool VkmProbeVolumeUpdater::createConstantBuffers(std::string* outError)
    {
        // Uploaded once, here: neither struct depends on which probe is being refreshed (see
        // VkmProbeCaptureConstants), so nothing in them changes frame to frame.
        const auto createAndUpload = [&](const void* data, uint64_t size, const char* name,
                                         VkmResourceHandle& outHandle) {
            VkmBufferInfo info{};
            info._flags = static_cast<VkmResourceCreateInfo>(
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowShaderRead) |
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowTransferDst));
            info._size = size;
            info._debugName = name;

            VkmBuffer* buffer = _driver->newBuffer(info);
            if (buffer == nullptr || !_driver->uploadToBuffer(buffer->getHandle(), data, size))
            {
                return false;
            }
            outHandle = buffer->getHandle();
            return true;
        };

        VkmProbeCaptureConstants captureConstants{};
        vkmBuildProbeFaceViewProjections(glm::vec3(0.0f), _descriptor._nearZ, _descriptor._farZ,
                                         captureConstants._faceViewProjection);
        if (!createAndUpload(&captureConstants, sizeof(captureConstants), "VkmProbeCaptureConstants",
                             _captureConstants))
        {
            return fail(outError, "Failed to create the probe capture constant buffer");
        }

        const glm::uvec2 captureExtent = getCaptureAtlasExtent();
        const VkmProbeVolume::Descriptor& volumeDescriptor = _volume->getDescriptor();

        const auto makeBlendConstants = [&](uint32_t octahedralResolution) {
            VkmProbeBlendConstants constants{};
            vkmBuildProbeFaceViewProjections(glm::vec3(0.0f), _descriptor._nearZ, _descriptor._farZ,
                                             constants._faceViewProjection);
            constants._blendParams = glm::vec4(static_cast<float>(octahedralResolution), 0.0f,
                                               static_cast<float>(kCaptureFacesPerRow),
                                               static_cast<float>(_descriptor._captureFaceSize));
            constants._captureAtlasSize =
                glm::vec4(static_cast<float>(captureExtent.x), static_cast<float>(captureExtent.y), 0.0f, 0.0f);
            return constants;
        };

        const VkmProbeBlendConstants irradiance = makeBlendConstants(volumeDescriptor._irradianceResolution);
        const VkmProbeBlendConstants distance = makeBlendConstants(volumeDescriptor._distanceResolution);
        if (!createAndUpload(&irradiance, sizeof(irradiance), "VkmProbeIrradianceBlendConstants",
                             _irradianceBlendConstants) ||
            !createAndUpload(&distance, sizeof(distance), "VkmProbeDistanceBlendConstants",
                             _distanceBlendConstants))
        {
            return fail(outError, "Failed to create the probe blend constant buffers");
        }
        return true;
    }

    bool VkmProbeVolumeUpdater::createTables(VkmPipelineStateManager* pipelineStateManager,
                                             std::string* outError)
    {
        const auto buildTable = [&](VkmPipelineStateBase* pipeline,
                                    const std::vector<VkmTableResourceEntry>& entries,
                                    VkmResourceTableBase*& outTable) {
            outTable = _driver->newResourceTable(pipeline, VkmResourceSetKind::PerPass, entries, outError);
            return outTable != nullptr;
        };

        // One capture pipeline per vertex layout: a scene's batches are grouped by layout and each
        // permutation is a separate pipeline, so each needs its own table.
        for (uint32_t preset = 0; preset < static_cast<uint32_t>(VkmVertexLayoutPreset::Count); ++preset)
        {
            const std::string name =
                std::string("probe_capture_pso[") +
                vkmVertexLayoutPresetName(static_cast<VkmVertexLayoutPreset>(preset)) + "]";
            VkmPipelineStateBase* pipeline =
                pipelineStateManager->getPipelineState(name, VkmPipelineStateOrigin::Engine);
            if (pipeline == nullptr)
            {
                return fail(outError, "A probe_capture_pso vertex-layout permutation is missing");
            }
            _capturePipelines[preset] = pipeline;
            if (!buildTable(pipeline, {{ 0, _captureConstants }}, _captureTables[preset]))
            {
                return false;
            }
        }

        _irradianceBlendPipeline =
            pipelineStateManager->getPipelineState("probe_blend_pso[irradiance]", VkmPipelineStateOrigin::Engine);
        _distanceBlendPipeline =
            pipelineStateManager->getPipelineState("probe_blend_pso[distance]", VkmPipelineStateOrigin::Engine);
        if (_irradianceBlendPipeline == nullptr || _distanceBlendPipeline == nullptr)
        {
            return fail(outError, "A probe_blend_pso permutation is missing");
        }

        return buildTable(_irradianceBlendPipeline,
                          {{ 0, _captureColor }, { 1, _sampler }, { 2, _irradianceBlendConstants }},
                          _irradianceBlendTable) &&
               buildTable(_distanceBlendPipeline,
                          {{ 0, _captureColor }, { 1, _sampler }, { 2, _distanceBlendConstants }},
                          _distanceBlendTable);
    }

    void VkmProbeVolumeUpdater::destroy()
    {
        if (_driver == nullptr)
        {
            return;
        }

        const auto destroyTable = [](VkmResourceTableBase*& table) {
            if (table != nullptr)
            {
                table->destroy();
                delete table;
                table = nullptr;
            }
        };
        for (VkmResourceTableBase*& table : _captureTables)
        {
            destroyTable(table);
        }
        destroyTable(_irradianceBlendTable);
        destroyTable(_distanceBlendTable);

        // Released immediately, not through the deferred reclaimer, matching VkmGBuffer::destroy():
        // the reclaimer frees only once the GPU timeline passes the frames that used a resource, so
        // anything handed to it at shutdown -- when nothing further will be submitted -- is still
        // held when the allocator tears down, and VMA aborts on the leak. Callers drain before
        // destroy(), the same contract every other resource owner here has.
        const auto release = [this](VkmResourceHandle& handle) {
            if (handle.isValid())
            {
                _driver->getRenderResourcePool()->releaseResource(handle);
                handle = VKM_INVALID_RESOURCE_HANDLE;
            }
        };
        release(_captureColor);
        release(_captureDepth);
        release(_sampler);
        release(_captureConstants);
        release(_irradianceBlendConstants);
        release(_distanceBlendConstants);

        _capturePipelines.fill(nullptr);
        _irradianceBlendPipeline = nullptr;
        _distanceBlendPipeline = nullptr;
        _driver = nullptr;
        _volume = nullptr;
        _descriptor = Descriptor{};
        _slice.clear();
        _sliceHysteresis.clear();
        _everRefreshed.clear();
        _cursor = 0;
        _atlasesCleared = false;
    }

    void VkmProbeVolumeUpdater::advanceSlice()
    {
        const uint32_t probeCount = _volume->getProbeCount();

        // Clamped, not wrapped. Wrapping would refresh some probes twice and others not at all
        // whenever the budget does not divide the probe count, which quietly breaks the guarantee
        // the round length is supposed to give.
        const uint32_t count = std::min(_descriptor._budget, probeCount - _cursor);

        _slice.clear();
        _sliceHysteresis.clear();
        for (uint32_t i = 0; i < count; ++i)
        {
            const uint32_t probeIndex = _cursor + i;
            _slice.push_back(probeIndex);
            _sliceHysteresis.push_back(_everRefreshed[probeIndex] ? _descriptor._hysteresis : 0.0f);
        }
        _cursor = (_cursor + count) % probeCount;
    }

    void VkmProbeVolumeUpdater::record(VkmRenderGraph* renderGraph, VkmScene* scene,
                                       const VkmFrameData& frameData)
    {
        VKM_ASSERT(renderGraph != nullptr, "VkmProbeVolumeUpdater::record requires a render graph");
        VKM_ASSERT(scene != nullptr, "VkmProbeVolumeUpdater::record requires a scene");
        VKM_ASSERT(isValid(), "VkmProbeVolumeUpdater::record requires an initialized updater");

        advanceSlice();
        if (_slice.empty())
        {
            return;
        }

        const uint32_t faceSize = _descriptor._captureFaceSize;
        const glm::uvec2 captureExtent = getCaptureAtlasExtent();

        std::vector<VkmResourceHandle> sceneResources;
        scene->collectReferencedResources(&sceneResources);

        // Cull against the box the refreshed probes can see, not against a camera: a probe looks in
        // all six directions and they share one visible list.
        glm::vec3 boxMin(std::numeric_limits<float>::max());
        glm::vec3 boxMax(std::numeric_limits<float>::lowest());
        for (uint32_t probeIndex : _slice)
        {
            const glm::vec3 position = _volume->getProbePosition(probeIndex);
            boxMin = glm::min(boxMin, position);
            boxMax = glm::max(boxMax, position);
        }
        boxMin -= glm::vec3(_descriptor._farZ);
        boxMax += glm::vec3(_descriptor._farZ);

        VkmFrameData probeFrameData = frameData;
        buildBoxPlanes(boxMin, boxMax, probeFrameData._frustumPlanes);

        const uint32_t frameIndex = renderGraph->frameIndex();
        const auto referenceScene = [&sceneResources](VkmRenderSubGraph* subGraph) {
            for (VkmResourceHandle handle : sceneResources)
            {
                subGraph->addReferencedResource(handle);
            }
        };

        VkmRenderTransferSubGraph* updateSubGraph = renderGraph->beginTransferSubGraph("ProbeSceneUpdate");
        referenceScene(updateSubGraph);
        const uint32_t cullView = _descriptor._cullViewIndex;
        updateSubGraph->setTransferCallback([scene, frameIndex, probeFrameData, cullView](VkmCommandBufferBase* commandBuffer) {
            scene->recordUpdate(commandBuffer, frameIndex, probeFrameData, cullView);
        });

        VkmRenderComputeSubGraph* cullSubGraph = renderGraph->beginComputeSubGraph("ProbeSceneCull");
        referenceScene(cullSubGraph);
        cullSubGraph->setComputeCallback([scene, cullView](VkmCommandBufferBase* commandBuffer) {
            scene->recordCull(commandBuffer, cullView);
        });

        VkmFrameBufferDescriptor captureFb{};
        captureFb._width = captureExtent.x;
        captureFb._height = captureExtent.y;
        captureFb._renderPass._colorAttachmentCount = 1;
        captureFb._renderPass._colorAttachments[0]._attachmentId = 0;
        captureFb._renderPass._colorAttachments[0]._loadAction = VkmLoadAction::Clear;
        captureFb._renderPass._colorAttachments[0]._storeAction = VkmStoreAction::Store;
        captureFb._colorAttachments[0] = _captureColor;
        VkmDepthStencilAttachmentDescriptor captureDepthDesc{};
        captureDepthDesc._attachmentId = 1;
        captureDepthDesc._loadAction = VkmLoadAction::Clear;
        captureDepthDesc._storeAction = VkmStoreAction::Store;
        captureDepthDesc._clearDepth = 1.0f;
        captureFb._renderPass._depthStencilAttachment = captureDepthDesc;
        captureFb._depthStencilAttachment = _captureDepth;

        VkmRenderGraphicsSubGraph* captureSubGraph =
            renderGraph->beginGraphicsSubGraph(captureFb, "ProbeCapture");
        referenceScene(captureSubGraph);
        captureSubGraph->addReferencedResource(_captureColor);
        captureSubGraph->addReferencedResource(_captureDepth);
        captureSubGraph->addReferencedResource(_captureConstants);
        captureSubGraph->setRenderCallback([this, faceSize, scene](VkmCommandBufferBase* commandBuffer) {
            for (uint32_t slot = 0; slot < _slice.size(); ++slot)
            {
                const glm::vec3 probePosition = _volume->getProbePosition(_slice[slot]);
                for (uint32_t face = 0; face < 6; ++face)
                {
                    const uint32_t tile = captureTileBase(slot) + face;
                    commandBuffer->setViewportAndScissor(
                        static_cast<int32_t>((tile % kCaptureFacesPerRow) * faceSize),
                        static_cast<int32_t>((tile / kCaptureFacesPerRow) * faceSize),
                        faceSize, faceSize);

                    VkmProbeCapturePushConstants push{};
                    push._probePositionWorld = probePosition;
                    push._faceIndex = face;

                    scene->recordDrawBatches(
                        commandBuffer,
                        [this](const VkmScene::DrawBatch& batch) {
                            return _capturePipelines[static_cast<uint32_t>(batch._layout)];
                        },
                        [this, &push](VkmCommandBufferBase* cb, const VkmScene::DrawBatch& batch) {
                            cb->bindResourceTable(_captureTables[static_cast<uint32_t>(batch._layout)]);
                            cb->setPushConstants(&push, sizeof(push));
                        },
                        _descriptor._cullViewIndex);
                }
            }
        });

        VkmRenderComputeSubGraph* barrierSubGraph = renderGraph->beginComputeSubGraph("ProbeCaptureToShaderRead");
        barrierSubGraph->addReferencedResource(_captureColor);
        barrierSubGraph->setComputeCallback([this](VkmCommandBufferBase* commandBuffer) {
            commandBuffer->barrierTextureForShaderRead(_captureColor);
        });

        // Two passes, not two attachments: the atlases have different extents, so a probe's cell is
        // at a different place in each.
        const auto recordBlend = [&](const char* name, VkmResourceHandle atlas, const glm::uvec2& atlasExtent,
                                     uint32_t cellSize, VkmPipelineStateBase* pipeline,
                                     VkmResourceTableBase* table, VkmResourceHandle constants,
                                     bool distanceAtlas) {
            VkmFrameBufferDescriptor fb{};
            fb._width = atlasExtent.x;
            fb._height = atlasExtent.y;
            fb._renderPass._colorAttachmentCount = 1;
            fb._renderPass._colorAttachments[0]._attachmentId = 0;
            // Load, so the probes this frame does not touch keep their values -- which is the whole
            // point of a round-robin budget. The first update has to Clear instead: on Vulkan the
            // first render pass transitions the atlas out of UNDEFINED and discards its contents.
            fb._renderPass._colorAttachments[0]._loadAction =
                _atlasesCleared ? VkmLoadAction::Load : VkmLoadAction::Clear;
            fb._renderPass._colorAttachments[0]._storeAction = VkmStoreAction::Store;
            fb._colorAttachments[0] = atlas;

            VkmRenderGraphicsSubGraph* subGraph = renderGraph->beginGraphicsSubGraph(fb, name);
            subGraph->addReferencedResource(atlas);
            subGraph->addReferencedResource(_captureColor);
            subGraph->addReferencedResource(_sampler);
            subGraph->addReferencedResource(constants);
            subGraph->setRenderCallback([this, pipeline, table, cellSize, distanceAtlas](
                                            VkmCommandBufferBase* commandBuffer) {
                commandBuffer->bindPipeline(pipeline);
                commandBuffer->bindResourceTable(table);
                for (uint32_t slot = 0; slot < _slice.size(); ++slot)
                {
                    const uint32_t probeIndex = _slice[slot];
                    const glm::uvec2 origin = distanceAtlas
                                                  ? _volume->getDistanceProbeTexelOrigin(probeIndex)
                                                  : _volume->getIrradianceProbeTexelOrigin(probeIndex);
                    commandBuffer->setViewportAndScissor(static_cast<int32_t>(origin.x),
                                                         static_cast<int32_t>(origin.y),
                                                         cellSize, cellSize);

                    VkmProbeBlendPushConstants push{};
                    push._captureTileBase = captureTileBase(slot);
                    push._hysteresis = _sliceHysteresis[slot];
                    commandBuffer->setPushConstants(&push, sizeof(push));
                    commandBuffer->draw(3, 1, 0, 0);
                }
            });
        };

        const VkmProbeVolume::Descriptor& volumeDescriptor = _volume->getDescriptor();
        const uint32_t border = 2u * VkmProbeVolume::kBorderTexels;
        recordBlend("ProbeBlendIrradiance", _volume->getIrradianceTexture(),
                    _volume->getIrradianceAtlasExtent(), volumeDescriptor._irradianceResolution + border,
                    _irradianceBlendPipeline, _irradianceBlendTable, _irradianceBlendConstants,
                    /*distanceAtlas=*/false);
        recordBlend("ProbeBlendDistance", _volume->getDistanceTexture(),
                    _volume->getDistanceAtlasExtent(), volumeDescriptor._distanceResolution + border,
                    _distanceBlendPipeline, _distanceBlendTable, _distanceBlendConstants,
                    /*distanceAtlas=*/true);

        _atlasesCleared = true;
        for (uint32_t probeIndex : _slice)
        {
            _everRefreshed[probeIndex] = true;
        }
    }
} // namespace vkm
