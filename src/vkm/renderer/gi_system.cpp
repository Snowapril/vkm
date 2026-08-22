// Copyright (c) 2026 Snowapril

#include <vkm/renderer/gi_system.h>

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/bindless_resource_manager.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/resource_table.h>
#include <vkm/renderer/backend/common/sampler.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/gbuffer.h>
#include <vkm/renderer/gi_composite.h>
#include <vkm/renderer/path_tracer.h>
#include <vkm/renderer/scene/scene.h>

#include <glm/gtx/component_wise.hpp>

#include <algorithm>

namespace vkm
{
    namespace
    {
        constexpr VkmFormat kGiHdrFormat = VkmFormat::R16G16B16A16_SFLOAT;

        bool fail(std::string* outError, const std::string& message)
        {
            if (outError != nullptr)
            {
                *outError = message;
            }
            return false;
        }

        VkmBuffer* createUniformBuffer(VkmDriverBase* driver, uint64_t size, const char* name)
        {
            VkmBufferInfo info{};
            info._flags = static_cast<VkmResourceCreateInfo>(
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowShaderRead) |
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowTransferDst));
            info._size = size;
            info._debugName = name;
            return driver->newBuffer(info);
        }

        VkmFrameBufferDescriptor makeFullscreenFb(const glm::uvec2& extent, VkmResourceHandle target)
        {
            VkmFrameBufferDescriptor fb{};
            fb._width = extent.x;
            fb._height = extent.y;
            fb._renderPass._colorAttachmentCount = 1;
            fb._renderPass._colorAttachments[0]._attachmentId = 0;
            fb._renderPass._colorAttachments[0]._loadAction = VkmLoadAction::DontCare;
            fb._renderPass._colorAttachments[0]._storeAction = VkmStoreAction::Store;
            fb._colorAttachments[0] = target;
            return fb;
        }

        void recordFullscreen(VkmRenderGraph* renderGraph, const char* name, const glm::uvec2& extent,
                              VkmResourceHandle target, VkmPipelineStateBase* pipeline,
                              VkmResourceTableBase* table)
        {
            VkmRenderGraphicsSubGraph* subGraph =
                renderGraph->beginGraphicsSubGraph(makeFullscreenFb(extent, target), name);
            subGraph->addReferencedResource(target, VkmResourceAccess::ColorAttachmentWrite);
            std::vector<VkmResourceAccessDeclaration> bound;
            table->collectReferencedResources(&bound);
            subGraph->addReferencedResources(bound);
            subGraph->setRenderCallback([pipeline, table](VkmCommandBufferBase* commandBuffer) {
                commandBuffer->bindPipeline(pipeline);
                commandBuffer->bindResourceTable(table);
                commandBuffer->draw(3, 1, 0, 0);
            });
        }
    } // namespace

    bool VkmGiSystem::initialize(VkmDriverBase* driver, VkmPipelineStateManager* pipelineStateManager,
                                 VkmGBuffer* gbuffer, const VkmGiSystemDescriptor& descriptor,
                                 std::string* outError)
    {
        VKM_ASSERT(driver != nullptr, "VkmGiSystem::initialize requires a driver");
        VKM_ASSERT(pipelineStateManager != nullptr, "VkmGiSystem::initialize requires a pipeline state manager");
        VKM_ASSERT(gbuffer != nullptr, "VkmGiSystem::initialize requires a G-buffer");

        _driver = driver;
        _pipelineStateManager = pipelineStateManager;
        _gbuffer = gbuffer;
        _descriptor = descriptor;

        _probeLightingPipeline =
            pipelineStateManager->getPipelineState("probe_lighting_pso", VkmPipelineStateOrigin::Engine);
        _ssgiPipeline = pipelineStateManager->getPipelineState("ssgi_pso", VkmPipelineStateOrigin::Engine);
        if (_probeLightingPipeline == nullptr || _ssgiPipeline == nullptr)
        {
            return fail(outError, "The GI system needs the engine PSO cache; run a build that generates it");
        }

        VkmSamplerInfo samplerInfo{};
        samplerInfo._debugName = "GiSystemSampler";
        VkmSampler* sampler = driver->newSampler(samplerInfo);
        if (sampler == nullptr)
        {
            return fail(outError, "Failed to create the GI system's sampler");
        }
        _sampler = sampler->getHandle();

        VkmBuffer* volumeBuffer =
            createUniformBuffer(driver, sizeof(VkmProbeVolumeConstants), "GiProbeVolumeConstants");
        VkmBuffer* ssgiBuffer = createUniformBuffer(driver, sizeof(VkmSsgiConstants), "GiSsgiConstants");
        if (volumeBuffer == nullptr || ssgiBuffer == nullptr)
        {
            destroy();
            return fail(outError, "Failed to create the GI system's uniform buffers");
        }
        _volumeBuffer = volumeBuffer->getHandle();
        _ssgiBuffer = ssgiBuffer->getHandle();

        // The second technique needs ray tracing; on a device without it the probe tier is the
        // only entry. Loaded exactly once -- a second load of the same directory destroys
        // pipelines live passes still hold pointers to.
        if ((driver->getDriverCapabilityFlags() & VkmDriverCapabilityFlags::RayTracing) != 0)
        {
            std::string rtError;
            if (vkmLoadRayTracingPipelineStates(pipelineStateManager, &rtError))
            {
                _rtPipelinesLoaded = true;
            }
            else
            {
                VKM_DEBUG_ERROR(("ReSTIR unavailable: " + rtError).c_str());
            }
        }
        return true;
    }

    bool VkmGiSystem::prepareScene(VkmScene* scene, std::string* outError)
    {
        VKM_ASSERT(scene != nullptr, "VkmGiSystem::prepareScene requires a scene");
        if (_driver == nullptr)
        {
            return fail(outError, "prepareScene before a successful VkmGiSystem::initialize");
        }
        _scene = scene;

        // Fit the grid to what was loaded. A probe outside the geometry contributes nothing but
        // still costs a full capture, so the grid is sized from the scene rather than guessed.
        const VkmSceneAABB bounds = scene->computeWorldBounds();
        const glm::vec3 center = bounds._valid ? bounds.getCenter() : glm::vec3(0.0f);
        const glm::vec3 extent = bounds._valid ? bounds.getExtent() : glm::vec3(8.0f);

        VkmProbeVolume::Descriptor volumeDescriptor{};
        volumeDescriptor._probeCounts = glm::max(_descriptor._probeCounts, glm::uvec3(2u));
        // A margin outside the bounds, so surfaces at the very edge still sit between probes
        // rather than outside the grid, where the lookup returns black.
        const glm::vec3 span = extent * 1.2f;
        volumeDescriptor._spacing =
            glm::max(span / glm::vec3(volumeDescriptor._probeCounts - glm::uvec3(1u)), glm::vec3(0.05f));
        volumeDescriptor._origin =
            center - glm::vec3(volumeDescriptor._probeCounts - glm::uvec3(1u)) * volumeDescriptor._spacing * 0.5f;
        if (!_volume.initialize(_driver, volumeDescriptor))
        {
            return fail(outError, "Failed to create the GI probe volume");
        }

        // The capture pass pushes once per (probe, face, batch), so the budget has to fit inside
        // one frame's push-constant ring region on Metal and WebGPU.
        const uint32_t batchCount = static_cast<uint32_t>(scene->getDrawBatches().size());
        const uint32_t ringBudget = kVkmPushConstantRingEntryCount / std::max(1u, 6u * batchCount + 2u);
        const uint32_t requestedBudget = _descriptor._probeBudget;
        const uint32_t budget = std::clamp(std::min(requestedBudget, ringBudget), 1u,
                                           VkmProbeVolumeUpdater::kMaxBudget);
        if (budget < requestedBudget)
        {
            VKM_DEBUG_INFO(("Probe budget limited to " + std::to_string(budget) + " (from " +
                            std::to_string(requestedBudget) + ") by the push-constant ring at " +
                            std::to_string(batchCount) + " draw batches").c_str());
        }

        VkmProbeVolumeUpdater::Descriptor updaterDescriptor{};
        updaterDescriptor._cullViewIndex = _descriptor._probeCullView;
        updaterDescriptor._budget = budget;
        updaterDescriptor._hysteresis = _descriptor._probeHysteresis;
        std::string error;
        if (!_updater.initialize(_driver, _pipelineStateManager, &_volume, updaterDescriptor, &error))
        {
            return fail(outError, "Failed to create the probe updater: " + error);
        }
        if (!_updater.buildMaterialTables(*scene, &error))
        {
            return fail(outError, "Failed to build the probe capture's material tables: " + error);
        }

        const VkmProbeVolumeConstants volumeConstants =
            _volume.makeConstants(_descriptor._probeNormalBiasFraction);
        _driver->uploadToBuffer(_volumeBuffer, &volumeConstants, sizeof(volumeConstants));

        // The contact term covers what falls between probes, so its reach is a fraction of the
        // probe spacing rather than a fixed distance.
        VkmSsgiConstants ssgi{};
        ssgi._params.y = glm::compMin(volumeDescriptor._spacing) * 0.35f;
        _driver->uploadToBuffer(_ssgiBuffer, &ssgi, sizeof(ssgi));

        // What makes ReSTIR selectable at all: the traced passes read the scene through this.
        // A failure downgrades to the probe technique rather than failing the scene.
        if (_rtPipelinesLoaded)
        {
            if (scene->buildAccelerationStructures(_driver, &error))
            {
                _restirAvailable = true;
            }
            else
            {
                VKM_DEBUG_ERROR(("ReSTIR unavailable: " + error).c_str());
            }
        }

        _sceneReady = true;
        return true;
    }

    bool VkmGiSystem::resize(const glm::uvec2& extent, VkmResourceHandle directTarget,
                             std::string* outError)
    {
        if (_driver == nullptr)
        {
            return fail(outError, "resize before a successful VkmGiSystem::initialize");
        }
        if (extent.x == 0 || extent.y == 0)
        {
            return fail(outError, "VkmGiSystem needs a non-empty extent");
        }
        if (_gbuffer->getExtent() != extent)
        {
            return fail(outError, "Resize the G-buffer to the same extent before the GI system");
        }
        _extent = extent;

        // Everything sized or bound per extent retires together: tables must outlive the frames
        // that bound them, and the restir pass deletes its own tables immediately in destroy().
        Retired retired{};
        retired._retiredAtFrame = _frameCounter;
        if (_probeLightingTable != nullptr)
        {
            retired._tables.push_back(_probeLightingTable);
            _probeLightingTable = nullptr;
        }
        if (_ssgiTable != nullptr)
        {
            retired._tables.push_back(_ssgiTable);
            _ssgiTable = nullptr;
        }
        retired._restir = std::move(_restir);
        if (!retired._tables.empty() || retired._restir != nullptr)
        {
            _retired.push_back(std::move(retired));
        }

        if (_indirectTarget != VKM_INVALID_RESOURCE_HANDLE)
        {
            _driver->getDeferredReclaimer()->requestRelease(_indirectTarget);
            _indirectTarget = VKM_INVALID_RESOURCE_HANDLE;
        }
        VkmTextureInfo targetInfo{};
        targetInfo._flags = static_cast<VkmResourceCreateInfo>(
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowColorAttachment) |
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowShaderRead));
        targetInfo._extent = glm::uvec3(extent, 1);
        targetInfo._numMipLevels = 1;
        targetInfo._numArrayLayers = 1;
        targetInfo._format = kGiHdrFormat;
        targetInfo._debugName = "GiIndirect";
        VkmTexture* indirect = _driver->newTexture(targetInfo);
        if (indirect == nullptr)
        {
            return fail(outError, "Failed to create the GI indirect target");
        }
        _indirectTarget = indirect->getHandle();

        const VkmResourceHandle normal = _gbuffer->getTexture(VkmGBuffer::Target::Normal);
        const VkmResourceHandle motion = _gbuffer->getTexture(VkmGBuffer::Target::MotionMetallic);
        std::string error;
        _probeLightingTable = _driver->newResourceTable(
            _probeLightingPipeline, VkmResourceSetKind::PerPass,
            {{ 0, normal }, { 1, motion }, { 2, _volume.getIrradianceTexture() },
             { 3, _volume.getDistanceTexture() }, { 4, _sampler }, { 5, _volumeBuffer },
             { 6, _volume.getProbeOffsetTexture() }}, &error);
        _ssgiTable = _driver->newResourceTable(
            _ssgiPipeline, VkmResourceSetKind::PerPass,
            {{ 0, normal }, { 1, motion }, { 2, directTarget }, { 3, _sampler }, { 4, _ssgiBuffer }},
            &error);
        if (_probeLightingTable == nullptr || _ssgiTable == nullptr)
        {
            return fail(outError, "Failed to build the GI system's per-pass tables: " + error);
        }

        if (_restirAvailable)
        {
            _restir = std::make_unique<VkmRestirPass>();
            if (!_restir->initialize(_driver, _pipelineStateManager, *_gbuffer, extent.x, extent.y,
                                     &error))
            {
                VKM_DEBUG_ERROR(("Failed to size the ReSTIR pass: " + error).c_str());
                _restir.reset();
            }
            _gbufferParity = 0;
        }
        return true;
    }

    VkmRestirOptions VkmGiSystem::makeRestirOptions() const
    {
        VkmRestirOptions options{};
        options._environmentRadiance = glm::vec3(_options._environmentRadiance);
        options._temporalResampling = true;
        options._spatialResampling = true;
        // Scaled with the resolution, per the option's own guidance (roughly 30 px at 1080p).
        options._neighbourRadius = std::max(8.0f, static_cast<float>(_extent.x) * (30.0f / 1920.0f));
        return options;
    }

    void VkmGiSystem::recordProbeTier(VkmRenderGraph* renderGraph, const VkmFrameData& frameData)
    {
        // The probe refresh owns its cull view and records its own update, cull, capture and blend.
        _updater.record(renderGraph, _scene, frameData);

        recordFullscreen(renderGraph, "GiProbeLighting", _extent, _indirectTarget,
                         _probeLightingPipeline, _probeLightingTable);

        // The contact term is added on top of the probe result, in the same target: its PSO blends
        // one-to-one and the pass loads rather than discards, so it can only ever brighten what the
        // probes produced (restir.md section 5).
        if (_options._ssgi)
        {
            VkmFrameBufferDescriptor ssgiFb = makeFullscreenFb(_extent, _indirectTarget);
            ssgiFb._renderPass._colorAttachments[0]._loadAction = VkmLoadAction::Load;
            VkmRenderGraphicsSubGraph* ssgiSubGraph = renderGraph->beginGraphicsSubGraph(ssgiFb, "GiSsgi");
            ssgiSubGraph->addReferencedResource(_indirectTarget, VkmResourceAccess::ColorAttachmentWrite);
            std::vector<VkmResourceAccessDeclaration> ssgiBound;
            _ssgiTable->collectReferencedResources(&ssgiBound);
            ssgiSubGraph->addReferencedResources(ssgiBound);
            VkmPipelineStateBase* pipeline = _ssgiPipeline;
            VkmResourceTableBase* table = _ssgiTable;
            ssgiSubGraph->setRenderCallback([pipeline, table](VkmCommandBufferBase* commandBuffer) {
                commandBuffer->bindPipeline(pipeline);
                commandBuffer->bindResourceTable(table);
                commandBuffer->draw(3, 1, 0, 0);
            });
        }
    }

    void VkmGiSystem::recordRestirTier(VkmRenderGraph* renderGraph, uint32_t frameIndex)
    {
        // The lighting constants carry this frame's parity-dependent slice indices, so they are
        // staged before the resample that will produce those slices.
        VkmRenderTransferSubGraph* constantsSubGraph =
            renderGraph->beginTransferSubGraph("GiRestirConstants");
        constantsSubGraph->addReferencedResource(_restir->getLightingConstantBuffer(),
                                                 VkmResourceAccess::TransferWrite);
        constantsSubGraph->addReferencedResource(_restir->getLightingStagingBuffer(frameIndex),
                                                 VkmResourceAccess::TransferRead);
        constantsSubGraph->setTransferCallback([this, frameIndex](VkmCommandBufferBase* commandBuffer) {
            _restir->recordUpdateLightingConstants(commandBuffer, frameIndex, makeRestirOptions(),
                                                   _options._restirDebugView,
                                                   _options._restirMisBlend ? 1.0f : 0.0f);
        });

        // The traced passes read the scene through sets 0/1 plus the TLAS, so the compute
        // subgraph declares the same Draw-phase set the G-buffer draw does.
        VkmRenderComputeSubGraph* resampleSubGraph = renderGraph->beginComputeSubGraph("GiRestir");
        std::vector<VkmResourceAccessDeclaration> referenced;
        _scene->collectReferencedResources(VkmScene::ReferencePhase::Draw, &referenced);
        resampleSubGraph->addReferencedResources(referenced);
        resampleSubGraph->addReferencedResource(_scene->getTopLevelAccelerationStructure(),
                                                VkmResourceAccess::AccelerationStructureShaderRead);
        for (uint32_t i = 0; i < VkmGBuffer::kTargetCount; ++i)
        {
            resampleSubGraph->addReferencedResource(_gbuffer->getTexture(static_cast<VkmGBuffer::Target>(i)),
                                                    VkmResourceAccess::ShaderSampledRead);
            resampleSubGraph->addReferencedResource(_gbuffer->getPrevTexture(static_cast<VkmGBuffer::Target>(i)),
                                                    VkmResourceAccess::ShaderSampledRead);
        }
        resampleSubGraph->addReferencedResource(_restir->getReservoirBuffer(),
                                                VkmResourceAccess::ShaderStorageReadWrite);
        resampleSubGraph->setComputeCallback([this](VkmCommandBufferBase* commandBuffer) {
            _restir->recordResample(commandBuffer, makeRestirOptions(), _gbufferParity);
        });

        VkmRenderGraphicsSubGraph* lightingSubGraph =
            renderGraph->beginGraphicsSubGraph(makeFullscreenFb(_extent, _indirectTarget),
                                               "GiRestirLighting");
        lightingSubGraph->addReferencedResource(_indirectTarget, VkmResourceAccess::ColorAttachmentWrite);
        for (uint32_t i = 0; i < VkmGBuffer::kTargetCount; ++i)
        {
            lightingSubGraph->addReferencedResource(_gbuffer->getTexture(static_cast<VkmGBuffer::Target>(i)),
                                                    VkmResourceAccess::ShaderSampledRead);
        }
        lightingSubGraph->addReferencedResource(_restir->getReservoirBuffer(),
                                                VkmResourceAccess::ShaderStorageReadWrite);
        lightingSubGraph->addReferencedResource(_restir->getLightingConstantBuffer(),
                                                VkmResourceAccess::ConstantBufferRead);
        lightingSubGraph->setRenderCallback([this](VkmCommandBufferBase* commandBuffer) {
            _restir->recordLighting(commandBuffer, _gbufferParity);
        });
    }

    void VkmGiSystem::record(VkmRenderGraph* renderGraph, const VkmFrameData& frameData,
                             uint32_t frameIndex)
    {
        VKM_ASSERT(renderGraph != nullptr, "VkmGiSystem::record requires a render graph");

        ++_frameCounter;
        drainRetired();
        if (!_sceneReady || _indirectTarget == VKM_INVALID_RESOURCE_HANDLE)
        {
            return;
        }
        // The runtime capability gate, not an #ifdef (restir.md section 5).
        if (!isRestirAvailable())
        {
            _options._technique = VkmGiTechnique::ProbeVolume;
        }

        if (_options._technique == VkmGiTechnique::Restir)
        {
            recordRestirTier(renderGraph, frameIndex);
        }
        else
        {
            recordProbeTier(renderGraph, frameData);
        }
    }

    bool VkmGiSystem::setProbeOffset(uint32_t probeIndex, const glm::vec3& offset)
    {
        if (!_volume.isValid() || probeIndex >= _volume.getProbeCount())
        {
            return false;
        }
        _volume.setProbeOffset(probeIndex, offset);
        _updater.invalidateProbe(probeIndex);
        // Frames already submitted are still sampling the offset texture, and the upload copies
        // into it outside the render graph. Draining first is what makes that safe; this is a
        // manual placement action, not a per-frame path.
        _driver->waitIdle();
        return _volume.uploadProbeOffsets();
    }

    bool VkmGiSystem::clearProbeOffsets()
    {
        if (!_volume.isValid())
        {
            return false;
        }
        const uint32_t probeCount = _volume.getProbeCount();
        for (uint32_t probeIndex = 0; probeIndex < probeCount; ++probeIndex)
        {
            _updater.invalidateProbe(probeIndex);
        }
        _volume.clearProbeOffsets();
        _driver->waitIdle();
        return _volume.uploadProbeOffsets();
    }

    void VkmGiSystem::advanceFrame()
    {
        _gbufferParity ^= 1u;
    }

    void VkmGiSystem::drainRetired()
    {
        for (size_t i = _retired.size(); i-- > 0;)
        {
            if (_frameCounter < _retired[i]._retiredAtFrame + FRAME_BUFFER_COUNT)
            {
                continue;
            }
            for (VkmResourceTableBase* table : _retired[i]._tables)
            {
                table->destroy();
                delete table;
            }
            if (_retired[i]._restir != nullptr)
            {
                _retired[i]._restir->destroy(_driver);
            }
            _retired.erase(_retired.begin() + static_cast<ptrdiff_t>(i));
        }
    }

    void VkmGiSystem::releaseTables()
    {
        for (VkmResourceTableBase** table : { &_probeLightingTable, &_ssgiTable })
        {
            if (*table != nullptr)
            {
                (*table)->destroy();
                delete *table;
                *table = nullptr;
            }
        }
    }

    void VkmGiSystem::destroy()
    {
        if (_driver == nullptr)
        {
            return;
        }

        releaseTables();
        for (Retired& retired : _retired)
        {
            for (VkmResourceTableBase* table : retired._tables)
            {
                table->destroy();
                delete table;
            }
            if (retired._restir != nullptr)
            {
                retired._restir->destroy(_driver);
            }
        }
        _retired.clear();
        if (_restir != nullptr)
        {
            _restir->destroy(_driver);
            _restir.reset();
        }
        _updater.destroy();
        _volume.destroy();

        VkmDeferredResourceReclaimer* reclaimer = _driver->getDeferredReclaimer();
        for (VkmResourceHandle* handle : { &_indirectTarget, &_volumeBuffer, &_ssgiBuffer })
        {
            if (handle->isValid())
            {
                reclaimer->requestRelease(*handle);
                *handle = VKM_INVALID_RESOURCE_HANDLE;
            }
        }
        // The sampler stays with the resource pool until teardown, like every other engine
        // sampler: nothing per-frame ever rebinds it, so there is no in-flight hazard to defer.
        _sampler = VKM_INVALID_RESOURCE_HANDLE;

        _probeLightingPipeline = nullptr;
        _ssgiPipeline = nullptr;
        _scene = nullptr;
        _sceneReady = false;
        _restirAvailable = false;
        _rtPipelinesLoaded = false;
        _extent = glm::uvec2(0, 0);
        _driver = nullptr;
    }
} // namespace vkm
