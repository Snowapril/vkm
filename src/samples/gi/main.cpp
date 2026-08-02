// Copyright (c) 2025 Snowapril
//
// The GI sample: the first application to drive the deferred chain end to end, and the first place
// the low-spec GI tier appears on screen.
//
// Per frame, in this order:
//
//   probe refresh      a round-robin slice of probes, culled against their own box (cull view 1)
//   scene update+cull  the camera's view (cull view 0)
//   G-buffer           MRT scene pass
//   deferred lighting  direct light, into an HDR target
//   probe lighting     the GI technique's output: indirect irradiance, into another HDR target
//   composite          direct + indirect * albedo, or a debug channel
//   tone map           into the backbuffer
//
// Two cull views in one frame is the point of VkmScene's viewIndex: a probe sees in every
// direction, so culling its capture against the camera frustum would drop exactly the geometry
// behind the camera that bounces light into the scene.

#include <cxxopts.hpp>

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/matrix.hpp>

#if defined(VKM_ENABLE_IMGUI)
#include <imgui.h>
#endif

#include <vkm/base/common.h>
#include <vkm/base/global_variable.h>
#include <vkm/platform/common/app_delegate.h>
#include <vkm/renderer/backend/common/bindless_resource_manager.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/per_pass_resource_table.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/sampler.h>
#include <vkm/renderer/backend/common/swapchain.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/camera.h>
#include <vkm/renderer/engine.h>
#include <vkm/renderer/gbuffer.h>
#include <vkm/renderer/gi_composite.h>
#include <vkm/renderer/probe_volume.h>
#include <vkm/renderer/probe_volume_updater.h>
#include <vkm/renderer/screenshot.h>
#include <vkm/renderer/scene/gltf_importer.h>
#include <vkm/renderer/backend/common/staging_buffer.h>
#include <vkm/renderer/scene/scene.h>

#if defined(VKM_PLATFORM_WINDOWS)
#include <vkm/platform/windows/application.h>
#elif defined(VKM_PLATFORM_WASM)
#include <vkm/platform/wasm/application.h>
#elif defined(VKM_PLATFORM_LINUX)
#include <vkm/platform/linux/application.h>
#else
#include <vkm/platform/apple/application.h>
#endif

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

using namespace vkm;

VKM_GLOBAL_VARIABLE(std::string, gv_gi_model_path,
                    std::string(RESOURCES_DIR) + "Scenes/Sponza/Sponza.gltf");
// Write a PNG of the composed frame and quit. This is how the sample gets verified at all on a
// machine where nobody can look at the window -- see vkmWriteTexturePng.
VKM_GLOBAL_VARIABLE(std::string, gv_gi_screenshot, "");
// Which frame to capture. Probes refresh a slice per frame and converge over rounds, so a
// screenshot taken too early shows a half-lit volume rather than the technique's actual output.
VKM_GLOBAL_VARIABLE(uint32_t, gv_gi_screenshot_frame, 600u);
// Which channel to show, as a VkmGiDebugView. Settable here as well as through the UI so a
// screenshot run can capture a specific term without anyone touching the window.
VKM_GLOBAL_VARIABLE(uint32_t, gv_gi_debug_view, 0u);

namespace
{
    constexpr VkmFormat kHdrFormat = VkmFormat::R16G16B16A16_SFLOAT;
    constexpr glm::vec3 kLightDirection{ 0.4f, 0.8f, 0.45f }; // towards the light

    // Mirrors LightConstants in deferred_lighting.hlsl.
    struct LightConstants
    {
        glm::vec4 _directionToLight{ 0.0f, 1.0f, 0.0f, 0.0f };
        glm::vec4 _radiance{ 1.0f, 1.0f, 1.0f, 0.0f };
    };

    // Mirrors TonemapConstants in tonemap.hlsl.
    struct TonemapConstants
    {
        glm::vec4 _exposureGamma{ 1.0f, 2.2f, 0.0f, 0.0f };
    };

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

    VkmTexture* createHdrTarget(VkmDriverBase* driver, const glm::uvec2& extent, const char* name)
    {
        VkmTextureInfo info{};
        info._flags = static_cast<VkmResourceCreateInfo>(
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowColorAttachment) |
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowShaderRead));
        info._extent = glm::uvec3(extent, 1);
        info._numMipLevels = 1;
        info._numArrayLayers = 1;
        info._format = kHdrFormat;
        info._debugName = name;
        return driver->newTexture(info);
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
} // namespace

class GiApplication : public AppDelegate
{
public:
    GiApplication() = default;
    virtual ~GiApplication() = default;

    virtual const char* getAppName() const override final { return "gi"; }

    virtual void postDriverReady(VkmEngine* engine) override final
    {
        _engine = engine;
        _cameraController.registerTo(engine->getInputHandler());
        engine->setActiveCamera(&_camera);

        // Every pass this sample runs is an engine PSO, already loaded from the engine cache --
        // there is no sample-owned shader here at all, which is what lets it build on WebGPU
        // without a per-sample WGSL cache.
        VkmPipelineStateManager* manager = engine->getPipelineStateManager();
        for (uint8_t i = 0; i < static_cast<uint8_t>(VkmVertexLayoutPreset::Count); ++i)
        {
            const std::string name = std::string("gbuffer_pso[") +
                                     vkmVertexLayoutPresetName(static_cast<VkmVertexLayoutPreset>(i)) + "]";
            _gbufferPipelines[i] = manager->getPipelineState(name, VkmPipelineStateOrigin::Engine);
        }
        _lightingPipeline = manager->getPipelineState("deferred_lighting_pso", VkmPipelineStateOrigin::Engine);
        _probeLightingPipeline = manager->getPipelineState("probe_lighting_pso", VkmPipelineStateOrigin::Engine);
        _compositePipeline = manager->getPipelineState("gi_composite_pso", VkmPipelineStateOrigin::Engine);
        _tonemapPipeline = manager->getPipelineState("tonemap_pso", VkmPipelineStateOrigin::Engine);
        if (_lightingPipeline == nullptr || _probeLightingPipeline == nullptr ||
            _compositePipeline == nullptr || _tonemapPipeline == nullptr)
        {
            VKM_DEBUG_ERROR("The GI sample needs the engine PSO cache; run a build that generates it");
            return;
        }

        VkmDriverBase* driver = engine->getDriver();
        VkmSamplerInfo samplerInfo{};
        samplerInfo._debugName = "GiSampler";
        VkmSampler* sampler = driver->newSampler(samplerInfo);
        _sampler = sampler != nullptr ? sampler->getHandle() : VKM_INVALID_RESOURCE_HANDLE;

        VkmBuffer* lightBuffer = createUniformBuffer(driver, sizeof(LightConstants), "GiLightConstants");
        VkmBuffer* tonemapBuffer = createUniformBuffer(driver, sizeof(TonemapConstants), "GiTonemapConstants");
        VkmBuffer* compositeBuffer = createUniformBuffer(driver, sizeof(VkmGiCompositeConstants), "GiCompositeConstants");
        VkmBuffer* volumeBuffer = createUniformBuffer(driver, sizeof(VkmProbeVolumeConstants), "GiProbeVolumeConstants");
        if (lightBuffer == nullptr || tonemapBuffer == nullptr || compositeBuffer == nullptr || volumeBuffer == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create the GI sample's uniform buffers");
            return;
        }
        _lightBuffer = lightBuffer->getHandle();
        _tonemapBuffer = tonemapBuffer->getHandle();
        _compositeBuffer = compositeBuffer->getHandle();
        _volumeBuffer = volumeBuffer->getHandle();

        for (uint32_t frame = 0; frame < FRAME_BUFFER_COUNT; ++frame)
        {
            VkmStagingBufferInfo stagingInfo{};
            stagingInfo._flags = VkmResourceCreateInfo::AllowTransferSrc;
            stagingInfo._size = sizeof(VkmGiCompositeConstants);
            stagingInfo._debugName = "GiCompositeStaging";
            VkmStagingBuffer* staging = driver->newStagingBuffer(stagingInfo);
            if (staging == nullptr)
            {
                VKM_DEBUG_ERROR("Failed to create the GI sample's composite staging buffers");
                return;
            }
            _compositeStaging[frame] = staging->getHandle();
            _compositeStagingPointers[frame] = staging;
        }

        const LightConstants light{ glm::vec4(glm::normalize(kLightDirection), 0.0f), glm::vec4(3.0f, 3.0f, 2.8f, 0.0f) };
        driver->uploadToBuffer(_lightBuffer, &light, sizeof(light));
        const TonemapConstants tonemap{};
        driver->uploadToBuffer(_tonemapBuffer, &tonemap, sizeof(tonemap));

        if (gv_gi_debug_view.get() < static_cast<uint32_t>(VkmGiDebugView::Count))
        {
            _debugView = static_cast<VkmGiDebugView>(gv_gi_debug_view.get());
        }

        loadScene(gv_gi_model_path.get());
    }

    virtual void preShutdown() override final
    {
        _cameraController.unregister();
        if (_engine == nullptr)
        {
            return;
        }
        _engine->setActiveCamera(nullptr);
        destroyTables(_tables);
        for (RetiredTables& retired : _retiredTables)
        {
            destroyTables(retired._tables);
        }
        _retiredTables.clear();
        _updater.destroy();
        _volume.destroy();
        _gbuffer.destroy();
        _scene.destroy(_engine->getDriver());
    }

    virtual void update(const double deltaTime) override final
    {
        (void)deltaTime;
        if (_engine == nullptr || !_sceneReady)
        {
            return;
        }
        retireTables();
        ensureTargets();
        takePendingScreenshot();
        drawUi();
    }

    virtual void render(uint32_t windowIndex, VkmRenderGraph* renderGraph, VkmResourceHandle backBuffer) override final
    {
        if (windowIndex != 0 || _engine == nullptr)
        {
            return;
        }
        ++_frameCounter;

        if (!_sceneReady || !_tables.isComplete())
        {
            // Still give the frame a pass, so the swapchain image is written and presented.
            VkmFrameBufferDescriptor fb = makeFullscreenFb(_extent, backBuffer);
            fb._renderPass._colorAttachments[0]._loadAction = VkmLoadAction::Clear;
            renderGraph->beginGraphicsSubGraph(fb, "GiEmpty")->setRenderCallback([](VkmCommandBufferBase*) {});
            return;
        }

        std::vector<VkmResourceHandle> sceneResources;
        _scene.collectReferencedResources(&sceneResources);

        VkmFrameData frameData;
        frameData._lightDirection = glm::vec4(glm::normalize(kLightDirection), 0.0f);

        // 1. The probe refresh owns cull view 1 and records its own update, cull, capture and blend.
        _updater.record(renderGraph, &_scene, frameData);

        // 2. The camera's own update and cull, in view 0.
        const uint32_t frameIndex = renderGraph->frameIndex();
        VkmFrameData cameraFrameData = frameData;
        vkmExtractFrustumPlanes(_camera.getViewProjection(), cameraFrameData._frustumPlanes);

        VkmRenderTransferSubGraph* updateSubGraph = renderGraph->beginTransferSubGraph("GiSceneUpdate");
        referenceAll(updateSubGraph, sceneResources);
        updateSubGraph->addReferencedResource(_compositeBuffer);
        updateSubGraph->addReferencedResource(_compositeStaging[frameIndex]);
        updateSubGraph->setTransferCallback([this, frameIndex, cameraFrameData](VkmCommandBufferBase* commandBuffer) {
            _scene.recordUpdate(commandBuffer, frameIndex, cameraFrameData, kCameraCullView);
            // The composite's settings ride a buffer whose contents change per frame while the
            // table binding it stays immutable, which is how a per-pass table is meant to be used.
            // One staging region per frame slot, for the same reason VkmScene keeps one.
            VkmGiCompositeConstants composite{};
            composite._params = glm::vec4(_indirectIntensity, static_cast<float>(_debugView), 0.0f, 0.0f);
            _compositeStagingPointers[frameIndex]->writeDirect(0, &composite, sizeof(composite));
            commandBuffer->copyBuffer(_compositeStaging[frameIndex], _compositeBuffer, 0, 0, sizeof(composite));
        });

        VkmRenderComputeSubGraph* cullSubGraph = renderGraph->beginComputeSubGraph("GiSceneCull");
        referenceAll(cullSubGraph, sceneResources);
        cullSubGraph->setComputeCallback([this](VkmCommandBufferBase* commandBuffer) {
            _scene.recordCull(commandBuffer, kCameraCullView);
        });

        // 3. G-buffer.
        VkmRenderGraphicsSubGraph* gbufferSubGraph =
            renderGraph->beginGraphicsSubGraph(_gbuffer.makeFrameBufferDescriptor(), "GiGBuffer");
        referenceAll(gbufferSubGraph, sceneResources);
        for (uint32_t i = 0; i < VkmGBuffer::kTargetCount; ++i)
        {
            gbufferSubGraph->addReferencedResource(_gbuffer.getTexture(static_cast<VkmGBuffer::Target>(i)));
        }
        gbufferSubGraph->setRenderCallback([this](VkmCommandBufferBase* commandBuffer) {
            _scene.recordDrawBatches(
                commandBuffer,
                [this](const VkmScene::DrawBatch& batch) {
                    return _gbufferPipelines[static_cast<uint32_t>(batch._layout)];
                },
                {}, kCameraCullView);
        });

        // 4. Everything the fullscreen passes sample has to leave its attachment layout first.
        VkmRenderComputeSubGraph* barrierSubGraph = renderGraph->beginComputeSubGraph("GiGBufferToShaderRead");
        barrierSubGraph->setComputeCallback([this](VkmCommandBufferBase* commandBuffer) {
            for (uint32_t i = 0; i < VkmGBuffer::kTargetCount; ++i)
            {
                commandBuffer->barrierTextureForShaderRead(_gbuffer.getTexture(static_cast<VkmGBuffer::Target>(i)));
            }
            commandBuffer->barrierTextureForShaderRead(_volume.getIrradianceTexture());
            commandBuffer->barrierTextureForShaderRead(_volume.getDistanceTexture());
        });

        recordFullscreen(renderGraph, "GiDirectLighting", _directTarget, _lightingPipeline, _tables._lighting);
        recordFullscreen(renderGraph, "GiProbeLighting", _indirectTarget, _probeLightingPipeline, _tables._probeLighting);

        VkmRenderComputeSubGraph* lightingBarrier = renderGraph->beginComputeSubGraph("GiLightingToShaderRead");
        lightingBarrier->setComputeCallback([this](VkmCommandBufferBase* commandBuffer) {
            commandBuffer->barrierTextureForShaderRead(_directTarget);
            commandBuffer->barrierTextureForShaderRead(_indirectTarget);
        });

        recordFullscreen(renderGraph, "GiComposite", _compositeTarget, _compositePipeline, _tables._composite);

        VkmRenderComputeSubGraph* compositeBarrier = renderGraph->beginComputeSubGraph("GiCompositeToShaderRead");
        compositeBarrier->setComputeCallback([this](VkmCommandBufferBase* commandBuffer) {
            commandBuffer->barrierTextureForShaderRead(_compositeTarget);
        });

        // 5. Tone map into the backbuffer.
        VkmRenderGraphicsSubGraph* tonemapSubGraph =
            renderGraph->beginGraphicsSubGraph(makeFullscreenFb(_extent, backBuffer), "GiTonemap");
        tonemapSubGraph->addReferencedResource(_compositeTarget);
        VkmPipelineStateBase* tonemapPipeline = _tonemapPipeline;
        VkmPerPassResourceTableBase* tonemapTable = _tables._tonemap;
        tonemapSubGraph->setRenderCallback([tonemapPipeline, tonemapTable](VkmCommandBufferBase* commandBuffer) {
            commandBuffer->bindPipeline(tonemapPipeline);
            commandBuffer->bindPerPassResources(tonemapTable);
            commandBuffer->draw(3, 1, 0, 0);
        });

        // The backbuffer cannot be read back (Metal keeps framebufferOnly on the drawable), so a
        // screenshot frame tone-maps a second time into a target this sample owns. Only on that
        // one frame -- this is a debugging path, not a permanent second pass.
        if (_screenshotTarget.isValid() && _frameCounter == gv_gi_screenshot_frame.get())
        {
            VkmRenderGraphicsSubGraph* shotSubGraph =
                renderGraph->beginGraphicsSubGraph(makeFullscreenFb(_extent, _screenshotTarget), "GiScreenshot");
            shotSubGraph->addReferencedResource(_screenshotTarget);
            VkmPipelineStateBase* pipeline = _tonemapPipeline;
            VkmPerPassResourceTableBase* table = _tables._tonemap;
            shotSubGraph->setRenderCallback([pipeline, table](VkmCommandBufferBase* commandBuffer) {
                commandBuffer->bindPipeline(pipeline);
                commandBuffer->bindPerPassResources(table);
                commandBuffer->draw(3, 1, 0, 0);
            });
            _screenshotPending = true;
        }

        _gbuffer.advanceFrame();
    }

private:
    // The probe refresh takes the other one; see the file header.
    static constexpr uint32_t kCameraCullView = 0;
    static constexpr uint32_t kProbeCullView = 1;

    struct Tables
    {
        VkmPerPassResourceTableBase* _lighting = nullptr;
        VkmPerPassResourceTableBase* _probeLighting = nullptr;
        VkmPerPassResourceTableBase* _composite = nullptr;
        VkmPerPassResourceTableBase* _tonemap = nullptr;

        bool isComplete() const
        {
            return _lighting != nullptr && _probeLighting != nullptr && _composite != nullptr && _tonemap != nullptr;
        }
    };

    // A resize rebuilds every table, and a table must outlive the frames that bound it -- so the
    // old ones wait out FRAME_COUNT frames rather than being deleted under a running GPU.
    struct RetiredTables
    {
        uint64_t _retiredAtFrame = 0;
        Tables _tables;
    };

    static void referenceAll(VkmRenderSubGraph* subGraph, const std::vector<VkmResourceHandle>& handles)
    {
        for (VkmResourceHandle handle : handles)
        {
            subGraph->addReferencedResource(handle);
        }
    }

    void recordFullscreen(VkmRenderGraph* renderGraph, const char* name, VkmResourceHandle target,
                          VkmPipelineStateBase* pipeline, VkmPerPassResourceTableBase* table)
    {
        VkmRenderGraphicsSubGraph* subGraph =
            renderGraph->beginGraphicsSubGraph(makeFullscreenFb(_extent, target), name);
        subGraph->addReferencedResource(target);
        subGraph->setRenderCallback([pipeline, table](VkmCommandBufferBase* commandBuffer) {
            commandBuffer->bindPipeline(pipeline);
            commandBuffer->bindPerPassResources(table);
            commandBuffer->draw(3, 1, 0, 0);
        });
    }

    void destroyTables(Tables& tables)
    {
        for (VkmPerPassResourceTableBase** table :
             { &tables._lighting, &tables._probeLighting, &tables._composite, &tables._tonemap })
        {
            if (*table != nullptr)
            {
                (*table)->destroy();
                delete *table;
                *table = nullptr;
            }
        }
    }

    void retireTables()
    {
        for (size_t i = _retiredTables.size(); i-- > 0;)
        {
            if (_frameCounter >= _retiredTables[i]._retiredAtFrame + FRAME_BUFFER_COUNT)
            {
                destroyTables(_retiredTables[i]._tables);
                _retiredTables.erase(_retiredTables.begin() + static_cast<ptrdiff_t>(i));
            }
        }
    }

    void loadScene(const std::string& path)
    {
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec))
        {
            VKM_DEBUG_ERROR(("No scene at '" + path + "'; pass --gv_gi_model_path=<file.gltf>").c_str());
            return;
        }

        VkmDriverBase* driver = _engine->getDriver();
        VkmSceneModel model;
        std::string error;
        VkmGltfImportOptions importOptions;
        if (!importGltfModel(path, &model, &error, importOptions) || !_scene.addModel(model, &error) ||
            !_scene.build(driver, _engine->getPipelineStateManager(), &error))
        {
            VKM_DEBUG_ERROR(("Failed to load the GI scene: " + error).c_str());
            return;
        }

        // Fit the grid to what was loaded. A probe outside the geometry contributes nothing but
        // still costs a full capture, so the grid is sized from the scene rather than guessed.
        const VkmSceneAABB bounds = _scene.computeWorldBounds();
        const glm::vec3 center = bounds._valid ? bounds.getCenter() : glm::vec3(0.0f);
        const glm::vec3 extent = bounds._valid ? bounds.getExtent() : glm::vec3(8.0f);

        VkmProbeVolume::Descriptor volumeDescriptor{};
        volumeDescriptor._probeCounts = glm::uvec3(16u, 6u, 16u);
        // A margin outside the bounds, so surfaces at the very edge still sit between probes
        // rather than outside the grid, where the lookup returns black.
        const glm::vec3 span = extent * 1.2f;
        volumeDescriptor._spacing =
            glm::max(span / glm::vec3(volumeDescriptor._probeCounts - glm::uvec3(1u)), glm::vec3(0.05f));
        volumeDescriptor._origin =
            center - glm::vec3(volumeDescriptor._probeCounts - glm::uvec3(1u)) * volumeDescriptor._spacing * 0.5f;
        _cameraController.frame(center, glm::length(extent) * 0.5f);
        if (!_volume.initialize(driver, volumeDescriptor))
        {
            VKM_DEBUG_ERROR("Failed to create the GI probe volume");
            return;
        }

        // The capture pass pushes once per (probe, face, batch), and Metal/WebGPU hand out a
        // push-constant ring entry per push with no per-frame reset (1024 entries, FRAME_BUFFER_COUNT
        // frames in flight). A budget chosen without reference to the scene's batch count therefore
        // wraps the ring onto entries a running frame still references -- so derive it, and say so
        // when the scene is the thing limiting it rather than the knob.
        const uint32_t batchCount = static_cast<uint32_t>(_scene.getDrawBatches().size());
        const uint32_t ringBudget =
            kVkmPushConstantRingEntryCount /
            (FRAME_BUFFER_COUNT * std::max(1u, 6u * batchCount + 2u));
        _probeBudget = std::clamp(std::min(_probeBudget, ringBudget), 1u, VkmProbeVolumeUpdater::kMaxBudget);
        if (_probeBudget < VkmProbeVolumeUpdater::kMaxBudget)
        {
            VKM_DEBUG_INFO(("Probe budget limited to " + std::to_string(_probeBudget) +
                            " by the push-constant ring at " + std::to_string(batchCount) + " draw batches").c_str());
        }

        VkmProbeVolumeUpdater::Descriptor updaterDescriptor{};
        updaterDescriptor._cullViewIndex = kProbeCullView;
        updaterDescriptor._budget = _probeBudget;
        updaterDescriptor._hysteresis = _hysteresis;
        if (!_updater.initialize(driver, _engine->getPipelineStateManager(), &_volume, updaterDescriptor, &error))
        {
            VKM_DEBUG_ERROR(("Failed to create the probe updater: " + error).c_str());
            return;
        }

        const VkmProbeVolumeConstants volumeConstants = _volume.makeConstants();
        driver->uploadToBuffer(_volumeBuffer, &volumeConstants, sizeof(volumeConstants));

        _sceneReady = true;
    }

    // Recreates everything sized to the swapchain, and the tables that name those textures.
    void ensureTargets()
    {
        VkmDriverBase* driver = _engine->getDriver();
        const glm::uvec2 extent = _engine->getMainSwapChain()->getExtent();
        if (extent.x == 0 || extent.y == 0 || extent == _extent)
        {
            return;
        }
        _extent = extent;

        if (!_gbuffer.isValid() ? !_gbuffer.initialize(driver, extent) : !_gbuffer.resize(extent))
        {
            VKM_DEBUG_ERROR("Failed to size the GI sample's G-buffer");
            return;
        }

        VkmDeferredResourceReclaimer* reclaimer = driver->getDeferredReclaimer();
        for (VkmResourceHandle* target : { &_directTarget, &_indirectTarget, &_compositeTarget, &_screenshotTarget })
        {
            if (target->isValid())
            {
                reclaimer->requestRelease(*target);
                *target = VKM_INVALID_RESOURCE_HANDLE;
            }
        }
        VkmTexture* direct = createHdrTarget(driver, extent, "GiDirect");
        VkmTexture* indirect = createHdrTarget(driver, extent, "GiIndirect");
        VkmTexture* composite = createHdrTarget(driver, extent, "GiComposite");
        if (direct == nullptr || indirect == nullptr || composite == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create the GI sample's HDR targets");
            return;
        }
        _directTarget = direct->getHandle();
        _indirectTarget = indirect->getHandle();
        _compositeTarget = composite->getHandle();

        // Only when one was asked for: it is the same size as the backbuffer and would otherwise
        // cost a full swapchain-sized target for nothing. Swapchain format, because the tone map
        // PSO's attachment is declared as the swapchain's.
        if (!gv_gi_screenshot.get().empty())
        {
            VkmTextureInfo shotInfo{};
            shotInfo._flags = static_cast<VkmResourceCreateInfo>(
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowColorAttachment) |
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowTransferSrc));
            shotInfo._extent = glm::uvec3(extent, 1);
            shotInfo._numMipLevels = 1;
            shotInfo._numArrayLayers = 1;
            shotInfo._format = driver->getSwapChainColorFormat();
            shotInfo._debugName = "GiScreenshot";
            VkmTexture* shot = driver->newTexture(shotInfo);
            if (shot == nullptr)
            {
                VKM_DEBUG_ERROR("Failed to create the GI screenshot target");
                return;
            }
            _screenshotTarget = shot->getHandle();
        }

        if (_tables.isComplete())
        {
            _retiredTables.push_back(RetiredTables{ _frameCounter, _tables });
            _tables = Tables{};
        }
        buildTables();
    }

    void buildTables()
    {
        VkmDriverBase* driver = _engine->getDriver();
        const VkmResourceHandle normal = _gbuffer.getTexture(VkmGBuffer::Target::Normal);
        const VkmResourceHandle baseColor = _gbuffer.getTexture(VkmGBuffer::Target::BaseColorRoughness);
        const VkmResourceHandle motion = _gbuffer.getTexture(VkmGBuffer::Target::MotionMetallic);

        std::string error;
        _tables._lighting = driver->newPerPassResourceTable(
            _lightingPipeline,
            {{ 0, normal }, { 1, baseColor }, { 2, motion }, { 3, _sampler }, { 4, _lightBuffer }}, &error);
        _tables._probeLighting = driver->newPerPassResourceTable(
            _probeLightingPipeline,
            {{ 0, normal }, { 1, motion }, { 2, _volume.getIrradianceTexture() },
             { 3, _volume.getDistanceTexture() }, { 4, _sampler }, { 5, _volumeBuffer }}, &error);
        _tables._composite = driver->newPerPassResourceTable(
            _compositePipeline,
            {{ 0, _directTarget }, { 1, _indirectTarget }, { 2, baseColor }, { 3, normal },
             { 4, motion }, { 5, _sampler }, { 6, _compositeBuffer }}, &error);
        _tables._tonemap = driver->newPerPassResourceTable(
            _tonemapPipeline, {{ 0, _compositeTarget }, { 1, _sampler }, { 2, _tonemapBuffer }}, &error);

        if (!_tables.isComplete())
        {
            VKM_DEBUG_ERROR(("Failed to build the GI sample's per-pass tables: " + error).c_str());
        }
    }

    // Deliberately after the frame that rendered it: readbackTexture submits and waits, which is
    // not something to do in the middle of recording.
    void takePendingScreenshot()
    {
        if (!_screenshotPending)
        {
            return;
        }
        _screenshotPending = false;
        vkmWriteTexturePng(_engine->getDriver(), _screenshotTarget, gv_gi_screenshot.get());
        // A screenshot run exists to produce the file, so it stops once it has one.
        _engine->getInputHandler().requestExit();
    }

    void drawUi()
    {
#if defined(VKM_ENABLE_IMGUI)
        ImGui::Begin("GI");
        int view = static_cast<int>(_debugView);
        if (ImGui::BeginCombo("View", vkmGiDebugViewName(_debugView)))
        {
            for (int i = 0; i < static_cast<int>(VkmGiDebugView::Count); ++i)
            {
                const VkmGiDebugView candidate = static_cast<VkmGiDebugView>(i);
                if (ImGui::Selectable(vkmGiDebugViewName(candidate), view == i))
                {
                    _debugView = candidate;
                }
            }
            ImGui::EndCombo();
        }
        ImGui::DragFloat("Indirect intensity", &_indirectIntensity, 0.05f, 0.0f, 8.0f);

        ImGui::Separator();
        ImGui::Text("Probes: %u, budget %u/frame", _volume.getProbeCount(), _updater.getDescriptor()._budget);
        const uint32_t roundLength = _updater.getRoundLengthInFrames();
        ImGui::Text("Round: %u frames", roundLength);
        // The number Phase 4 exists to surface: this tier trades rays for amortized rasterization,
        // and convergence time is the bill.
        ImGui::Text("90%% convergence: %u frames",
                    VkmProbeVolumeUpdater::framesToConverge(_volume.getProbeCount(),
                                                            _updater.getDescriptor()._budget,
                                                            _updater.getDescriptor()._hysteresis, 0.1f));
        ImGui::End();
#endif
    }

    VkmEngine* _engine = nullptr;
    VkmCamera _camera;
    VkmOrbitCameraController _cameraController{ &_camera };

    VkmScene _scene;
    VkmGBuffer _gbuffer;
    VkmProbeVolume _volume;
    VkmProbeVolumeUpdater _updater;

    std::array<VkmPipelineStateBase*, static_cast<size_t>(VkmVertexLayoutPreset::Count)> _gbufferPipelines{};
    VkmPipelineStateBase* _lightingPipeline = nullptr;
    VkmPipelineStateBase* _probeLightingPipeline = nullptr;
    VkmPipelineStateBase* _compositePipeline = nullptr;
    VkmPipelineStateBase* _tonemapPipeline = nullptr;

    VkmResourceHandle _sampler{ VKM_INVALID_RESOURCE_HANDLE };
    VkmResourceHandle _lightBuffer{ VKM_INVALID_RESOURCE_HANDLE };
    VkmResourceHandle _tonemapBuffer{ VKM_INVALID_RESOURCE_HANDLE };
    VkmResourceHandle _compositeBuffer{ VKM_INVALID_RESOURCE_HANDLE };
    VkmResourceHandle _volumeBuffer{ VKM_INVALID_RESOURCE_HANDLE };
    VkmResourceHandle _directTarget{ VKM_INVALID_RESOURCE_HANDLE };
    VkmResourceHandle _indirectTarget{ VKM_INVALID_RESOURCE_HANDLE };
    VkmResourceHandle _compositeTarget{ VKM_INVALID_RESOURCE_HANDLE };
    VkmResourceHandle _screenshotTarget{ VKM_INVALID_RESOURCE_HANDLE };
    bool _screenshotPending = false;

    Tables _tables;
    std::vector<RetiredTables> _retiredTables;

    std::array<VkmResourceHandle, FRAME_BUFFER_COUNT> _compositeStaging{};
    std::array<VkmStagingBuffer*, FRAME_BUFFER_COUNT> _compositeStagingPointers{};

    glm::uvec2 _extent{ 0, 0 };
    uint64_t _frameCounter = 0;
    bool _sceneReady = false;

    VkmGiDebugView _debugView = VkmGiDebugView::Composite;
    float _indirectIntensity = 1.0f;
    uint32_t _probeBudget = 32u;
    float _hysteresis = 0.9f;
};

int main(int argc, char* argv[])
{
    VkmApplication app;

    int ret = app.entryPoint(new GiApplication(), argc, argv);
    app.destroy();

    return ret;
}
