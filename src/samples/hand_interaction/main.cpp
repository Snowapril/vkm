// Copyright (c) 2025 Snowapril
//
// A hand, seen through the camera, knocking a rendered sphere around.
//
// Per frame:
//   1. The hand input source hands over the newest camera frame, which is uploaded into one
//      texture and drawn as the background, and the newest hand pose.
//   2. The pose is smoothed, differenced against last frame's for velocity, and turned into six
//      collision proxies -- five fingertips and the palm (ball_sim.h).
//   3. The ball is advanced in fixed substeps against those proxies and the window edges.
//   4. The ball and, optionally, the proxies are drawn as unit spheres scaled and placed by push
//      constants, all from one mesh and one pipeline.
//
// The simulation is two-dimensional on purpose. Both plausible sources of a third dimension --
// monocular depth estimation, or inferring distance from the hand's apparent size -- are
// relative and noisy frame to frame, and feeding that into a collision test produces a ball that
// jitters and tunnels. Constraining everything to one plane in front of the camera costs nothing
// the interaction needs and makes it behave the same every time.

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

#if defined(VKM_ENABLE_IMGUI)
#include <imgui.h>
#endif

#include <vkm/base/common.h>
#include <vkm/base/global_variable.h>
#include <vkm/platform/common/app_delegate.h>
#include <vkm/platform/common/input_handler.h>
#include <vkm/renderer/backend/common/bindless_resource_manager.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/swapchain.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/camera.h>
#include <vkm/renderer/engine.h>

#if defined(VKM_PLATFORM_WINDOWS)
#include <vkm/platform/windows/application.h>
#elif defined(VKM_PLATFORM_WASM)
#include <vkm/platform/wasm/application.h>
#elif defined(VKM_PLATFORM_LINUX)
#include <vkm/platform/linux/application.h>
#else // defined(VKM_PLATFORM_WINDOWS)
#include <vkm/platform/apple/application.h>
#endif // defined(VKM_PLATFORM_WINDOWS)

#include "ball_sim.h"
#include "hand_input.h"
#include "sphere_mesh.h"

using namespace vkm;

// "cursor" forces the mouse-driven stand-in even where a camera source exists, which is the only
// way to exercise the fallback path on a machine that has a working camera.
VKM_GLOBAL_VARIABLE(std::string, gv_hand_input, "auto");

namespace
{
    constexpr VkmFormat kDepthFormat = VkmFormat::D32_SFLOAT;

    constexpr float kFovYRadians = 0.90f; // ~51 degrees
    constexpr float kNearZ = 0.05f;
    constexpr float kFarZ = 50.0f;
    // Distance from the camera to the plane everything lives on. Only the ratio between this and
    // the field of view matters; it is the plane's size in world units that follows from them.
    constexpr float kPlaneDistance = 2.0f;

    constexpr uint32_t kSphereStacks = 24;
    constexpr uint32_t kSphereSlices = 32;

    // Time constant of the pose smoothing. Long enough to swallow Vision's per-frame jitter,
    // short enough that a fast swipe still reads as a fast swipe.
    constexpr float kPoseSmoothingSeconds = 0.06f;
    // A pose older than this is dropped rather than left pushing the ball from a stale position.
    constexpr float kPoseMaxAgeSeconds = 0.30f;

    constexpr float kFingertipRadius = 0.035f; // sim units, i.e. fractions of the window width
    constexpr float kPalmRadius = 0.075f;

    /*
    * @brief One sphere draw. Mirrors the PushConstants struct in hand_sphere.hlsl.
    */
    struct SpherePushConstants
    {
        float centerAndRadius[4];
        float color[4];
        uint32_t vertexBufferIndex;
        uint32_t indexBufferIndex;
    };

    struct BackgroundPushConstants
    {
        uint32_t cameraTextureSlot;
    };

    constexpr uint32_t kMaxSphereDraws = 1 + kHandColliderCount;
}

class HandInteractionApplication : public AppDelegate
{
public:
    HandInteractionApplication() = default;
    virtual ~HandInteractionApplication() = default;

    virtual void postDriverReady(VkmEngine* engine) override final
    {
        _engine = engine;

        VkmPipelineStateManager* manager = engine->getPipelineStateManager();
        std::string error;
        if (!manager->loadPipelineStatesFromDirectory(SAMPLE_DIR, SAMPLE_SHADER_CACHE_DIR,
                                                      VkmPipelineStateOrigin::User, &error))
        {
            VKM_DEBUG_ERROR(("Failed to load sample pipeline states: " + error).c_str());
            return;
        }

        _backgroundPso = manager->getPipelineState("hand_background_pso[default]", VkmPipelineStateOrigin::User);
        _spherePso = manager->getPipelineState("hand_sphere_pso[default]", VkmPipelineStateOrigin::User);
        VKM_ASSERT(_backgroundPso != nullptr, "Failed to create hand_background_pso[default]");
        VKM_ASSERT(_spherePso != nullptr, "Failed to create hand_sphere_pso[default]");

        _camera.lookAt(glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        _camera.setPerspective(kFovYRadians, kNearZ, kFarZ);
        engine->setActiveCamera(&_camera);

        createSphereGeometry();
        createHandInput();
    }

    virtual void preShutdown() override final
    {
        if (_handInput != nullptr)
        {
            _handInput->stop();
            _handInput.reset();
        }

        if (_engine != nullptr)
        {
            _engine->setActiveCamera(nullptr);
        }
    }

    virtual void update(const double deltaTime) override final
    {
        if (_engine == nullptr)
        {
            return;
        }

        updateDepthTexture();

        VkmSwapChainBase* swapChain = _engine->getMainSwapChain();
        if (swapChain == nullptr)
        {
            return;
        }
        const glm::uvec2 extent = swapChain->getExtent();
        if (extent.x == 0 || extent.y == 0)
        {
            return;
        }

        const float step = static_cast<float>(deltaTime);
        // The playfield is measured in window widths, so its height is the reciprocal aspect and
        // it re-fits the window on every resize with no other state to update.
        const float invAspect = static_cast<float>(extent.y) / static_cast<float>(extent.x);
        _simParams._boundsMin = glm::vec2(0.0f, 0.0f);
        _simParams._boundsMax = glm::vec2(1.0f, invAspect);

        if (_handInput != nullptr)
        {
            _handInput->setViewportSize(extent.x, extent.y);
            acquireCameraFrame();
            acquirePose(step);
        }

        const HandPose previousPose = _smoothedPose;
        updateSmoothedPose(step);
        buildHandColliders(_smoothedPose, previousPose, step, invAspect,
                           kFingertipRadius, kPalmRadius, &_handColliders);

        stepBall(_simParams, _handColliders, step, &_ball);

        drawUi();
    }

    virtual void render(uint32_t windowIndex, VkmRenderGraph* renderGraph, VkmResourceHandle backBuffer) override final
    {
        VkmSwapChainBase* swapChain = _engine->getSwapChain(windowIndex);
        const glm::uvec2 extent = swapChain->getExtent();
        if (extent.x == 0 || extent.y == 0)
        {
            return;
        }

        recordBackgroundPass(renderGraph, backBuffer, extent);
        recordSpherePass(renderGraph, backBuffer, extent);
    }

    virtual const char* getAppName() const override final
    {
        return "HandInteractionApplication";
    }

private:
    void createSphereGeometry()
    {
        SphereMesh mesh;
        buildSphereMesh(kSphereStacks, kSphereSlices, &mesh);

        VkmDriverBase* driver = _engine->getDriver();

        // AllowShaderWrite rather than AllowShaderRead: the latter maps to a uniform buffer,
        // which cannot back a bindless storage-buffer array element. AllowTransferSrc is what the
        // WebGPU backend's registerBuffer copy needs and costs nothing elsewhere. Same reasoning
        // as the triangle sample's buffers.
        const VkmResourceCreateInfo bufferFlags = VkmResourceCreateInfo::AllowShaderWrite |
                                                  VkmResourceCreateInfo::AllowTransferDst |
                                                  VkmResourceCreateInfo::AllowTransferSrc;

        VkmBufferInfo vertexInfo{};
        vertexInfo._flags = bufferFlags;
        vertexInfo._size = mesh._vertices.size() * sizeof(SphereVertex);
        vertexInfo._debugName = "HandInteractionSphereVertices";
        VkmBuffer* vertexBuffer = driver->newBuffer(vertexInfo);
        VKM_ASSERT(vertexBuffer != nullptr, "Failed to create the sphere vertex buffer");
        if (!driver->uploadToBuffer(vertexBuffer->getHandle(), mesh._vertices.data(), vertexInfo._size))
        {
            VKM_DEBUG_ERROR("Failed to upload the sphere vertex buffer");
            return;
        }

        VkmBufferInfo indexInfo{};
        indexInfo._flags = bufferFlags;
        indexInfo._size = mesh._indices.size() * sizeof(uint32_t);
        indexInfo._debugName = "HandInteractionSphereIndices";
        VkmBuffer* indexBuffer = driver->newBuffer(indexInfo);
        VKM_ASSERT(indexBuffer != nullptr, "Failed to create the sphere index buffer");
        if (!driver->uploadToBuffer(indexBuffer->getHandle(), mesh._indices.data(), indexInfo._size))
        {
            VKM_DEBUG_ERROR("Failed to upload the sphere index buffer");
            return;
        }

        VkmBindlessResourceManagerBase* bindlessManager = driver->getBindlessResourceManager();
        _sphereVertexSlot = bindlessManager->registerBuffer(vertexBuffer->getHandle(), VkmBindlessArrayType::Buffer);
        _sphereIndexSlot = bindlessManager->registerBuffer(indexBuffer->getHandle(), VkmBindlessArrayType::IndexBuffer);
        VKM_ASSERT(_sphereVertexSlot != INVALID_VALUE32 && _sphereIndexSlot != INVALID_VALUE32,
                   "Failed to register the sphere buffers into the bindless set");

        _sphereVertexHandle = vertexBuffer->getHandle();
        _sphereIndexHandle = indexBuffer->getHandle();
        _sphereIndexCount = static_cast<uint32_t>(mesh._indices.size());
    }

    void createHandInput()
    {
        const bool forceCursor = gv_hand_input.get() == "cursor";
        if (!forceCursor)
        {
            std::unique_ptr<HandInputSource> platformSource(createPlatformHandInput());
            std::string error;
            if (platformSource != nullptr && platformSource->start(&error))
            {
                _handInput = std::move(platformSource);
                return;
            }
            if (platformSource != nullptr)
            {
                _handInputFallbackReason = error;
                VKM_DEBUG_LOG(("Camera hand input unavailable, falling back to the cursor: " + error).c_str());
            }
            else
            {
                _handInputFallbackReason = "no camera hand tracking on this platform";
            }
        }
        else
        {
            _handInputFallbackReason = "forced by --gv_hand_input=cursor";
        }

        std::unique_ptr<HandInputSource> cursorSource(createMouseHandInput(&_engine->getInputHandler()));
        std::string error;
        if (!cursorSource->start(&error))
        {
            VKM_DEBUG_ERROR(("Failed to start the cursor hand input: " + error).c_str());
            return;
        }
        _handInput = std::move(cursorSource);
    }

    void acquireCameraFrame()
    {
        if (!_handInput->tryAcquireFrame(&_cameraFrame))
        {
            return;
        }
        if (_cameraFrame._width == 0 || _cameraFrame._height == 0)
        {
            return;
        }

        VkmDriverBase* driver = _engine->getDriver();
        if (_cameraTexture == VKM_INVALID_RESOURCE_HANDLE)
        {
            VkmTextureInfo textureInfo{};
            textureInfo._flags = VkmResourceCreateInfo::AllowShaderRead | VkmResourceCreateInfo::AllowTransferDst;
            textureInfo._extent = glm::uvec3(_cameraFrame._width, _cameraFrame._height, 1);
            textureInfo._numMipLevels = 1;
            textureInfo._numArrayLayers = 1;
            // The capture's BGRA bytes go in untouched; hand_background.hlsl swizzles them back.
            textureInfo._format = VkmFormat::R8G8B8A8_UNORM;
            textureInfo._debugName = "HandInteractionCameraFrame";

            VkmTexture* texture = driver->newTexture(textureInfo);
            if (texture == nullptr)
            {
                VKM_DEBUG_ERROR("Failed to create the camera frame texture");
                return;
            }
            _cameraTexture = texture->getHandle();

            _cameraTextureSlot = driver->getBindlessResourceManager()->registerTexture(_cameraTexture);
            if (_cameraTextureSlot == INVALID_VALUE32)
            {
                VKM_DEBUG_ERROR("Failed to register the camera frame texture into the bindless set");
                return;
            }
        }

        if (!driver->uploadToTexture(_cameraTexture, _cameraFrame._pixels.data(), _cameraFrame._pixels.size()))
        {
            VKM_DEBUG_ERROR("Failed to upload a camera frame");
        }
    }

    void acquirePose(float deltaTime)
    {
        HandPose pose;
        if (_handInput->tryAcquirePose(&pose))
        {
            _rawPose = pose;
            _rawPoseAgeSeconds = 0.0f;
            return;
        }

        // Detection runs asynchronously and drops frames under load, so a gap of a frame or two is
        // normal and the last pose stays live across it.
        _rawPoseAgeSeconds += deltaTime;
        if (_rawPoseAgeSeconds > kPoseMaxAgeSeconds)
        {
            _rawPose._valid = false;
        }
    }

    void updateSmoothedPose(float deltaTime)
    {
        if (!_rawPose._valid)
        {
            _smoothedPose._valid = false;
            return;
        }

        if (!_smoothedPose._valid)
        {
            // Nothing to blend from: a fresh detection starts where it was seen, not where the
            // last one ended, or the hand would visibly slide in from its previous position.
            _smoothedPose = _rawPose;
            return;
        }

        const float alpha = 1.0f - std::exp(-deltaTime / kPoseSmoothingSeconds);
        for (uint32_t i = 0; i < kHandJointCount; ++i)
        {
            _smoothedPose._joints[i] += (_rawPose._joints[i] - _smoothedPose._joints[i]) * alpha;
            _smoothedPose._confidence[i] = _rawPose._confidence[i];
        }
        _smoothedPose._valid = true;
    }

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
        depthInfo._debugName = "HandInteractionDepthBuffer";

        VkmTexture* depthTexture = driver->newTexture(depthInfo);
        if (depthTexture == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create the depth buffer");
            return;
        }

        _depthTexture = depthTexture->getHandle();
        _depthExtent = extent;
    }

    void recordBackgroundPass(VkmRenderGraph* renderGraph, VkmResourceHandle backBuffer, const glm::uvec2& extent)
    {
        VkmFrameBufferDescriptor frameBufferDesc;
        frameBufferDesc._renderPass._colorAttachmentCount = 1;
        frameBufferDesc._renderPass._colorAttachments[0]._attachmentId = 0;
        frameBufferDesc._renderPass._colorAttachments[0]._loadAction = VkmLoadAction::Clear;
        frameBufferDesc._renderPass._colorAttachments[0]._storeAction = VkmStoreAction::Store;
        frameBufferDesc._renderPass._colorAttachments[0]._clearColors[0] = 0.06f;
        frameBufferDesc._renderPass._colorAttachments[0]._clearColors[1] = 0.07f;
        frameBufferDesc._renderPass._colorAttachments[0]._clearColors[2] = 0.09f;
        frameBufferDesc._renderPass._colorAttachments[0]._clearColors[3] = 1.0f;
        frameBufferDesc._width = extent.x;
        frameBufferDesc._height = extent.y;
        frameBufferDesc._colorAttachments[0] = backBuffer;

        VkmRenderGraphicsSubGraph* subGraph = renderGraph->beginGraphicsSubGraph(frameBufferDesc, "HandBackgroundPass");

        // No camera behind this source, or no frame delivered yet: the clear is the background.
        if (_cameraTextureSlot == INVALID_VALUE32 || _backgroundPso == nullptr)
        {
            return;
        }

        subGraph->addReferencedResource(_cameraTexture, VkmResourceAccess::ShaderSampledRead);

        VkmPipelineStateBase* pipeline = _backgroundPso;
        const BackgroundPushConstants pushConstants{ _cameraTextureSlot };
        subGraph->setRenderCallback([pipeline, pushConstants](VkmCommandBufferBase* commandBuffer) {
            commandBuffer->bindPipeline(pipeline);
            commandBuffer->setPushConstants(&pushConstants, sizeof(pushConstants));
            commandBuffer->draw(3, 1, 0, 0);
        });
    }

    void recordSpherePass(VkmRenderGraph* renderGraph, VkmResourceHandle backBuffer, const glm::uvec2& extent)
    {
        if (_spherePso == nullptr || _sphereIndexCount == 0 || _depthTexture == VKM_INVALID_RESOURCE_HANDLE)
        {
            return;
        }

        VkmFrameBufferDescriptor frameBufferDesc;
        frameBufferDesc._renderPass._colorAttachmentCount = 1;
        frameBufferDesc._renderPass._colorAttachments[0]._attachmentId = 0;
        // The background pass already wrote this attachment, so loading is what keeps it.
        frameBufferDesc._renderPass._colorAttachments[0]._loadAction = VkmLoadAction::Load;
        frameBufferDesc._renderPass._colorAttachments[0]._storeAction = VkmStoreAction::Store;
        frameBufferDesc._width = extent.x;
        frameBufferDesc._height = extent.y;
        frameBufferDesc._colorAttachments[0] = backBuffer;

        VkmDepthStencilAttachmentDescriptor depthDesc{};
        depthDesc._attachmentId = 0;
        depthDesc._loadAction = VkmLoadAction::Clear;
        depthDesc._storeAction = VkmStoreAction::Store;
        depthDesc._clearDepth = 1.0f;
        depthDesc._clearStencil = 0;
        frameBufferDesc._renderPass._depthStencilAttachment = depthDesc;
        frameBufferDesc._depthStencilAttachment = _depthTexture;

        std::array<SpherePushConstants, kMaxSphereDraws> draws{};
        const uint32_t drawCount = collectSphereDraws(extent, &draws);

        VkmRenderGraphicsSubGraph* subGraph = renderGraph->beginGraphicsSubGraph(frameBufferDesc, "HandSpherePass");
        subGraph->addReferencedResource(_sphereVertexHandle, VkmResourceAccess::ShaderStorageRead);
        subGraph->addReferencedResource(_sphereIndexHandle, VkmResourceAccess::ShaderStorageRead);

        VkmPipelineStateBase* pipeline = _spherePso;
        const uint32_t indexCount = _sphereIndexCount;
        subGraph->setRenderCallback([pipeline, draws, drawCount, indexCount](VkmCommandBufferBase* commandBuffer) {
            commandBuffer->bindPipeline(pipeline);
            for (uint32_t i = 0; i < drawCount; ++i)
            {
                commandBuffer->setPushConstants(&draws[i], sizeof(SpherePushConstants));
                commandBuffer->draw(indexCount, 1, 0, 0);
            }
        });
    }

    /*
    * @brief Fills the per-draw push constants for the ball and, when shown, the hand proxies.
    * @return How many entries of `outDraws` were written.
    */
    uint32_t collectSphereDraws(const glm::uvec2& extent, std::array<SpherePushConstants, kMaxSphereDraws>* outDraws) const
    {
        const float aspect = static_cast<float>(extent.x) / static_cast<float>(extent.y);
        const float halfHeight = kPlaneDistance * std::tan(kFovYRadians * 0.5f);
        const float worldScale = simToWorldScale(halfHeight, aspect);

        uint32_t count = 0;
        auto push = [&](const glm::vec2& simPosition, float simRadius, const glm::vec3& color) {
            const glm::vec3 world = simToWorld(simPosition, halfHeight, aspect, kPlaneDistance);
            SpherePushConstants& draw = (*outDraws)[count++];
            draw.centerAndRadius[0] = world.x;
            draw.centerAndRadius[1] = world.y;
            draw.centerAndRadius[2] = world.z;
            draw.centerAndRadius[3] = simRadius * worldScale;
            draw.color[0] = color.r;
            draw.color[1] = color.g;
            draw.color[2] = color.b;
            draw.color[3] = 1.0f;
            draw.vertexBufferIndex = _sphereVertexSlot;
            draw.indexBufferIndex = _sphereIndexSlot;
        };

        push(_ball._position, _ball._radius, glm::vec3(0.95f, 0.45f, 0.25f));

        if (_showHandProxies)
        {
            for (uint32_t i = 0; i < _handColliders._count; ++i)
            {
                const HandCollider& collider = _handColliders._colliders[i];
                push(collider._center, collider._radius, glm::vec3(0.25f, 0.75f, 0.95f));
            }
        }

        return count;
    }

    void drawUi()
    {
#if defined(VKM_ENABLE_IMGUI)
        ImGui::Begin("Hand Interaction");

        ImGui::Text("Source: %s", _handInput != nullptr ? _handInput->getName() : "none");
        if (!_handInputFallbackReason.empty())
        {
            ImGui::TextWrapped("Fallback: %s", _handInputFallbackReason.c_str());
        }
        ImGui::Text("Hand: %s", _smoothedPose._valid ? "tracked" : "not detected");
        if (_smoothedPose._valid)
        {
            float meanConfidence = 0.0f;
            for (uint32_t i = 0; i < kHandJointCount; ++i)
            {
                meanConfidence += _smoothedPose._confidence[i];
            }
            ImGui::Text("Mean confidence: %.2f", meanConfidence / static_cast<float>(kHandJointCount));
        }

        ImGui::Separator();
        ImGui::Text("Ball: (%.3f, %.3f)  speed %.3f",
                    _ball._position.x, _ball._position.y, glm::length(_ball._velocity));
        ImGui::SliderFloat("Gravity", &_simParams._gravity, -1.0f, 1.5f);
        ImGui::SliderFloat("Damping", &_simParams._linearDamping, 0.0f, 3.0f);
        ImGui::SliderFloat("Wall bounce", &_simParams._wallRestitution, 0.0f, 1.0f);
        ImGui::SliderFloat("Hand bounce", &_simParams._handRestitution, 0.0f, 1.0f);
        ImGui::SliderFloat("Ball radius", &_ball._radius, 0.02f, 0.20f);
        ImGui::Checkbox("Show hand proxies", &_showHandProxies);
        if (ImGui::Button("Reset ball"))
        {
            _ball._position = glm::vec2(0.5f, 0.2f);
            _ball._velocity = glm::vec2(0.0f, 0.0f);
        }

        ImGui::End();
#endif
    }

    VkmEngine* _engine{ nullptr };
    VkmPipelineStateBase* _backgroundPso{ nullptr };
    VkmPipelineStateBase* _spherePso{ nullptr };

    VkmCamera _camera;

    VkmResourceHandle _sphereVertexHandle{ VKM_INVALID_RESOURCE_HANDLE };
    VkmResourceHandle _sphereIndexHandle{ VKM_INVALID_RESOURCE_HANDLE };
    uint32_t _sphereVertexSlot{ INVALID_VALUE32 };
    uint32_t _sphereIndexSlot{ INVALID_VALUE32 };
    uint32_t _sphereIndexCount{ 0 };

    VkmResourceHandle _depthTexture{ VKM_INVALID_RESOURCE_HANDLE };
    glm::uvec2 _depthExtent{ 0, 0 };

    std::unique_ptr<HandInputSource> _handInput;
    std::string _handInputFallbackReason;
    CameraFrame _cameraFrame;
    VkmResourceHandle _cameraTexture{ VKM_INVALID_RESOURCE_HANDLE };
    uint32_t _cameraTextureSlot{ INVALID_VALUE32 };

    HandPose _rawPose;
    HandPose _smoothedPose;
    float _rawPoseAgeSeconds{ 0.0f };
    HandColliders _handColliders;

    BallState _ball;
    BallSimParams _simParams;
    bool _showHandProxies{ true };
};

int main(int argc, char* argv[])
{
    VkmApplication app;

    int ret = app.entryPoint(new HandInteractionApplication(), argc, argv);
    app.destroy();

    return ret;
}
