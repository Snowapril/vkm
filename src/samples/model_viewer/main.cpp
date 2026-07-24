#include <cxxopts.hpp>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/gtc/matrix_inverse.hpp>

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
#include <vkm/renderer/engine.h>
#include <vkm/renderer/scene/gltf_importer.h>
#include <vkm/renderer/scene/scene_model.h>
#include <vkm/renderer/scene/scene_model_gpu.h>

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
    // Mirrors PushConstants in model_viewer.hlsl (HLSL cbuffer packing: a float4 starts on
    // a 16-byte boundary, which is what the explicit padding is for).
    struct ModelViewerPushConstants
    {
        glm::mat4 _modelViewProjection;
        uint32_t _vertexBufferSlot;
        uint32_t _indexBufferSlot;
        float _pad0[2];
        glm::vec4 _baseColor;
        glm::vec4 _lightDirectionObjectSpace;
    };
    static_assert(sizeof(ModelViewerPushConstants) <= kVkmBindlessPushConstantSize,
                  "The push constants must fit the engine-global push-constant range");

    constexpr VkmFormat kDepthFormat = VkmFormat::D32_SFLOAT;
    constexpr glm::vec3 kWorldLightDirection{ 0.4f, 0.8f, 0.45f }; // towards the light
    constexpr glm::vec4 kFallbackBaseColor{ 0.8f, 0.8f, 0.8f, 1.0f };

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

    // Orbit camera: left-drag rotates, scroll dollies in/out.
    class OrbitCamera
    {
    public:
        void frame(const VkmSceneAABB& bounds)
        {
            _target = bounds._valid ? bounds.getCenter() : glm::vec3(0.0f);
            const float radius = bounds._valid ? glm::length(bounds.getExtent()) * 0.5f : 1.0f;
            _distance = std::max(radius * 2.5f, 0.001f);
            _minDistance = _distance * 0.05f;
            _maxDistance = _distance * 20.0f;
        }

        void update(VkmInputHandler& input)
        {
            if (input.isMouseButtonDown(VkmMouseButton::Left))
            {
                _yaw += static_cast<float>(input.getCursorDeltaX()) * 0.005f;
                _pitch += static_cast<float>(input.getCursorDeltaY()) * 0.005f;
                // Just short of the poles, where the up vector would degenerate.
                _pitch = std::clamp(_pitch, -1.5f, 1.5f);
            }

            const double scroll = input.getScrollDeltaY();
            if (scroll != 0.0)
            {
                _distance = std::clamp(_distance * std::pow(0.9f, static_cast<float>(scroll)), _minDistance, _maxDistance);
            }
        }

        glm::mat4 getViewProjection(uint32_t width, uint32_t height) const
        {
            const glm::vec3 eye = _target + glm::vec3{
                _distance * std::cos(_pitch) * std::sin(_yaw),
                _distance * std::sin(_pitch),
                _distance * std::cos(_pitch) * std::cos(_yaw),
            };
            const glm::mat4 view = glm::lookAtRH(eye, _target, glm::vec3(0.0f, 1.0f, 0.0f));

            const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
            // *_ZO: the engine's clip space is +Y-up with a [0,1] depth range on every
            // backend (the Vulkan backend's -fvk-invert-y handles its inverted NDC).
            const glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(50.0f), aspect,
                                                               _distance * 0.01f, _distance * 10.0f);
            return projection * view;
        }

    private:
        glm::vec3 _target{ 0.0f };
        float _distance = 3.0f;
        float _minDistance = 0.1f;
        float _maxDistance = 100.0f;
        float _yaw = 0.6f;
        float _pitch = 0.3f;
    };
} // namespace

class ModelViewerApplication : public AppDelegate
{
public:
    ModelViewerApplication() = default;
    virtual ~ModelViewerApplication() = default;

    virtual void postDriverReady(VkmEngine* engine) override final
    {
        _engine = engine;

        VkmPipelineStateManager* manager = engine->getPipelineStateManager();
        std::string err;
        if (!manager->loadPipelineStatesFromDirectory(SAMPLE_DIR, SAMPLE_SHADER_CACHE_DIR, VkmPipelineStateOrigin::User, &err))
        {
            VKM_DEBUG_ERROR(("Failed to load sample pipeline states: " + err).c_str());
            return;
        }
        _pso = manager->getPipelineState("model_viewer_pso[default]", VkmPipelineStateOrigin::User);
        VKM_ASSERT(_pso != nullptr, "Failed to create model_viewer_pso[default]");

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
        if (_engine != nullptr)
        {
            _modelGpu.destroy(_engine->getDriver());
        }
    }

    virtual void update(const double deltaTime) override final
    {
        (void)deltaTime;
        _camera.update(_engine->getInputHandler());
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

        auto graphicsSubGraph = renderGraph->beginGraphicsSubGraph(frameBufferDesc, "ModelViewerPass");
        if (!_modelReady || _pso == nullptr)
        {
            return; // still a valid frame: the clear alone runs.
        }

        const glm::mat4 viewProjection = _camera.getViewProjection(extent.x, extent.y);

        std::vector<ModelViewerPushConstants> draws;
        draws.reserve(_drawList.size());
        std::vector<uint32_t> indexCounts;
        indexCounts.reserve(_drawList.size());

        for (const VkmSceneModel::DrawItem& item : _drawList)
        {
            const VkmSceneModelGpu::MeshGpu& meshGpu = _modelGpu.getMeshes()[item._meshIndex];
            if (meshGpu._indexCount == 0)
            {
                continue;
            }

            graphicsSubGraph->addReferencedResource(meshGpu._vertexBuffer);
            graphicsSubGraph->addReferencedResource(meshGpu._indexBuffer);

            const uint32_t materialIndex = _model._meshes[item._meshIndex]._materialIndex;
            const glm::vec4 baseColor = materialIndex < _model._materials.size()
                                            ? _model._materials[materialIndex]._baseColorFactor
                                            : kFallbackBaseColor;

            ModelViewerPushConstants pushConstants{};
            pushConstants._modelViewProjection = viewProjection * item._worldTransform;
            pushConstants._vertexBufferSlot = meshGpu._vertexBufferSlot;
            pushConstants._indexBufferSlot = meshGpu._indexBufferSlot;
            pushConstants._baseColor = baseColor;
            // Shading happens in object space, so the light -- not the normal -- is what
            // gets transformed: dot(inverseTranspose(M) * n, l) == dot(n, inverse(M) * l).
            pushConstants._lightDirectionObjectSpace = glm::vec4(
                glm::normalize(glm::inverse(glm::mat3(item._worldTransform)) * kWorldLightDirection), 0.0f);

            draws.push_back(pushConstants);
            indexCounts.push_back(meshGpu._indexCount);
        }

        VkmPipelineStateBase* pso = _pso;
        graphicsSubGraph->setRenderCallback([pso, draws = std::move(draws), indexCounts = std::move(indexCounts)]
                                            (VkmCommandBufferBase* commandBuffer) {
            commandBuffer->bindPipeline(pso);
            for (size_t i = 0; i < draws.size(); ++i)
            {
                commandBuffer->setPushConstants(&draws[i], sizeof(ModelViewerPushConstants));
                commandBuffer->draw(indexCounts[i], 1, 0, 0);
            }
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

        // The old model's buffers are still referenced by frames in flight, and its bindless
        // slots would be handed straight back out by the upload below. Draining the queue is
        // the honest way to make both safe, and this path is already a stall.
        driver->getCommandQueue(VkmCommandQueueType::Graphics, 0)->waitIdle(MAX_GPU_TIMEOUT_PER_FRAME);
        _modelReady = false;
        _drawList.clear();
        _modelGpu.destroy(driver);

        if (!_modelGpu.upload(driver, model, &error))
        {
            _loadError = error;
            VKM_DEBUG_ERROR(("Failed to upload the model: " + error).c_str());
            // Unlike a failed import, this already tore the previous scene down.
            _model = VkmSceneModel{};
            _currentScenePath.clear();
            return;
        }

        _model = std::move(model);
        _drawList = _model.buildDrawList();
        _camera.frame(_model.computeWorldBounds());
        _currentScenePath = path;
        _loadError.clear();
        _modelReady = true;

        VKM_DEBUG_LOG(("Imported '" + path + "': " +
                       std::to_string(_model._meshes.size()) + " meshes, " +
                       std::to_string(_model.getTotalVertexCount()) + " vertices").c_str());
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
        if (_modelReady)
        {
            ImGui::Text("Loaded: %s", _currentScenePath.c_str());
            ImGui::Text("%zu meshes, %llu vertices, %zu draws",
                        _model._meshes.size(),
                        static_cast<unsigned long long>(_model.getTotalVertexCount()),
                        _drawList.size());
            if (ImGui::Button("Reframe camera"))
            {
                _camera.frame(_model.computeWorldBounds());
            }
        }
        else
        {
            ImGui::TextDisabled("No scene loaded");
        }

        if (!_loadError.empty())
        {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s", _loadError.c_str());
        }

        ImGui::End();
    }
#endif // VKM_ENABLE_IMGUI

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
    VkmPipelineStateBase* _pso{nullptr};
    VkmSceneModel _model;
    VkmSceneModelGpu _modelGpu;
    std::vector<VkmSceneModel::DrawItem> _drawList;
    OrbitCamera _camera;
    std::vector<SceneEntry> _sceneEntries;
    std::string _currentScenePath;
    std::string _pendingScenePath; // set by the browser, consumed at the end of update()
    std::string _loadError;
    bool _modelReady{false};
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
