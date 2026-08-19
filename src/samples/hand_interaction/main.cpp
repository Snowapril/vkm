// Copyright (c) 2025 Snowapril
//
// A hand, seen through the camera, knocking a rendered sphere around.
//
// Per frame:
//   1. VkmVideoCaptureBase hands over the newest camera frame, which is uploaded into one texture
//      and drawn as the background, and is offered to VkmHandTrackerBase for detection.
//   2. The newest pose is mirrored to match the mirrored background, smoothed, differenced
//      against last frame's for velocity, and turned into six collision proxies -- five
//      fingertips and the palm (ball_sim.h).
//   3. The ball is advanced in fixed substeps against those proxies and the window edges.
//   4. The ball and, optionally, the proxies are drawn as unit spheres scaled and placed by push
//      constants, all from one mesh and one pipeline.
//
// The simulation is two-dimensional on purpose. Both plausible sources of a third dimension --
// monocular depth estimation, or inferring distance from the hand's apparent size -- are relative
// and noisy frame to frame, and feeding that into a collision test produces a ball that jitters
// and tunnels. Constraining everything to one plane in front of the camera costs nothing the
// interaction needs and makes it behave the same every time.
//
// Where no hand tracker exists -- everywhere but Apple, including the browser -- the cursor stands
// in for the hand and the camera background still runs.

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
#include <vkm/platform/common/hand_tracker.h>
#include <vkm/platform/common/input_handler.h>
#include <vkm/platform/common/video_capture.h>
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
#include "cursor_hand_tracker.h"
#include "sphere_mesh.h"

using namespace vkm;

// "cursor" forces the stand-in even where a hand tracker exists, which is the only way to
// exercise the fallback path on a machine whose camera tracking works.
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

    // Time constant of the pose smoothing. Long enough to swallow a detector's per-frame jitter,
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
        createSampler();
        createCapture();
        createHandTracker();
    }

    virtual void preShutdown() override final
    {
        if (_capture != nullptr)
        {
            _capture->stop();
            _capture.reset();
        }
        if (_handTracker != nullptr)
        {
            _handTracker->stop();
            _handTracker.reset();
        }
        _cursorTracker = nullptr;

        releaseBackgroundTable();

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

        if (_cursorTracker != nullptr)
        {
            _cursorTracker->setViewportSize(extent.x, extent.y);
        }

        acquireCameraFrame();
        acquirePose(step);

        const VkmHandPose previousPose = _smoothedPose;
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

        // AllowShaderWrite rather than AllowShaderRead: the latter maps to a uniform buffer, which
        // cannot back a bindless storage-buffer array element. AllowTransferSrc is what the
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

    void createSampler()
    {
        VkmSamplerInfo samplerInfo{};
        samplerInfo._debugName = "HandInteractionSampler";
        VkmSampler* sampler = _engine->getDriver()->newSampler(samplerInfo);
        if (sampler == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create the camera frame sampler");
            return;
        }
        _sampler = sampler->getHandle();
    }

    void createCapture()
    {
        std::unique_ptr<VkmVideoCaptureBase> capture(vkmCreateVideoCapture());
        if (capture == nullptr)
        {
            _cameraStatus = "no camera capture on this platform";
            return;
        }

        std::string error;
        if (!capture->start(&error))
        {
            _cameraStatus = error;
            VKM_DEBUG_LOG(("Camera unavailable: " + error).c_str());
            return;
        }
        _capture = std::move(capture);
        _cameraStatus = "waiting for the first frame";
    }

    void createHandTracker()
    {
        const bool forceCursor = gv_hand_input.get() == "cursor";
        if (!forceCursor)
        {
            std::unique_ptr<VkmHandTrackerBase> tracker(vkmCreateHandTracker());
            std::string error;
            if (tracker != nullptr && tracker->start(&error))
            {
                _handTracker = std::move(tracker);
                return;
            }
            if (tracker != nullptr)
            {
                _trackerFallbackReason = error;
                VKM_DEBUG_LOG(("Hand tracking unavailable, falling back to the cursor: " + error).c_str());
            }
            else
            {
                _trackerFallbackReason = "no hand tracking on this platform";
            }
        }
        else
        {
            _trackerFallbackReason = "forced by --gv_hand_input=cursor";
        }

        // The cursor tracker already reports where the user sees the pointer, so unlike a camera
        // tracker its pose must not be mirrored.
        _mirrorPose = false;

        auto cursorTracker = std::make_unique<CursorHandTracker>(&_engine->getInputHandler());
        std::string error;
        if (!cursorTracker->start(&error))
        {
            VKM_DEBUG_ERROR(("Failed to start the cursor hand tracker: " + error).c_str());
            return;
        }
        _cursorTracker = cursorTracker.get();
        _handTracker = std::move(cursorTracker);
    }

    void acquireCameraFrame()
    {
        if (_capture == nullptr || !_capture->tryAcquireFrame(&_cameraFrame))
        {
            return;
        }
        if (_cameraFrame._width == 0 || _cameraFrame._height == 0)
        {
            return;
        }

        // Offered to the tracker before the upload: detection runs off-thread from here, so the
        // sooner it starts the fresher its answer is when it lands.
        if (_handTracker != nullptr)
        {
            _handTracker->submitFrame(_cameraFrame);
        }

        VkmDriverBase* driver = _engine->getDriver();
        if (_cameraTexture == VKM_INVALID_RESOURCE_HANDLE)
        {
            VkmTextureInfo textureInfo{};
            textureInfo._flags = VkmResourceCreateInfo::AllowShaderRead | VkmResourceCreateInfo::AllowTransferDst;
            textureInfo._extent = glm::uvec3(_cameraFrame._width, _cameraFrame._height, 1);
            textureInfo._numMipLevels = 1;
            textureInfo._numArrayLayers = 1;
            // The capture's own channel order, so its bytes go in untouched: BGRA from
            // AVFoundation, RGBA from a browser canvas.
            textureInfo._format = _cameraFrame._format;
            textureInfo._debugName = "HandInteractionCameraFrame";

            VkmTexture* texture = driver->newTexture(textureInfo);
            if (texture == nullptr)
            {
                VKM_DEBUG_ERROR("Failed to create the camera frame texture");
                return;
            }
            _cameraTexture = texture->getHandle();
            buildBackgroundTable();
        }

        if (!driver->uploadToTexture(_cameraTexture, _cameraFrame._pixels.data(), _cameraFrame._pixels.size()))
        {
            VKM_DEBUG_ERROR("Failed to upload a camera frame");
            return;
        }
        _cameraStatus.clear();
    }

    void buildBackgroundTable()
    {
        if (_backgroundPso == nullptr || _sampler == VKM_INVALID_RESOURCE_HANDLE)
        {
            return;
        }

        std::string error;
        // Set 2 rather than the bindless set: WGSL has no runtime-sized texture array, so this is
        // the only route that exists on every backend.
        _backgroundTable = _engine->getDriver()->newResourceTable(
            _backgroundPso, VkmResourceSetKind::PerPass,
            { { 0, _cameraTexture }, { 1, _sampler } }, &error);
        if (_backgroundTable == nullptr)
        {
            VKM_DEBUG_ERROR(("Failed to build the background resource table: " + error).c_str());
        }
    }

    void releaseBackgroundTable()
    {
        if (_backgroundTable != nullptr)
        {
            _backgroundTable->destroy();
            delete _backgroundTable;
            _backgroundTable = nullptr;
        }
    }

    void acquirePose(float deltaTime)
    {
        if (_handTracker == nullptr)
        {
            return;
        }

        VkmHandPose pose;
        if (_handTracker->tryAcquirePose(&pose))
        {
            // A tracker reports in image space; the background is drawn mirrored, so the pose is
            // mirrored to match and everything downstream works in what the user sees.
            if (_mirrorPose)
            {
                for (uint32_t i = 0; i < kVkmHandJointCount; ++i)
                {
                    pose._joints[i].x = 1.0f - pose._joints[i].x;
                }
            }
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
        for (uint32_t i = 0; i < kVkmHandJointCount; ++i)
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

        // No camera, or no frame delivered yet: the clear is the background.
        if (_backgroundTable == nullptr || _backgroundPso == nullptr)
        {
            return;
        }

        std::vector<VkmResourceAccessDeclaration> referenced;
        _backgroundTable->collectReferencedResources(&referenced);
        subGraph->addReferencedResources(referenced);

        VkmPipelineStateBase* pipeline = _backgroundPso;
        VkmResourceTableBase* table = _backgroundTable;
        subGraph->setRenderCallback([pipeline, table](VkmCommandBufferBase* commandBuffer) {
            commandBuffer->bindPipeline(pipeline);
            commandBuffer->bindResourceTable(table);
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

        ImGui::Text("Camera: %s", _capture != nullptr ? _capture->getName() : "none");
        if (!_cameraStatus.empty())
        {
            ImGui::TextWrapped("  %s", _cameraStatus.c_str());
        }
        ImGui::Text("Tracker: %s", _handTracker != nullptr ? _handTracker->getName() : "none");
        if (!_trackerFallbackReason.empty())
        {
            ImGui::TextWrapped("  %s", _trackerFallbackReason.c_str());
        }
        ImGui::Text("Hand: %s", _smoothedPose._valid ? "tracked" : "not detected");
        if (_smoothedPose._valid)
        {
            float meanConfidence = 0.0f;
            for (uint32_t i = 0; i < kVkmHandJointCount; ++i)
            {
                meanConfidence += _smoothedPose._confidence[i];
            }
            ImGui::Text("Mean confidence: %.2f", meanConfidence / static_cast<float>(kVkmHandJointCount));
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
        // On for a camera tracker, which reports in image space, and off for the cursor.
        ImGui::Checkbox("Mirror hand", &_mirrorPose);
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

    std::unique_ptr<VkmVideoCaptureBase> _capture;
    std::unique_ptr<VkmHandTrackerBase> _handTracker;
    // Non-owning; set only when _handTracker is the cursor stand-in, which is the one kind that
    // has to be told how large the window is.
    CursorHandTracker* _cursorTracker{ nullptr };
    std::string _cameraStatus;
    std::string _trackerFallbackReason;

    VkmVideoFrame _cameraFrame;
    VkmResourceHandle _cameraTexture{ VKM_INVALID_RESOURCE_HANDLE };
    VkmResourceHandle _sampler{ VKM_INVALID_RESOURCE_HANDLE };
    VkmResourceTableBase* _backgroundTable{ nullptr };

    VkmHandPose _rawPose;
    VkmHandPose _smoothedPose;
    float _rawPoseAgeSeconds{ 0.0f };
    HandColliders _handColliders;

    BallState _ball;
    BallSimParams _simParams;
    bool _showHandProxies{ true };
    bool _mirrorPose{ true };
};

int main(int argc, char* argv[])
{
    VkmApplication app;

    int ret = app.entryPoint(new HandInteractionApplication(), argc, argv);
    app.destroy();

    return ret;
}
