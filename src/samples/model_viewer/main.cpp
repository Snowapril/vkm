#include <cxxopts.hpp>

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>

#if defined(VKM_ENABLE_IMGUI)
#include <imgui.h>
#include <ImGuizmo.h>
#endif

#include <common/sample_scene_browser.h>

#include <vkm/base/common.h>
#include <vkm/base/global_variable.h>
#include <vkm/platform/common/app_delegate.h>
#include <vkm/platform/common/input_codes.h>
#include <vkm/platform/common/input_handler.h>
#include <vkm/renderer/backend/common/bindless_resource_manager.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/command_queue.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/swapchain.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/camera.h>
#include <vkm/renderer/engine.h>
#include <vkm/renderer/memory_report.h>
#include <vkm/renderer/scene/gltf_importer.h>
#include <vkm/renderer/scene/scene.h>
#include <vkm/renderer/scene/scene_model.h>

#if defined(VKM_PLATFORM_WINDOWS)
#include <vkm/platform/windows/application.h>
#elif defined(VKM_PLATFORM_WASM)
#include <vkm/platform/wasm/application.h>
#elif defined(VKM_PLATFORM_LINUX)
#include <vkm/platform/linux/application.h>
#else // defined(VKM_PLATFORM_WINDOWS)
#include <vkm/platform/apple/application.h>
#endif // defined(VKM_PLATFORM_WINDOWS)

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

using namespace vkm;

// Which glTF(s) to open at startup, comma-separated:
//   ./model_viewer --gv_model_path=/path/one.gltf,/path/two.gltf
// Every listed file is loaded into one scene at the transform its own nodes give it; the Scene
// Browser's gizmo is how two assets authored around the origin get separated. Defaults to the
// sphere committed under resources/Scenes/, which is present without running download_scenes.py.
VKM_GLOBAL_VARIABLE(std::string, gv_model_path,
                    std::string(RESOURCES_DIR) + "Scenes/EmissiveSphere/EmissiveSphere.gltf");

namespace
{
    constexpr VkmFormat kDepthFormat = VkmFormat::D32_SFLOAT;
    constexpr glm::vec3 kWorldLightDirection{ 0.4f, 0.8f, 0.45f }; // towards the light

    // What the pixel shader draws instead of shading. Mirrors the VKM_DEBUG_MODE_* constants in
    // model_viewer.hlsl; travels there as VkmFrameData::_debugMode.
    enum class DebugMode : uint32_t
    {
        Lit = 0,
        BaseColor = 1,
        MaterialIndex = 2,
        Normal = 3,
        TangentNormal = 4,
        StreamingMip = 5,
        Count = 6,
    };

    constexpr size_t kDebugModeCount = static_cast<size_t>(DebugMode::Count);

    // Both indexed by DebugMode, so the sized declarations fail to compile if one drifts.
    constexpr const char* kDebugModeNames[kDebugModeCount] = {
        "Lit", "Base color", "Material index", "World normal", "TBN normal", "Streaming mip",
    };
    // Digits, so nothing collides with the fly camera's WASD/QE/Shift set.
    constexpr VkmKeyCode kDebugModeKeys[kDebugModeCount] = {
        VkmKeyCode::Num0, VkmKeyCode::Num1, VkmKeyCode::Num2, VkmKeyCode::Num3, VkmKeyCode::Num4,
        VkmKeyCode::Num5,
    };

    // Points the orbit controller at a scene's bounds; an empty/invalid AABB falls back to a
    // unit sphere at the origin so the camera still ends up somewhere sensible.
    void frameCameraOnBounds(VkmOrbitCameraController& controller, const VkmSceneAABB& bounds)
    {
        const glm::vec3 center = bounds._valid ? bounds.getCenter() : glm::vec3(0.0f);
        const float radius = bounds._valid ? glm::length(bounds.getExtent()) * 0.5f : 1.0f;
        controller.frame(center, radius);
    }

#if defined(VKM_ENABLE_IMGUI)
    /*
    * What streaming currently holds, against what the same textures would cost unstreamed.
    *
    * The baseline is the point. A resident figure on its own says nothing about whether streaming
    * is doing anything, which is exactly the question this panel exists to answer.
    */
    void drawTextureStreamingStats(const VkmTextureStreamingStats& stats)
    {
        if (stats._textureCount == 0)
        {
            ImGui::TextDisabled("No streamed textures in this scene");
            return;
        }

        const uint64_t saved =
            (stats._fullChainBytes > stats._residentBytes) ? stats._fullChainBytes - stats._residentBytes : 0;
        const double savedFraction =
            stats._fullChainBytes > 0 ? static_cast<double>(saved) / static_cast<double>(stats._fullChainBytes) : 0.0;

        ImGui::Text("Streamed textures: %u", stats._textureCount);
        ImGui::Text("  resident   %s", formatByteSize(stats._residentBytes).c_str());
        ImGui::Text("  full chain %s", formatByteSize(stats._fullChainBytes).c_str());
        ImGui::Text("  saved      %s (%.1f%%)", formatByteSize(saved).c_str(), savedFraction * 100.0);

        char overlay[64];
        std::snprintf(overlay, sizeof(overlay), "%.0f%% of unstreamed", (1.0 - savedFraction) * 100.0);
        ImGui::ProgressBar(static_cast<float>(1.0 - savedFraction), ImVec2(-FLT_MIN, 0.0f), overlay);

        // Where the textures actually sit, which is what makes a small saving legible: a scene
        // whose every texture is still at level 0 has nothing to give back yet.
        std::string levels;
        for (uint32_t level = 0; level < kVkmStreamingHistogramLevels; ++level)
        {
            if (stats._levelHistogram[level] != 0)
            {
                levels += "  " + std::to_string(level) + ":" + std::to_string(stats._levelHistogram[level]);
            }
        }
        ImGui::Text("  at level %s", levels.c_str());

        // Texel bytes, so this is what an upload writes rather than what the allocator committed;
        // tiling and alignment padding are invisible from here and make the real saving smaller.
        ImGui::TextDisabled("Texel bytes, excluding allocator padding");

        if (stats._rebuildInFlight || stats._pendingRetireCount != 0)
        {
            // Says out loud why the engine-wide figure below can sit above the resident one.
            ImGui::TextDisabled("Rebuilding: %s, %u texture(s) awaiting release",
                                stats._rebuildInFlight ? "yes" : "no", stats._pendingRetireCount);
        }
        if (stats._failedCount != 0)
        {
            ImGui::TextDisabled("%u texture(s) pinned by a failed rebuild", stats._failedCount);
        }

        // Zero here with streaming on means the readback never arrived and the targets quietly
        // came from the bounding-sphere estimate instead -- a silent fallback worth seeing.
        ImGui::TextDisabled("Targets from GPU feedback: %u of %u", stats._feedbackCount, stats._textureCount);
    }
#endif
} // namespace

class ModelViewerApplication : public AppDelegate
{
public:
    ModelViewerApplication() = default;
    virtual ~ModelViewerApplication() = default;

    virtual void postDriverReady(VkmEngine* engine) override final
    {
        _engine = engine;
        _cameraController.registerTo(engine->getInputHandler());
        engine->setActiveCamera(&_camera);

        VkmPipelineStateManager* manager = engine->getPipelineStateManager();
        std::string err;
        if (!manager->loadPipelineStatesFromDirectory(SAMPLE_DIR, SAMPLE_SHADER_CACHE_DIR, VkmPipelineStateOrigin::User, &err))
        {
            VKM_DEBUG_ERROR(("Failed to load sample pipeline states: " + err).c_str());
            return;
        }
        // One PSO permutation per vertex layout preset; a scene only binds the ones its pools use.
        for (uint8_t i = 0; i < static_cast<uint8_t>(VkmVertexLayoutPreset::Count); ++i)
        {
            const std::string psoName =
                std::string("model_viewer_pso[") +
                vkmVertexLayoutPresetName(static_cast<VkmVertexLayoutPreset>(i)) + "]";
            _layoutPipelines[i] = manager->getPipelineState(psoName, VkmPipelineStateOrigin::User);
            VKM_ASSERT(_layoutPipelines[i] != nullptr, ("Failed to create " + psoName).c_str());
        }

        _sceneEntries = vkmsample::scanSceneDirectory(std::filesystem::path(RESOURCES_DIR) / "Scenes");

        std::vector<std::string> startupPaths;
        std::error_code ec;
        for (const std::string& path : vkmsample::splitScenePaths(gv_model_path.get()))
        {
            if (std::filesystem::is_regular_file(path, ec))
            {
                startupPaths.push_back(path);
            }
            else
            {
                VKM_DEBUG_INFO(("No scene at '" + path +
                                "'; pick one in the Scene Browser or run scripts/download_scenes.py").c_str());
            }
        }
        if (!startupPaths.empty())
        {
            rebuildScene(startupPaths);
        }
    }

    virtual void preShutdown() override final
    {
        _cameraController.unregister();
        _flyController.unregister();
        if (_engine != nullptr)
        {
            _engine->setActiveCamera(nullptr); // the camera dies with this delegate
            _scene.destroy(_engine->getDriver());
        }
    }

    virtual void update(const double deltaTime) override final
    {
        // Only the fly controller needs a tick: the orbit controller is driven entirely by the
        // events its listener receives.
        if (_flyMode)
        {
            _flyController.tick(deltaTime);
        }
        pollDebugModeKeys();
        updateDepthTexture();
        updateTextureStreaming();

#if defined(VKM_ENABLE_IMGUI)
        drawSceneBrowser();
#endif

        // Deferred to here so the swap never happens while the browser window is still
        // being built (rebuildScene() invalidates what that code is iterating over).
        if (_hasPendingScenePaths)
        {
            const std::vector<std::string> paths = std::move(_pendingScenePaths);
            _pendingScenePaths.clear();
            _hasPendingScenePaths = false;
            rebuildScene(paths);
        }
    }

    virtual void render(uint32_t windowIndex, VkmRenderGraph* renderGraph, VkmResourceHandle backBuffer) override final
    {
        VkmSwapChainBase* swapChain = _engine->getSwapChain(windowIndex);
        const glm::uvec2 extent = swapChain->getExtent();

        VkmFrameBufferDescriptor frameBufferDesc;
        frameBufferDesc._renderPass._colorAttachmentCount = 1;
        frameBufferDesc._renderPass._colorAttachments[0]._attachmentId = 0;
        frameBufferDesc._renderPass._colorAttachments[0]._loadAction = VkmLoadAction::Clear;
        frameBufferDesc._renderPass._colorAttachments[0]._storeAction = VkmStoreAction::Store;
        frameBufferDesc._renderPass._colorAttachments[0]._clearColors[0] = 0.08f;
        frameBufferDesc._renderPass._colorAttachments[0]._clearColors[1] = 0.09f;
        frameBufferDesc._renderPass._colorAttachments[0]._clearColors[2] = 0.11f;
        frameBufferDesc._renderPass._colorAttachments[0]._clearColors[3] = 1.0f;

        frameBufferDesc._width = extent.x;
        frameBufferDesc._height = extent.y;
        frameBufferDesc._colorAttachments[0] = backBuffer;

        if (_depthTexture != VKM_INVALID_RESOURCE_HANDLE)
        {
            VkmDepthStencilAttachmentDescriptor depthDesc{};
            depthDesc._attachmentId = 0;
            depthDesc._loadAction = VkmLoadAction::Clear;
            depthDesc._storeAction = VkmStoreAction::Store;
            depthDesc._clearDepth = 1.0f;
            depthDesc._clearStencil = 0;
            frameBufferDesc._renderPass._depthStencilAttachment = depthDesc;
            frameBufferDesc._depthStencilAttachment = _depthTexture;
        }

        if (!_sceneReady)
        {
            // Still a valid frame: the clear alone runs.
            renderGraph->beginGraphicsSubGraph(frameBufferDesc, "ModelViewerPass");
            return;
        }

        VkmFrameData frameData;
        // The camera itself travels through descriptor set 1, which the engine rewrites for the
        // active camera each frame; the scene only needs the frustum planes derived from it.
        vkmExtractFrustumPlanes(_camera.getViewProjection(), frameData._frustumPlanes);
        // World space now that the draw path pushes no per-object constants: the shader rotates the
        // normal by the object's normalTransform instead of pre-rotating the light per draw.
        frameData._lightDirection = glm::vec4(glm::normalize(kWorldLightDirection), 0.0f);
        frameData._debugMode = static_cast<uint32_t>(_debugMode);

        std::vector<VkmResourceAccessDeclaration> referenced;

        // The per-frame copies have to be recorded outside a render pass, and subgraphs commit in
        // insertion order, so this publishes them ahead of the draws that read them.
        auto updateSubGraph = renderGraph->beginTransferSubGraph("SceneUpdate");
        referenced.clear();
        _scene.collectReferencedResources(VkmScene::ReferencePhase::Update, &referenced);
        updateSubGraph->addReferencedResources(referenced);
        // The graph is the per-frame-slot object, so it already knows which slot this is.
        const uint32_t frameIndex = renderGraph->frameIndex();
        updateSubGraph->setTransferCallback([this, frameIndex, frameData](VkmCommandBufferBase* commandBuffer) {
            _scene.recordUpdate(commandBuffer, frameIndex, frameData);
        });

        // A moved object changes only its instance transform, so the structure the gizmo
        // invalidated is rebuilt in place rather than recreated.
        if (_accelerationStructureDirty)
        {
            auto rebuildSubGraph = renderGraph->beginComputeSubGraph("SceneAccelerationStructureRebuild");
            rebuildSubGraph->addReferencedResource(_scene.getTopLevelAccelerationStructure(),
                                                   VkmResourceAccess::AccelerationStructureBuildWrite);
            rebuildSubGraph->setComputeCallback([this](VkmCommandBufferBase* commandBuffer) {
                _scene.recordAccelerationStructureUpdate(commandBuffer);
            });
            _accelerationStructureDirty = false;
        }

        // Frustum culling and the emit pass that writes this frame's indirect draw arguments.
        auto cullSubGraph = renderGraph->beginComputeSubGraph("SceneCull");
        referenced.clear();
        _scene.collectReferencedResources(VkmScene::ReferencePhase::Cull, &referenced);
        cullSubGraph->addReferencedResources(referenced);
        cullSubGraph->setComputeCallback([this](VkmCommandBufferBase* commandBuffer) {
            _scene.recordCull(commandBuffer);
        });

        auto graphicsSubGraph = renderGraph->beginGraphicsSubGraph(frameBufferDesc, "ModelViewerPass");
        referenced.clear();
        _scene.collectReferencedResources(VkmScene::ReferencePhase::Draw, &referenced);
        graphicsSubGraph->addReferencedResources(referenced);
        graphicsSubGraph->setRenderCallback([this](VkmCommandBufferBase* commandBuffer) {
            _scene.recordDrawBatches(commandBuffer, [this](const VkmScene::DrawBatch& batch) {
                return _layoutPipelines[static_cast<size_t>(batch._layout)];
            });
        });
    }

    virtual const char* getAppName() const override final
    {
        return "ModelViewerApplication";
    }

private:
    /*
    * @brief Replaces whatever is loaded with the glTFs at `paths`, in order.
    * @details Synchronous and stalling by nature -- VkmDriverBase::uploadToBuffer already blocks
    * per buffer -- so the frame that triggers a load simply takes as long as the load does. A
    * model that fails to import is skipped and reported; the rest still load. Every model is
    * placed at the transform its own nodes give it, and each keeps a gizmo transform on top of
    * that, reset to identity here because a rebuild re-imports from scratch.
    * @param paths Files to load. An empty list leaves the viewer with no scene.
    */
    void rebuildScene(const std::vector<std::string>& paths)
    {
        VkmDriverBase* driver = _engine->getDriver();

        // Imported before anything is torn down, so a failed import leaves the current scene
        // standing rather than emptying the viewer.
        std::vector<VkmSceneModel> models;
        std::vector<std::string> loadedPaths;
        std::string error;
        std::string failures;
        for (const std::string& path : paths)
        {
            VkmSceneModel model;
            if (!importGltfModel(path, &model, &error))
            {
                failures += (failures.empty() ? "" : "; ") + path + ": " + error;
                VKM_DEBUG_ERROR(("Failed to import '" + path + "': " + error).c_str());
                continue;
            }
            models.push_back(std::move(model));
            loadedPaths.push_back(path);
        }

        if (models.empty() && !paths.empty())
        {
            _loadError = failures;
            return;
        }

        // The old scene's buffers are still referenced by frames in flight, and its bindless
        // slots would be handed straight back out by the build below. Draining the queue is
        // the honest way to make both safe, and this path is already a stall.
        driver->getCommandQueue(VkmCommandQueueType::Graphics, 0)->waitIdle(MAX_GPU_TIMEOUT_PER_FRAME);
        _sceneReady = false;
        _scene.destroy(driver);
        _models.clear();
        _bakedTransforms.clear();
        _meshCount = 0;
        _vertexCount = 0;

        for (size_t i = 0; i < models.size(); ++i)
        {
            if (!_scene.addModel(models[i], &error))
            {
                _loadError = error;
                VKM_DEBUG_ERROR(("Failed to add '" + loadedPaths[i] + "': " + error).c_str());
                _scene.destroy(driver);
                _models.clear();
                return;
            }
            LoadedModel entry;
            entry._path = loadedPaths[i];
            entry._displayName = std::filesystem::path(loadedPaths[i]).filename().string();
            _models.push_back(std::move(entry));
            _meshCount += models[i]._meshes.size();
            _vertexCount += models[i].getTotalVertexCount();
        }

        if (!_models.empty() && !_scene.build(driver, _engine->getPipelineStateManager(), &error))
        {
            _loadError = error;
            VKM_DEBUG_ERROR(("Failed to build the scene: " + error).c_str());
            // Unlike a failed import, this already tore the previous scene down.
            _scene.destroy(driver);
            _models.clear();
            return;
        }

        // Optional: the structures give the F4 inspector (and any ray-query pass) something to
        // show, and a backend without the capability just skips.
        _accelerationStructureReady = false;
        if (!_models.empty() &&
            (driver->getDriverCapabilityFlags() & VkmDriverCapabilityFlags::RayTracing) != 0)
        {
            std::string asError;
            if (_scene.buildAccelerationStructures(driver, &asError))
            {
                _accelerationStructureReady = true;
            }
            else
            {
                VKM_DEBUG_ERROR(("Failed to build acceleration structures: " + asError).c_str());
            }
        }

        // The placement each object was built with. A gizmo drag composes onto this rather than
        // onto the object's current transform, so dragging cannot accumulate drift.
        const std::vector<VkmSceneObject>& objects = _scene.getObjects();
        _bakedTransforms.reserve(objects.size());
        for (const VkmSceneObject& object : objects)
        {
            _bakedTransforms.push_back(object._worldTransform);
        }

        _selectedModel = _models.empty() ? -1 : 0;
        // frame() moves the shared camera whether or not the orbit controller is the registered
        // one, so in fly mode the fly controller has to adopt the framed pose or its next tick
        // would snap the view back.
        frameCameraOnBounds(_cameraController, _scene.computeWorldBounds());
        if (_flyMode)
        {
            _flyController.syncFromCamera();
        }
        _loadError = failures;
        _sceneReady = !_models.empty();

        VKM_DEBUG_LOG((std::to_string(_models.size()) + " model(s): " +
                       std::to_string(_meshCount) + " meshes, " +
                       std::to_string(_vertexCount) + " vertices, " +
                       std::to_string(_scene.getDrawBatches().size()) + " draw batches").c_str());
    }

    // The paths currently loaded, in order -- what a rebuild after an add or a remove replays.
    std::vector<std::string> loadedPaths() const
    {
        std::vector<std::string> paths;
        paths.reserve(_models.size());
        for (const LoadedModel& model : _models)
        {
            paths.push_back(model._path);
        }
        return paths;
    }

    /*
    * @brief Republishes every object of `modelIndex` under that model's gizmo transform.
    * @details Batching reorders the scene's objects, so the model's objects are found by their
    * _modelIndex rather than by a contiguous range. setObjectTransform widens the range
    * recordUpdate() re-uploads and flags the batch bounds, so nothing else has to be refreshed
    * here for rasterization.
    * @param modelIndex Which loaded model moved.
    */
    void applyModelTransform(size_t modelIndex)
    {
        const glm::mat4& gizmo = _models[modelIndex]._transform;
        const std::vector<VkmSceneObject>& objects = _scene.getObjects();
        for (uint32_t i = 0; i < static_cast<uint32_t>(objects.size()); ++i)
        {
            if (objects[i]._modelIndex == static_cast<uint32_t>(modelIndex))
            {
                _scene.setObjectTransform(i, gizmo * _bakedTransforms[i]);
            }
        }
        _accelerationStructureDirty = _accelerationStructureReady;
    }

#if defined(VKM_ENABLE_IMGUI)
    void drawSceneBrowser()
    {
        if (!ImGui::Begin("Scene Browser"))
        {
            ImGui::End();
            return;
        }

        if (ImGui::Button("Rescan"))
        {
            _sceneEntries = vkmsample::scanSceneDirectory(std::filesystem::path(RESOURCES_DIR) / "Scenes");
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%zu scene(s) under resources/Scenes", _sceneEntries.size());

        if (_sceneEntries.empty())
        {
            ImGui::TextWrapped("Nothing to load. Run 'python3 scripts/download_scenes.py' to fetch "
                               "the sample scenes, then press Rescan.");
        }

        // Available: what is on disk. Selecting here picks what Add and Replace will load, and
        // says nothing about what is currently in the scene -- that is the Loaded list below.
        ImGui::TextDisabled("Available");
        if (ImGui::BeginListBox("##scenes", ImVec2(-FLT_MIN, 6 * ImGui::GetTextLineHeightWithSpacing())))
        {
            for (int i = 0; i < static_cast<int>(_sceneEntries.size()); ++i)
            {
                const bool selected = (i == _selectedEntry);
                if (ImGui::Selectable(_sceneEntries[i]._displayName.c_str(), selected))
                {
                    _selectedEntry = i;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndListBox();
        }

        const bool hasEntry = _selectedEntry >= 0 && _selectedEntry < static_cast<int>(_sceneEntries.size());
        // Both go through the same rebuild: addModel() must precede build(), so adding one model
        // to a built scene means re-importing all of them.
        if (!hasEntry)
        {
            ImGui::BeginDisabled();
        }
        if (ImGui::Button("Add"))
        {
            _pendingScenePaths = loadedPaths();
            _pendingScenePaths.push_back(_sceneEntries[_selectedEntry]._path);
            _hasPendingScenePaths = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Replace"))
        {
            _pendingScenePaths = {_sceneEntries[_selectedEntry]._path};
            _hasPendingScenePaths = true;
        }
        if (!hasEntry)
        {
            ImGui::EndDisabled();
        }

        ImGui::Separator();
        ImGui::TextDisabled("Loaded (%zu)", _models.size());
        if (_models.empty())
        {
            ImGui::TextDisabled("No scene loaded");
        }
        else
        {
            if (ImGui::BeginListBox("##loaded", ImVec2(-FLT_MIN, 4 * ImGui::GetTextLineHeightWithSpacing())))
            {
                for (int i = 0; i < static_cast<int>(_models.size()); ++i)
                {
                    char label[256];
                    std::snprintf(label, sizeof(label), "[%d] %s", i, _models[i]._displayName.c_str());
                    if (ImGui::Selectable(label, i == _selectedModel))
                    {
                        _selectedModel = i;
                    }
                }
                ImGui::EndListBox();
            }

            if (ImGui::Button("Remove") && _selectedModel >= 0)
            {
                std::vector<std::string> paths = loadedPaths();
                paths.erase(paths.begin() + _selectedModel);
                _pendingScenePaths = std::move(paths);
                _hasPendingScenePaths = true;
            }
            ImGui::SameLine();
            if (ImGui::Button("Reframe camera"))
            {
                setFlyMode(false);
                frameCameraOnBounds(_cameraController, _scene.computeWorldBounds());
            }

            ImGui::Text("%zu meshes, %llu vertices, %zu objects in %zu batch(es)",
                        _meshCount,
                        static_cast<unsigned long long>(_vertexCount),
                        _scene.getObjects().size(),
                        _scene.getDrawBatches().size());
            drawGizmoUi();
        }

        ImGui::Separator();
        int debugMode = static_cast<int>(_debugMode);
        if (ImGui::Combo("Debug view", &debugMode, kDebugModeNames, static_cast<int>(kDebugModeCount)))
        {
            _debugMode = static_cast<DebugMode>(debugMode);
        }
        if (_debugMode == DebugMode::TangentNormal)
        {
            ImGui::TextDisabled("Magenta = no tangent (the asset ships none, or the layout has no room)");
        }
        else if (_debugMode == DebugMode::StreamingMip)
        {
            ImGui::TextDisabled("Green = the whole chain is resident, red = only the coarsest level");
        }
        else
        {
            ImGui::TextDisabled("Or press 0-%zu over the render window", kDebugModeCount - 1);
        }

        ImGui::Separator();
        drawTextureStreamingUi();

        ImGui::Separator();
        bool flyMode = _flyMode;
        if (ImGui::Checkbox("Fly camera (WASD)", &flyMode))
        {
            setFlyMode(flyMode);
        }
        if (_flyMode)
        {
            ImGui::TextDisabled("WASD move, Q/E down/up, Shift boost, left-drag looks");
            float moveSpeed = _flyController.getMoveSpeed();
            if (ImGui::DragFloat("Speed", &moveSpeed, 0.1f, 0.01f, 1000.0f, "%.2f u/s"))
            {
                _flyController.setMoveSpeed(moveSpeed);
            }
        }
        else
        {
            ImGui::TextDisabled("Left-drag orbits, scroll dollies");
        }

        if (!_loadError.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", _loadError.c_str());
        }

        ImGui::End();
    }
    /*
    * @brief Mode buttons and the mouse gizmo for the selected model.
    * @details The mode buttons stay in this panel; the manipulator itself is drawn on the scene
    * window through the engine's gizmo overlay. The whole matrix is read back, translation,
    * rotation and scale alike, and becomes the model's transform.
    */
    void drawGizmoUi()
    {
        if (_selectedModel < 0 || _selectedModel >= static_cast<int>(_models.size()))
        {
            return;
        }

        ImGui::Separator();
        int operation = static_cast<int>(_gizmoOperation);
        ImGui::TextDisabled("Gizmo");
        bool changedMode = ImGui::RadioButton("Translate", &operation, static_cast<int>(ImGuizmo::TRANSLATE));
        ImGui::SameLine();
        changedMode |= ImGui::RadioButton("Rotate", &operation, static_cast<int>(ImGuizmo::ROTATE));
        ImGui::SameLine();
        changedMode |= ImGui::RadioButton("Scale", &operation, static_cast<int>(ImGuizmo::SCALE));
        if (changedMode)
        {
            _gizmoOperation = static_cast<ImGuizmo::OPERATION>(operation);
        }

        LoadedModel& model = _models[static_cast<size_t>(_selectedModel)];
        if (ImGui::Button("Reset transform"))
        {
            model._transform = glm::mat4(1.0f);
            applyModelTransform(static_cast<size_t>(_selectedModel));
        }

        // Opened by the engine against the scene window, so the manipulator is drawn and dragged
        // over the scene rather than over this panel's own window.
        if (!_engine->beginGizmoOverlay())
        {
            return;
        }
        const glm::mat4 view = _camera.getView();
        const glm::mat4 projection = _camera.getProjection();
        glm::mat4 transform = model._transform;
        if (ImGuizmo::Manipulate(&view[0][0], &projection[0][0], _gizmoOperation, ImGuizmo::WORLD,
                                 &transform[0][0]))
        {
            model._transform = transform;
            applyModelTransform(static_cast<size_t>(_selectedModel));
        }
        _engine->endGizmoOverlay();

        // The light table bakes emissive triangles in world space at build (TODO.md), so a moved
        // emitter lights the scene from where it was loaded until the scene is rebuilt.
        ImGui::TextDisabled("Emissive lighting stays where the model was loaded");
    }
#endif // VKM_ENABLE_IMGUI

    /*
    * @brief Selects a debug view from the number row of the window the scene is drawn in.
    * @details The pressed edge is drained once per frame by the engine before update() runs, so
    * polling here fires exactly once per physical press. ImGui lives in its own window on desktop,
    * so its combo and these keys never see the same keystroke.
    */
    void pollDebugModeKeys()
    {
        if (_engine == nullptr)
        {
            return;
        }

        const VkmInputHandler& input = _engine->getInputHandler();
        for (size_t mode = 0; mode < kDebugModeCount; ++mode)
        {
            if (input.isKeyPressed(kDebugModeKeys[mode]))
            {
                _debugMode = static_cast<DebugMode>(mode);
            }
        }
    }

    /*
    * @brief Hands the camera between the orbit and fly controllers.
    * @details Only one is registered at a time, so neither can fight the other over the shared
    * camera. The incoming controller adopts the current view first, so the switch is invisible.
    */
    void setFlyMode(bool enabled)
    {
        if (enabled == _flyMode || _engine == nullptr)
        {
            return;
        }
        _flyMode = enabled;

        if (_flyMode)
        {
            _cameraController.unregister();
            _flyController.syncFromCamera();
            _flyController.registerTo(_engine->getInputHandler());
        }
        else
        {
            _flyController.unregister();
            _cameraController.registerTo(_engine->getInputHandler());
            // The orbit controller only touches the camera when it receives an event, so
            // without this the view would sit wherever the fly camera left it and then jump on
            // the first drag. Reframing is the one pose an orbit camera can always define.
            frameCameraOnBounds(_cameraController, _scene.computeWorldBounds());
        }
    }

#if defined(VKM_ENABLE_IMGUI)
    void drawTextureStreamingUi()
    {
        if (_scene.isTextureStreamingAvailable())
        {
            VkmTextureStreamingSettings settings = _scene.getTextureStreamingSettings();
            bool changed = ImGui::Checkbox("Texture streaming", &settings._enabled);

            int mipBias = static_cast<int>(settings._mipBias);
            if (ImGui::SliderInt("Mip bias", &mipBias, -4, 4))
            {
                settings._mipBias = static_cast<int32_t>(mipBias);
                changed = true;
            }
            ImGui::TextDisabled("Negative keeps more detail than the screen needs, positive trades it for memory");

            if (changed)
            {
                _scene.setTextureStreamingSettings(settings);
            }

            drawTextureStreamingStats(_scene.getTextureStreamingStats());
        }
        else
        {
            ImGui::TextDisabled("Texture streaming: unavailable (no bindless texture array)");
        }

        // Drawn on every backend, streaming or not: it is the whole-engine figure the streamed
        // total has to be read against, and the one number that includes render targets.
        const VkmResourceCategoryUsage textures =
            _engine->getDriver()->getRenderResourcePool()->getCategoryMemoryUsage(VkmResourceType::Texture);
        ImGui::Text("All textures: %s live in %u", formatByteSize(textures.totalAllocatedBytes).c_str(),
                    textures.liveCount);
    }
#else
    void drawTextureStreamingUi() {}
#endif

    /*
    * Runs before the frame records anything, which is what the streamer's texture creation and
    * blocking uploads need -- and the camera is already current, the engine having drained input
    * before calling update().
    */
    void updateTextureStreaming()
    {
        if (!_sceneReady)
        {
            return;
        }

        VkmTextureStreamingView view;
        view._cameraPosition = _camera.getPosition();
        view._viewportHeight = _camera.getViewportHeight();
        view._fovYRadians = _camera.getFovYRadians();
        _scene.updateTextureStreaming(_engine->getDriver(), view);
    }

    // The depth buffer must always match the swapchain, which the window can resize under us.

    void updateDepthTexture()
    {
        VkmSwapChainBase* swapChain = _engine->getMainSwapChain();
        if (swapChain == nullptr)
        {
            return;
        }

        const glm::uvec2 extent = swapChain->getExtent();
        if (extent.x == 0 || extent.y == 0 || extent == _depthExtent)
        {
            return;
        }

        VkmDriverBase* driver = _engine->getDriver();
        if (_depthTexture != VKM_INVALID_RESOURCE_HANDLE)
        {
            // Deferred: earlier frames may still be reading this texture on the GPU.
            driver->getDeferredReclaimer()->requestRelease(_depthTexture);
            _depthTexture = VKM_INVALID_RESOURCE_HANDLE;
        }

        VkmTextureInfo depthInfo{};
        depthInfo._flags = VkmResourceCreateInfo::AllowDepthStencilAttachment;
        depthInfo._extent = glm::uvec3(extent, 1);
        depthInfo._numMipLevels = 1;
        depthInfo._numArrayLayers = 1;
        depthInfo._format = kDepthFormat;
        depthInfo._debugName = "ModelViewerDepthBuffer";

        VkmTexture* depthTexture = driver->newTexture(depthInfo);
        if (depthTexture == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create the depth buffer");
            return;
        }

        _depthTexture = depthTexture->getHandle();
        _depthExtent = extent;
    }

private:
    VkmEngine* _engine{nullptr};
    // One PSO per vertex layout preset, indexed by VkmVertexLayoutPreset.
    std::array<VkmPipelineStateBase*, static_cast<size_t>(VkmVertexLayoutPreset::Count)> _layoutPipelines{};
    VkmScene _scene;
    VkmCamera _camera;
    VkmOrbitCameraController _cameraController{&_camera};
    // Exactly one of the two is registered at a time; see setFlyMode().
    VkmFlyCameraController _flyController{&_camera};
    bool _flyMode{false};
    DebugMode _debugMode{DebugMode::Lit};
    std::vector<vkmsample::SceneEntry> _sceneEntries;
    // One loaded glTF. _transform is what the gizmo edits; it composes onto the placement the
    // asset's own nodes gave each object, which _bakedTransforms holds.
    struct LoadedModel
    {
        std::string _path;
        std::string _displayName;
        glm::mat4 _transform{1.0f};
    };
    std::vector<LoadedModel> _models;
    // 1:1 with VkmScene::getObjects(), captured right after build().
    std::vector<glm::mat4> _bakedTransforms;
    int _selectedModel{-1}; // which model the gizmo drives; -1 when nothing is loaded
    int _selectedEntry{-1}; // which Available row Add/Replace act on
#if defined(VKM_ENABLE_IMGUI)
    ImGuizmo::OPERATION _gizmoOperation{ImGuizmo::TRANSLATE};
#endif
    // Set by the browser, consumed at the end of update(): loading tears down the scene the
    // browser is still iterating over.
    std::vector<std::string> _pendingScenePaths;
    bool _hasPendingScenePaths{false};
    std::string _loadError;
    bool _sceneReady{false};
    bool _accelerationStructureReady{false};
    bool _accelerationStructureDirty{false};
    // Import-time totals kept for the browser's stats line; the CPU-side model is dropped once
    // its geometry is pooled.
    size_t _meshCount{0};
    uint64_t _vertexCount{0};
    VkmResourceHandle _depthTexture{VKM_INVALID_RESOURCE_HANDLE};
    glm::uvec2 _depthExtent{0, 0};
};

int main(int argc, char* argv[])
{
    VkmApplication app;

    int ret = app.entryPoint(new ModelViewerApplication(), argc, argv);
    app.destroy();

    return ret;
}
