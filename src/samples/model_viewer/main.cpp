#include <cxxopts.hpp>

#include <glm/geometric.hpp>
#include <glm/mat3x3.hpp>
#include <glm/matrix.hpp>

#if defined(VKM_ENABLE_IMGUI)
#include <imgui.h>
#endif

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
#include <vkm/renderer/backend/common/swapchain.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/camera.h>
#include <vkm/renderer/engine.h>
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
#include <filesystem>
#include <string>
#include <vector>

using namespace vkm;

// Which glTF to open at startup: ./model_viewer --gv_model_path=/path/to/scene.gltf
// Defaults to the asset scripts/download_scenes.py drops into resources/Scenes/; when that
// file is absent the viewer starts empty and the scene browser lists whatever else is there.
VKM_GLOBAL_VARIABLE(std::string, gv_model_path,
                    std::string(RESOURCES_DIR) + "Scenes/DamagedHelmet/DamagedHelmet.gltf");

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
        Count = 5,
    };

    constexpr size_t kDebugModeCount = static_cast<size_t>(DebugMode::Count);

    // Both indexed by DebugMode, so the sized declarations fail to compile if one drifts.
    constexpr const char* kDebugModeNames[kDebugModeCount] = {
        "Lit", "Base color", "Material index", "World normal", "TBN normal",
    };
    // Digits, so nothing collides with the fly camera's WASD/QE/Shift set.
    constexpr VkmKeyCode kDebugModeKeys[kDebugModeCount] = {
        VkmKeyCode::Num0, VkmKeyCode::Num1, VkmKeyCode::Num2, VkmKeyCode::Num3, VkmKeyCode::Num4,
    };

    // One loadable file found under resources/Scenes/.
    struct SceneEntry
    {
        std::string _displayName; // path relative to the scenes directory
        std::string _path;
    };

    // Walks resources/Scenes/ for glTF files. Scenes are one directory per asset (that is
    // how scripts/download_scenes.py lays them out), so the search has to be recursive.
    std::vector<SceneEntry> scanSceneDirectory()
    {
        const std::filesystem::path scenesDirectory = std::filesystem::path(RESOURCES_DIR) / "Scenes";

        std::vector<SceneEntry> entries;
        std::error_code ec;
        if (!std::filesystem::is_directory(scenesDirectory, ec))
        {
            return entries;
        }

        for (const std::filesystem::directory_entry& entry :
             std::filesystem::recursive_directory_iterator(scenesDirectory, ec))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            const std::filesystem::path extension = entry.path().extension();
            if (extension != ".gltf" && extension != ".glb")
            {
                continue;
            }
            entries.push_back(SceneEntry{
                std::filesystem::relative(entry.path(), scenesDirectory, ec).generic_string(),
                entry.path().string(),
            });
        }

        std::sort(entries.begin(), entries.end(), [](const SceneEntry& lhs, const SceneEntry& rhs) {
            return lhs._displayName < rhs._displayName;
        });
        return entries;
    }

    // Points the orbit controller at a scene's bounds; an empty/invalid AABB falls back to a
    // unit sphere at the origin so the camera still ends up somewhere sensible.
    void frameCameraOnBounds(VkmOrbitCameraController& controller, const VkmSceneAABB& bounds)
    {
        const glm::vec3 center = bounds._valid ? bounds.getCenter() : glm::vec3(0.0f);
        const float radius = bounds._valid ? glm::length(bounds.getExtent()) * 0.5f : 1.0f;
        controller.frame(center, radius);
    }
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

        _sceneEntries = scanSceneDirectory();

        const std::string startupPath = gv_model_path.get();
        std::error_code ec;
        if (std::filesystem::is_regular_file(startupPath, ec))
        {
            loadScene(startupPath);
        }
        else
        {
            VKM_DEBUG_INFO(("No scene at '" + startupPath +
                            "'; pick one in the Scene Browser or run scripts/download_scenes.py").c_str());
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

#if defined(VKM_ENABLE_IMGUI)
        drawSceneBrowser();
#endif

        // Deferred to here so the swap never happens while the browser window is still
        // being built (loadScene() invalidates what that code is iterating over).
        if (!_pendingScenePath.empty())
        {
            const std::string path = std::move(_pendingScenePath);
            _pendingScenePath.clear();
            loadScene(path);
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
    * Replaces whatever is loaded with the glTF at `path`. Synchronous and stalling by
    * nature -- VkmDriverBase::uploadToBuffer already blocks per buffer -- so the frame that
    * triggers a load simply takes as long as the load does.
    */
    void loadScene(const std::string& path)
    {
        VkmDriverBase* driver = _engine->getDriver();

        VkmSceneModel model;
        std::string error;
        if (!importGltfModel(path, &model, &error))
        {
            _loadError = error;
            VKM_DEBUG_ERROR(("Failed to import the model: " + error).c_str());
            return;
        }

        // The old scene's buffers are still referenced by frames in flight, and its bindless
        // slots would be handed straight back out by the build below. Draining the queue is
        // the honest way to make both safe, and this path is already a stall.
        driver->getCommandQueue(VkmCommandQueueType::Graphics, 0)->waitIdle(MAX_GPU_TIMEOUT_PER_FRAME);
        _sceneReady = false;
        _scene.destroy(driver);

        if (!_scene.addModel(model, &error) ||
            !_scene.build(driver, _engine->getPipelineStateManager(), &error))
        {
            _loadError = error;
            VKM_DEBUG_ERROR(("Failed to build the scene: " + error).c_str());
            // Unlike a failed import, this already tore the previous scene down.
            _scene.destroy(driver);
            _currentScenePath.clear();
            return;
        }

        // Optional: the structures give the F4 inspector (and any ray-query pass) something to
        // show, and a backend without the capability just skips.
        if ((driver->getDriverCapabilityFlags() & VkmDriverCapabilityFlags::RayTracing) != 0)
        {
            std::string asError;
            if (!_scene.buildAccelerationStructures(driver, &asError))
            {
                VKM_DEBUG_ERROR(("Failed to build acceleration structures: " + asError).c_str());
            }
        }

        _meshCount = model._meshes.size();
        _vertexCount = model.getTotalVertexCount();
        // frame() moves the shared camera whether or not the orbit controller is the registered
        // one, so in fly mode the fly controller has to adopt the framed pose or its next tick
        // would snap the view back.
        frameCameraOnBounds(_cameraController, _scene.computeWorldBounds());
        if (_flyMode)
        {
            _flyController.syncFromCamera();
        }
        _currentScenePath = path;
        _loadError.clear();
        _sceneReady = true;

        VKM_DEBUG_LOG(("Imported '" + path + "': " +
                       std::to_string(_meshCount) + " meshes, " +
                       std::to_string(_vertexCount) + " vertices, " +
                       std::to_string(_scene.getDrawBatches().size()) + " draw batches").c_str());
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
            _sceneEntries = scanSceneDirectory();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%zu scene(s) under resources/Scenes", _sceneEntries.size());

        if (_sceneEntries.empty())
        {
            ImGui::TextWrapped("Nothing to load. Run 'python3 scripts/download_scenes.py' to fetch "
                               "the sample scenes, then press Rescan.");
        }

        if (ImGui::BeginListBox("##scenes", ImVec2(-FLT_MIN, 8 * ImGui::GetTextLineHeightWithSpacing())))
        {
            for (const SceneEntry& entry : _sceneEntries)
            {
                const bool selected = (entry._path == _currentScenePath);
                if (ImGui::Selectable(entry._displayName.c_str(), selected) && !selected)
                {
                    _pendingScenePath = entry._path;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndListBox();
        }

        ImGui::Separator();
        if (_sceneReady)
        {
            ImGui::Text("Loaded: %s", _currentScenePath.c_str());
            ImGui::Text("%zu meshes, %llu vertices, %zu objects in %zu batch(es)",
                        _meshCount,
                        static_cast<unsigned long long>(_vertexCount),
                        _scene.getObjects().size(),
                        _scene.getDrawBatches().size());
            if (ImGui::Button("Reframe camera"))
            {
                setFlyMode(false);
                frameCameraOnBounds(_cameraController, _scene.computeWorldBounds());
            }
        }
        else
        {
            ImGui::TextDisabled("No scene loaded");
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
        else
        {
            ImGui::TextDisabled("Or press 0-%zu over the render window", kDebugModeCount - 1);
        }

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
    std::vector<SceneEntry> _sceneEntries;
    std::string _currentScenePath;
    std::string _pendingScenePath; // set by the browser, consumed at the end of update()
    std::string _loadError;
    bool _sceneReady{false};
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
