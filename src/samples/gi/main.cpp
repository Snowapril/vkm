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
#include <glm/gtx/component_wise.hpp>
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
#include <vkm/renderer/backend/common/resource_table.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/sampler.h>
#include <vkm/renderer/backend/common/swapchain.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/backend/common/upscaler.h>
#include <vkm/renderer/camera.h>
#include <vkm/renderer/engine.h>
#include <vkm/renderer/gbuffer.h>
#include <vkm/renderer/gi_composite.h>
#include <vkm/renderer/gi_system.h>
#include <vkm/renderer/screenshot.h>

#include <cstdio>
#include <vkm/renderer/scene/gltf_importer.h>
#include <vkm/renderer/backend/common/staging_buffer.h>
#include <vkm/renderer/scene/scene.h>
#include <vkm/renderer/scene/scene_material_tables.h>

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
#include <memory>
#include <string>
#include <vector>

using namespace vkm;

#if defined(VKM_WASM_GI_SCENE_GLTF)
// A wasm build has no filesystem to browse and no command line, so the scene baked into MEMFS at
// link time is the default (see this sample's CMakeLists and -DVKM_WASM_GI_SCENE).
VKM_GLOBAL_VARIABLE(std::string, gv_gi_model_path, VKM_WASM_GI_SCENE_GLTF);
#else
VKM_GLOBAL_VARIABLE(std::string, gv_gi_model_path,
                    std::string(RESOURCES_DIR) + "Scenes/Sponza/Sponza.gltf");
#endif
// Write a PNG of the composed frame and quit. This is how the sample gets verified at all on a
// machine where nobody can look at the window -- see vkmWriteTexturePng.
#if defined(VKM_WASM_GI_AUTO_SCREENSHOT)
VKM_GLOBAL_VARIABLE(std::string, gv_gi_screenshot, "/vkm_gi.png");
#else
VKM_GLOBAL_VARIABLE(std::string, gv_gi_screenshot, "");
#endif
// Which frame to capture. Probes refresh a slice per frame and converge over rounds, so a
// screenshot taken too early shows a half-lit volume rather than the technique's actual output.
#if defined(VKM_WASM_GI_SCREENSHOT_FRAME)
VKM_GLOBAL_VARIABLE(uint32_t, gv_gi_screenshot_frame, VKM_WASM_GI_SCREENSHOT_FRAME);
#else
VKM_GLOBAL_VARIABLE(uint32_t, gv_gi_screenshot_frame, 600u);
#endif
// Which channel to show, as a VkmGiDebugView. Settable here as well as through the UI so a
// screenshot run can capture a specific term without anyone touching the window.
VKM_GLOBAL_VARIABLE(uint32_t, gv_gi_debug_view, 0u);
// Off switch for the contact term, so a screenshot run can A/B it against the probes alone.
VKM_GLOBAL_VARIABLE(bool, gv_gi_ssgi, true);
// Probes per axis. More is better lit and slower to converge, a round being probeCount/budget
// frames. Y is lower than X and Z because scenes are wider than they are tall.
VKM_GLOBAL_VARIABLE(uint32_t, gv_gi_probes_x, 20u);
VKM_GLOBAL_VARIABLE(uint32_t, gv_gi_probes_y, 10u);
VKM_GLOBAL_VARIABLE(uint32_t, gv_gi_probes_z, 20u);
// Fraction of the scene's radius to orbit at. Below 1 the camera starts inside the geometry, which
// is where a probe volume is actually judged -- an exterior view can look right while the interior
// is black, which is exactly how the scene-scale bug got past a screenshot.
VKM_GLOBAL_VARIABLE(float, gv_gi_camera_distance, 1.0f);
// Which GI technique fills the indirect target: 0 = probe volume (+SSGI), 1 = ReSTIR. Clamped to
// 0 on a device without ray tracing -- the selection is a runtime capability question, not a
// build-time one (restir.md section 5).
VKM_GLOBAL_VARIABLE(uint32_t, gv_gi_technique, 0u);
// 8.7's final-shading MIS blend: smooth surfaces lean on the pixel's own fresh sample instead of
// the resampled one, whose cached radiance is the documented ReSTIR GI bias on low roughness.
// Settable here so a screenshot run can A/B it.
VKM_GLOBAL_VARIABLE(bool, gv_gi_restir_mis, true);
// Render resolution as a fraction of the swapchain's, clamped to [0.25, 1]. Below 1 the scene
// renders at the reduced extent and the tonemap pass upscales into the backbuffer -- bilinearly
// until a temporal upscaler takes that spot.
VKM_GLOBAL_VARIABLE(float, gv_gi_render_scale, 1.0f);

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
        _compositePipeline = manager->getPipelineState("gi_composite_pso", VkmPipelineStateOrigin::Engine);
        _tonemapPipeline = manager->getPipelineState("tonemap_pso", VkmPipelineStateOrigin::Engine);
        if (_lightingPipeline == nullptr || _compositePipeline == nullptr || _tonemapPipeline == nullptr)
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
        if (lightBuffer == nullptr || tonemapBuffer == nullptr || compositeBuffer == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create the GI sample's uniform buffers");
            return;
        }
        _lightBuffer = lightBuffer->getHandle();
        _tonemapBuffer = tonemapBuffer->getHandle();
        _compositeBuffer = compositeBuffer->getHandle();

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
        // Uploaded once the scene's scale is known; see loadScene.

        if (gv_gi_debug_view.get() < static_cast<uint32_t>(VkmGiDebugView::Count))
        {
            _debugView = static_cast<VkmGiDebugView>(gv_gi_debug_view.get());
        }

        // The engine owns both GI techniques and the switch between them; the sample only maps
        // its command-line surface onto the system's descriptor and options.
        VkmGiSystemDescriptor giDescriptor{};
        giDescriptor._probeCounts =
            glm::uvec3(gv_gi_probes_x.get(), gv_gi_probes_y.get(), gv_gi_probes_z.get());
        giDescriptor._probeBudget = 32;
        giDescriptor._probeHysteresis = 0.9f;
        giDescriptor._probeCullView = kProbeCullView;
        std::string giError;
        if (!_gi.initialize(driver, manager, &_gbuffer, giDescriptor, &giError))
        {
            VKM_DEBUG_ERROR(("Failed to create the GI system: " + giError).c_str());
            return;
        }
        _gi.options()._technique = static_cast<VkmGiTechnique>(gv_gi_technique.get() != 0 ? 1 : 0);
        _gi.options()._ssgi = gv_gi_ssgi.get();
        _gi.options()._restirMisBlend = gv_gi_restir_mis.get();

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
        destroyUpscaler(_upscaler);
        _upscaler = nullptr;
        for (RetiredUpscaler& retired : _retiredUpscalers)
        {
            destroyUpscaler(retired._upscaler);
        }
        _retiredUpscalers.clear();
        _gi.destroy();
        _gbuffer.destroy();
        for (VkmSceneMaterialTables& tables : _gbufferMaterialTables)
        {
            tables.destroy(_engine->getDriver());
        }
        _scene.destroy(_engine->getDriver());
    }

    virtual void update(const double deltaTime) override final
    {
        if (_engine == nullptr || !_sceneReady)
        {
            return;
        }
        // The technique's own capability gate lives in VkmGiSystem now, clamped at record().
        _lastDeltaTime = static_cast<float>(deltaTime);
        retireTables();
        ensureTargets();
        // Runs after ensureTargets so the phase count follows a resize immediately. The camera
        // must carry this frame's jitter before the engine publishes set 1 (update precedes
        // render in the engine loop), and exactly the same value rides the upscale dispatch.
        if (_upscaler != nullptr)
        {
            const uint32_t phaseCount = vkmUpscaleJitterPhaseCount(_renderExtent.x, _extent.x);
            _camera.setJitterPixels(vkmUpscaleJitterPixels(_frameCounter, phaseCount));
        }
        else
        {
            _camera.setJitterPixels(glm::vec2(0.0f));
        }
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

        VkmFrameData frameData;
        frameData._lightDirection = glm::vec4(glm::normalize(kLightDirection), 0.0f);

        // 1. The camera's own update and cull, in view 0. (The GI system records its own
        // subgraphs — including the probe refresh's second cull view — from record() below.)
        const uint32_t frameIndex = renderGraph->frameIndex();
        VkmFrameData cameraFrameData = frameData;
        vkmExtractFrustumPlanes(_camera.getViewProjection(), cameraFrameData._frustumPlanes);

        VkmRenderTransferSubGraph* updateSubGraph = renderGraph->beginTransferSubGraph("GiSceneUpdate");
        referenceScene(updateSubGraph, _scene, VkmScene::ReferencePhase::Update);
        updateSubGraph->addReferencedResource(_compositeBuffer, VkmResourceAccess::TransferWrite);
        updateSubGraph->addReferencedResource(_compositeStaging[frameIndex], VkmResourceAccess::TransferRead);
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
        referenceScene(cullSubGraph, _scene, VkmScene::ReferencePhase::Cull);
        cullSubGraph->setComputeCallback([this](VkmCommandBufferBase* commandBuffer) {
            _scene.recordCull(commandBuffer, kCameraCullView);
        });

        // 3. G-buffer.
        VkmRenderGraphicsSubGraph* gbufferSubGraph =
            renderGraph->beginGraphicsSubGraph(_gbuffer.makeFrameBufferDescriptor(), "GiGBuffer");
        referenceScene(gbufferSubGraph, _scene, VkmScene::ReferencePhase::Draw);
        for (uint32_t i = 0; i < VkmGBuffer::kTargetCount; ++i)
        {
            gbufferSubGraph->addReferencedResource(_gbuffer.getTexture(static_cast<VkmGBuffer::Target>(i)),
                                                  VkmResourceAccess::ColorAttachmentWrite);
        }
        gbufferSubGraph->setRenderCallback([this](VkmCommandBufferBase* commandBuffer) {
            _scene.recordDrawBatches(
                commandBuffer,
                [this](const VkmScene::DrawBatch& batch) {
                    return _gbufferPipelines[static_cast<uint32_t>(batch._layout)];
                },
                // Binds this batch's material at set 3 where the backend needs one (WebGPU); a
                // no-op where the shader indexes the bindless array instead. A batch carries
                // exactly one material, which is why VkmScene splits them that way.
                [this](VkmCommandBufferBase* cb, const VkmScene::DrawBatch& batch) {
                    _gbufferMaterialTables[static_cast<uint32_t>(batch._layout)].bind(cb, batch._materialIndex);
                },
                kCameraCullView);
        });

        // 4. The fullscreen passes, each declaring what it samples so the graph hands the
        // attachments over.
        recordFullscreen(renderGraph, "GiDirectLighting", _directTarget, _lightingPipeline, _tables._lighting);

        // The engine's GI fills its indirect target with whichever technique is selected; the
        // composite below consumes it without knowing which (restir.md section 5).
        _gi.record(renderGraph, frameData, frameIndex);

        recordFullscreen(renderGraph, "GiComposite", _compositeTarget, _compositePipeline, _tables._composite);

        // 4.5 Temporal upscale of the HDR composite to the display extent, when a backend
        // upscaler exists. The tonemap table then samples _upscaledTarget instead (buildTables).
        if (_upscaler != nullptr)
        {
            VkmUpscalerDispatchDesc upscaleDesc{};
            upscaleDesc._color = _compositeTarget;
            upscaleDesc._depth = _gbuffer.getDepthTexture();
            upscaleDesc._motion = _gbuffer.getTexture(VkmGBuffer::Target::MotionMetallic);
            upscaleDesc._output = _upscaledTarget;
            upscaleDesc._jitterPixels = _camera.getJitterPixels();
            upscaleDesc._deltaTimeSeconds = _lastDeltaTime;
            upscaleDesc._reset = _upscalerResetPending;
            upscaleDesc._nearZ = _camera.getNearZ();
            upscaleDesc._farZ = _camera.getFarZ();
            upscaleDesc._fovYRadians = _camera.getFovYRadians();
            _upscalerResetPending = false;
            _upscaler->recordDispatch(renderGraph, upscaleDesc);
        }

        // 5. Tone map into the backbuffer.
        VkmRenderGraphicsSubGraph* tonemapSubGraph =
            renderGraph->beginGraphicsSubGraph(makeFullscreenFb(_extent, backBuffer), "GiTonemap");
        std::vector<VkmResourceAccessDeclaration> tonemapBound;
        _tables._tonemap->collectReferencedResources(&tonemapBound);
        tonemapSubGraph->addReferencedResources(tonemapBound);
        VkmPipelineStateBase* tonemapPipeline = _tonemapPipeline;
        VkmResourceTableBase* tonemapTable = _tables._tonemap;
        tonemapSubGraph->setRenderCallback([tonemapPipeline, tonemapTable](VkmCommandBufferBase* commandBuffer) {
            commandBuffer->bindPipeline(tonemapPipeline);
            commandBuffer->bindResourceTable(tonemapTable);
            commandBuffer->draw(3, 1, 0, 0);
        });

        // The backbuffer cannot be read back (Metal keeps framebufferOnly on the drawable), so a
        // screenshot frame tone-maps a second time into a target this sample owns. Only on that
        // one frame -- this is a debugging path, not a permanent second pass.
        if (_screenshotTarget.isValid() && _frameCounter == gv_gi_screenshot_frame.get())
        {
            VkmRenderGraphicsSubGraph* shotSubGraph =
                renderGraph->beginGraphicsSubGraph(makeFullscreenFb(_extent, _screenshotTarget), "GiScreenshot");
            shotSubGraph->addReferencedResource(_screenshotTarget, VkmResourceAccess::ColorAttachmentWrite);
            VkmPipelineStateBase* pipeline = _tonemapPipeline;
            VkmResourceTableBase* table = _tables._tonemap;
            shotSubGraph->setRenderCallback([pipeline, table](VkmCommandBufferBase* commandBuffer) {
                commandBuffer->bindPipeline(pipeline);
                commandBuffer->bindResourceTable(table);
                commandBuffer->draw(3, 1, 0, 0);
            });
            _screenshotPending = true;
        }

        _gbuffer.advanceFrame();
        _gi.advanceFrame();
    }

private:
    // The probe refresh takes the other one; see the file header.
    static constexpr uint32_t kCameraCullView = 0;
    static constexpr uint32_t kProbeCullView = 1;

    struct Tables
    {
        VkmResourceTableBase* _lighting = nullptr;
        VkmResourceTableBase* _composite = nullptr;
        VkmResourceTableBase* _tonemap = nullptr;

        bool isComplete() const
        {
            return _lighting != nullptr && _composite != nullptr && _tonemap != nullptr;
        }
    };

    // A resize rebuilds every table, and a table must outlive the frames that bound it -- so the
    // old ones wait out FRAME_COUNT frames rather than being deleted under a running GPU.
    struct RetiredTables
    {
        uint64_t _retiredAtFrame = 0;
        Tables _tables;
    };

    // Same delay for a retired upscaler: its backend object is referenced by in-flight command
    // buffers exactly like a table's descriptors.
    struct RetiredUpscaler
    {
        uint64_t _retiredAtFrame = 0;
        VkmUpscalerBase* _upscaler = nullptr;
    };

    static void referenceScene(VkmRenderSubGraph* subGraph, const VkmScene& scene,
                               VkmScene::ReferencePhase phase)
    {
        std::vector<VkmResourceAccessDeclaration> declarations;
        scene.collectReferencedResources(phase, &declarations);
        subGraph->addReferencedResources(declarations);
    }

    /*
    * @brief A fullscreen pass writing `target`, reading whatever its resource table binds.
    *
    * The table declares itself: it is the one place a pass's resources are named without the
    * render graph seeing them, so asking it is what keeps the sampled list and the bound list from
    * drifting apart.
    */
    void recordFullscreen(VkmRenderGraph* renderGraph, const char* name, VkmResourceHandle target,
                          VkmPipelineStateBase* pipeline, VkmResourceTableBase* table)
    {
        VkmRenderGraphicsSubGraph* subGraph =
            renderGraph->beginGraphicsSubGraph(makeFullscreenFb(_renderExtent, target), name);
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

    void destroyTables(Tables& tables)
    {
        for (VkmResourceTableBase** table : { &tables._lighting, &tables._composite, &tables._tonemap })
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
        for (size_t i = _retiredUpscalers.size(); i-- > 0;)
        {
            if (_frameCounter >= _retiredUpscalers[i]._retiredAtFrame + FRAME_BUFFER_COUNT)
            {
                destroyUpscaler(_retiredUpscalers[i]._upscaler);
                _retiredUpscalers.erase(_retiredUpscalers.begin() + static_cast<ptrdiff_t>(i));
            }
        }
    }

    static void destroyUpscaler(VkmUpscalerBase* upscaler)
    {
        if (upscaler != nullptr)
        {
            upscaler->destroy();
            delete upscaler;
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

        // Must follow build(), which is where the material textures are created.
        for (uint32_t i = 0; i < static_cast<uint32_t>(VkmVertexLayoutPreset::Count); ++i)
        {
            if (_gbufferPipelines[i] != nullptr &&
                !_gbufferMaterialTables[i].initialize(driver, _scene, _gbufferPipelines[i], &error))
            {
                VKM_DEBUG_ERROR(("Failed to build the GI sample's material tables: " + error).c_str());
                return;
            }
        }

        // The GI system fits its probe grid to these bounds; the sample only frames the camera.
        const VkmSceneAABB bounds = _scene.computeWorldBounds();
        const glm::vec3 center = bounds._valid ? bounds.getCenter() : glm::vec3(0.0f);
        const glm::vec3 extent = bounds._valid ? bounds.getExtent() : glm::vec3(8.0f);
        _cameraController.frame(center, glm::length(extent) * 0.5f * gv_gi_camera_distance.get());

        if (!_gi.prepareScene(&_scene, &error))
        {
            VKM_DEBUG_ERROR(("Failed to prepare the GI system's scene: " + error).c_str());
            return;
        }

        _sceneReady = true;
    }

    // Recreates everything sized to the swapchain (scene targets at the scaled render extent),
    // and the tables that name those textures.
    void ensureTargets()
    {
        VkmDriverBase* driver = _engine->getDriver();
        const glm::uvec2 extent = _engine->getMainSwapChain()->getExtent();
        if (extent.x == 0 || extent.y == 0)
        {
            return;
        }
        const float renderScale = std::clamp(gv_gi_render_scale.get(), 0.25f, 1.0f);
        const glm::uvec2 renderExtent =
            glm::max(glm::uvec2(glm::vec2(extent) * renderScale + 0.5f), glm::uvec2(1u));
        if (extent == _extent && renderExtent == _renderExtent)
        {
            return;
        }
        _extent = extent;
        _renderExtent = renderExtent;
        // At scale 1 the override is released so the camera follows the swapchain again.
        _engine->setCameraViewportOverride(renderExtent == extent ? glm::uvec2(0u) : renderExtent);

        if (!_gbuffer.isValid() ? !_gbuffer.initialize(driver, renderExtent) : !_gbuffer.resize(renderExtent))
        {
            VKM_DEBUG_ERROR("Failed to size the GI sample's G-buffer");
            return;
        }

        VkmDeferredResourceReclaimer* reclaimer = driver->getDeferredReclaimer();
        // No _indirectTarget here: the GI system owns it and sizes it from resize() below.
        for (VkmResourceHandle* target :
             { &_directTarget, &_compositeTarget, &_upscaledTarget, &_screenshotTarget })
        {
            if (target->isValid())
            {
                reclaimer->requestRelease(*target);
                *target = VKM_INVALID_RESOURCE_HANDLE;
            }
        }
        VkmTexture* direct = createHdrTarget(driver, renderExtent, "GiDirect");
        VkmTexture* composite = createHdrTarget(driver, renderExtent, "GiComposite");
        if (direct == nullptr || composite == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create the GI sample's HDR targets");
            return;
        }
        _directTarget = direct->getHandle();
        _compositeTarget = composite->getHandle();

        // The GI system sizes its indirect target and passes with the G-buffer, so it takes the
        // render extent rather than the swapchain's; the composite table below binds whatever
        // handle it now reports.
        std::string giError;
        if (!_gi.resize(renderExtent, _directTarget, &giError))
        {
            VKM_DEBUG_ERROR(("Failed to size the GI system: " + giError).c_str());
            return;
        }

        // The upscaler's extents are fixed at creation, so a resize retires it like the tables:
        // it must outlive the frames whose command buffers still reference its internal state.
        if (_upscaler != nullptr)
        {
            _retiredUpscalers.push_back(RetiredUpscaler{ _frameCounter, _upscaler });
            _upscaler = nullptr;
        }
        if (renderExtent != extent &&
            (driver->getDriverCapabilityFlags() & VkmDriverCapabilityFlags::TemporalUpscaling) != 0)
        {
            // ColorAttachment as well: MetalFX publishes render-target usage as part of its
            // output-texture requirements (outputTextureUsage), not just shader write.
            VkmTextureInfo upscaledInfo{};
            upscaledInfo._flags = static_cast<VkmResourceCreateInfo>(
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowShaderRead) |
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowShaderWrite) |
                static_cast<uint32_t>(VkmResourceCreateInfo::AllowColorAttachment));
            upscaledInfo._extent = glm::uvec3(extent, 1);
            upscaledInfo._numMipLevels = 1;
            upscaledInfo._numArrayLayers = 1;
            upscaledInfo._format = kHdrFormat;
            upscaledInfo._debugName = "GiUpscaled";
            VkmTexture* upscaled = driver->newTexture(upscaledInfo);
            if (upscaled != nullptr)
            {
                _upscaledTarget = upscaled->getHandle();
                VkmUpscalerDescriptor upscalerDesc{};
                upscalerDesc._renderExtent = renderExtent;
                upscalerDesc._displayExtent = extent;
                upscalerDesc._colorFormat = kHdrFormat;
                upscalerDesc._depthFormat = VkmGBuffer::getDepthFormat();
                upscalerDesc._motionFormat = kHdrFormat;
                upscalerDesc._outputFormat = kHdrFormat;
                upscalerDesc._debugName = "GiTemporalUpscale";
                _upscaler = driver->newUpscaler(upscalerDesc);
                _upscalerResetPending = true;
            }
            if (_upscaler == nullptr)
            {
                // Bilinear fallback: the tonemap samples the render-extent composite instead.
                VKM_DEBUG_ERROR("Failed to create the GI temporal upscaler; falling back to bilinear");
                if (_upscaledTarget.isValid())
                {
                    reclaimer->requestRelease(_upscaledTarget);
                    _upscaledTarget = VKM_INVALID_RESOURCE_HANDLE;
                }
            }
        }

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
        _tables._lighting = driver->newResourceTable(
            _lightingPipeline, VkmResourceSetKind::PerPass,
            {{ 0, normal }, { 1, baseColor }, { 2, motion }, { 3, _sampler }, { 4, _lightBuffer },
             { 5, _gbuffer.getTexture(VkmGBuffer::Target::Emissive) }}, &error);
        _tables._composite = driver->newResourceTable(
            _compositePipeline, VkmResourceSetKind::PerPass,
            {{ 0, _directTarget }, { 1, _gi.getIndirectTexture() }, { 2, baseColor }, { 3, normal },
             { 4, motion }, { 5, _sampler }, { 6, _compositeBuffer },
             { 7, _gbuffer.getTexture(VkmGBuffer::Target::Emissive) }}, &error);
        // With an upscaler the tonemap reads the display-extent upscale; without one it samples
        // the render-extent composite directly (bilinear when the extents differ).
        const VkmResourceHandle tonemapSource =
            _upscaler != nullptr ? _upscaledTarget : _compositeTarget;
        _tables._tonemap = driver->newResourceTable(
            _tonemapPipeline, VkmResourceSetKind::PerPass, {{ 0, tonemapSource }, { 1, _sampler }, { 2, _tonemapBuffer }}, &error);

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
#if defined(__EMSCRIPTEN__)
        // A wasm build writes into MEMFS, which nothing outside the page can open, so the file is
        // echoed to the console as base64 for a headless run to recover. This is how the WebGPU
        // render path gets *looked at* rather than only built and unit-tested; Chrome's own
        // --screenshot fires before the device has finished initializing and captures a blank page.
        dumpScreenshotAsBase64(gv_gi_screenshot.get());
#endif
        // A screenshot run exists to produce the file, so it stops once it has one.
        _engine->getInputHandler().requestExit();
    }


#if defined(__EMSCRIPTEN__)
    // Prints "VKM_SCREENSHOT_BASE64:<data>" on one line. Verification scaffolding, wasm only.
    static void dumpScreenshotAsBase64(const std::string& path)
    {
        std::FILE* file = std::fopen(path.c_str(), "rb");
        if (file == nullptr)
        {
            VKM_DEBUG_ERROR("screenshot base64 dump: could not reopen the PNG from MEMFS");
            return;
        }
        std::vector<unsigned char> bytes;
        unsigned char chunk[4096];
        size_t read = 0;
        while ((read = std::fread(chunk, 1, sizeof(chunk), file)) > 0)
        {
            bytes.insert(bytes.end(), chunk, chunk + read);
        }
        std::fclose(file);

        static const char* kAlphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string encoded;
        encoded.reserve(((bytes.size() + 2) / 3) * 4);
        for (size_t i = 0; i < bytes.size(); i += 3)
        {
            const uint32_t remaining = static_cast<uint32_t>(bytes.size() - i);
            const uint32_t triple = (static_cast<uint32_t>(bytes[i]) << 16) |
                                    ((remaining > 1 ? static_cast<uint32_t>(bytes[i + 1]) : 0u) << 8) |
                                    (remaining > 2 ? static_cast<uint32_t>(bytes[i + 2]) : 0u);
            encoded.push_back(kAlphabet[(triple >> 18) & 0x3F]);
            encoded.push_back(kAlphabet[(triple >> 12) & 0x3F]);
            encoded.push_back(remaining > 1 ? kAlphabet[(triple >> 6) & 0x3F] : '=');
            encoded.push_back(remaining > 2 ? kAlphabet[triple & 0x3F] : '=');
        }
        std::printf("VKM_SCREENSHOT_BASE64:%s\n", encoded.c_str());
        std::fflush(stdout);
    }
#endif

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
        VkmGiOptions& giOptions = _gi.options();
        ImGui::Checkbox("SSGI contact term", &giOptions._ssgi);

        if (_gi.isRestirAvailable())
        {
            ImGui::Separator();
            static const char* kTechniqueNames[] = { "Probe volume", "ReSTIR" };
            int technique = static_cast<int>(giOptions._technique);
            if (ImGui::Combo("Technique", &technique, kTechniqueNames, 2))
            {
                giOptions._technique = static_cast<VkmGiTechnique>(technique);
            }
            if (giOptions._technique == VkmGiTechnique::Restir)
            {
                ImGui::DragFloat("Environment radiance", &giOptions._environmentRadiance, 0.05f, 0.0f, 8.0f);
                ImGui::Checkbox("Roughness MIS blend", &giOptions._restirMisBlend);
                static const char* kRestirViewNames[] = { "Lighting", "Confidence (M)", "Age", "Weight" };
                int restirView = static_cast<int>(giOptions._restirDebugView);
                if (ImGui::Combo("ReSTIR view", &restirView, kRestirViewNames, 4))
                {
                    giOptions._restirDebugView = static_cast<VkmRestirDebugView>(restirView);
                }
            }
        }

        ImGui::Separator();
        ImGui::Text("Render: %ux%u -> %ux%u (%s)", _renderExtent.x, _renderExtent.y,
                    _extent.x, _extent.y,
                    _upscaler != nullptr ? "temporal upscale"
                                         : (_renderExtent == _extent ? "native" : "bilinear"));

        ImGui::Separator();
        const VkmProbeVolume& volume = _gi.getProbeVolume();
        const VkmProbeVolumeUpdater& updater = _gi.getProbeUpdater();
        ImGui::Text("Probes: %u, budget %u/frame", volume.getProbeCount(), updater.getDescriptor()._budget);
        ImGui::Text("Round: %u frames", updater.getRoundLengthInFrames());
        // The number Phase 4 exists to surface: this tier trades rays for amortized rasterization,
        // and convergence time is the bill.
        ImGui::Text("90%% convergence: %u frames",
                    VkmProbeVolumeUpdater::framesToConverge(volume.getProbeCount(),
                                                            updater.getDescriptor()._budget,
                                                            updater.getDescriptor()._hysteresis, 0.1f));
        ImGui::End();
#endif
    }

    VkmEngine* _engine = nullptr;
    VkmCamera _camera;
    VkmOrbitCameraController _cameraController{ &_camera };

    VkmScene _scene;
    VkmGBuffer _gbuffer;
    VkmGiSystem _gi;

    std::array<VkmPipelineStateBase*, static_cast<size_t>(VkmVertexLayoutPreset::Count)> _gbufferPipelines{};
    // One set-3 table per material, per G-buffer permutation. Empty on a backend whose shader
    // samples materials through the bindless array; see VkmSceneMaterialTables.
    std::array<VkmSceneMaterialTables, static_cast<size_t>(VkmVertexLayoutPreset::Count)> _gbufferMaterialTables{};
    VkmPipelineStateBase* _lightingPipeline = nullptr;
    VkmPipelineStateBase* _compositePipeline = nullptr;
    VkmPipelineStateBase* _tonemapPipeline = nullptr;

    VkmResourceHandle _sampler{ VKM_INVALID_RESOURCE_HANDLE };
    VkmResourceHandle _lightBuffer{ VKM_INVALID_RESOURCE_HANDLE };
    VkmResourceHandle _tonemapBuffer{ VKM_INVALID_RESOURCE_HANDLE };
    VkmResourceHandle _compositeBuffer{ VKM_INVALID_RESOURCE_HANDLE };
    VkmResourceHandle _directTarget{ VKM_INVALID_RESOURCE_HANDLE };
    VkmResourceHandle _compositeTarget{ VKM_INVALID_RESOURCE_HANDLE };
    VkmResourceHandle _upscaledTarget{ VKM_INVALID_RESOURCE_HANDLE };
    VkmResourceHandle _screenshotTarget{ VKM_INVALID_RESOURCE_HANDLE };
    bool _screenshotPending = false;

    VkmUpscalerBase* _upscaler = nullptr;
    std::vector<RetiredUpscaler> _retiredUpscalers;
    bool _upscalerResetPending = false;
    float _lastDeltaTime = 0.0f;

    Tables _tables;
    std::vector<RetiredTables> _retiredTables;

    std::array<VkmResourceHandle, FRAME_BUFFER_COUNT> _compositeStaging{};
    std::array<VkmStagingBuffer*, FRAME_BUFFER_COUNT> _compositeStagingPointers{};

    glm::uvec2 _extent{ 0, 0 };       // display: the swapchain's extent
    glm::uvec2 _renderExtent{ 0, 0 }; // scene targets: _extent scaled by gv_gi_render_scale
    uint64_t _frameCounter = 0;
    bool _sceneReady = false;

    VkmGiDebugView _debugView = VkmGiDebugView::Composite;
    float _indirectIntensity = 1.0f;
};

int main(int argc, char* argv[])
{
    VkmApplication app;

    int ret = app.entryPoint(new GiApplication(), argc, argv);
    app.destroy();

    return ret;
}
