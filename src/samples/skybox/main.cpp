#include <cxxopts.hpp>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

#include <vkm/base/common.h>
#include <vkm/base/global_variable.h>
#include <vkm/platform/common/app_delegate.h>
#include <vkm/platform/common/input_codes.h>
#include <vkm/platform/common/input_handler.h>
#include <vkm/renderer/backend/common/bindless_resource_manager.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/command_queue.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/swapchain.h>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/engine.h>
#include <vkm/renderer/scene/image_loader.h>

#if defined(VKM_PLATFORM_WINDOWS)
#include <vkm/platform/windows/application.h>
#elif defined(VKM_PLATFORM_LINUX)
#include <vkm/platform/linux/application.h>
#else // defined(VKM_PLATFORM_WINDOWS)
#include <vkm/platform/apple/application.h>
#endif // defined(VKM_PLATFORM_WINDOWS)

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <string>

using namespace vkm;

// Directory holding the six cube faces: ./skybox --gv_cubemap_dir=/path/to/faces/
// When the faces are missing the sample falls back to a generated cubemap, so it always has
// something to draw.
VKM_GLOBAL_VARIABLE(std::string, gv_cubemap_dir, std::string(RESOURCES_DIR) + "Skyboxes/default/");

namespace
{
    // Cube faces in VkmTextureType::Cube's array-layer order.
    constexpr std::array<const char*, kVkmCubeFaceCount> kFaceFileNames = {
        "posx.png", "negx.png", "posy.png", "negy.png", "posz.png", "negz.png",
    };

    /*
    * Fallback face colors, one per layer in the same order. Deliberately six *different*
    * hues rather than a pretty gradient: with distinct colors a face-order or orientation
    * mistake is obvious the moment the sample runs, which is the whole point of having a
    * built-in cubemap. Opposite faces are complementary (+X red / -X cyan, +Y green / -Y
    * magenta, +Z blue / -Z yellow) so a swapped pair is equally obvious.
    */
    constexpr std::array<std::array<uint8_t, 4>, kVkmCubeFaceCount> kFallbackFaceColors = {{
        {{255, 64, 64, 255}},   // +X red
        {{64, 255, 255, 255}},  // -X cyan
        {{64, 255, 64, 255}},   // +Y green
        {{255, 64, 255, 255}},  // -Y magenta
        {{64, 64, 255, 255}},   // +Z blue
        {{255, 255, 64, 255}},  // -Z yellow
    }};

    constexpr uint32_t kFallbackFaceExtent = 64;

    // Mirrors PushConstants in skybox.hlsl.
    struct SkyboxPushConstants
    {
        glm::mat4 _inverseViewRotationProjection;
        uint32_t _cubemapSlot;
    };
    static_assert(sizeof(SkyboxPushConstants) <= kVkmBindlessPushConstantSize,
                  "The push constants must fit the engine-global push-constant range");

    // Look-around-only camera: the skybox is infinitely distant, so position is irrelevant
    // and only the view rotation reaches the shader. Left-drag looks around.
    class LookCamera
    {
    public:
        void update(VkmInputHandler& input)
        {
            if (input.isMouseButtonDown(VkmMouseButton::Left))
            {
                _yaw += static_cast<float>(input.getCursorDeltaX()) * 0.005f;
                // Just short of the poles, where the up vector would degenerate.
                _pitch = std::clamp(_pitch + static_cast<float>(input.getCursorDeltaY()) * 0.005f, -1.5f, 1.5f);
            }
        }

        /*
        * @brief Inverse of projection * view-rotation, which is what the vertex shader needs
        * to turn a clip-space corner straight into a look direction.
        * @details The view's translation is dropped (mat3 -> mat4) before inverting, so the
        * unprojected result is already a direction and the sky cannot be "moved through".
        */
        glm::mat4 getInverseViewRotationProjection(uint32_t width, uint32_t height) const
        {
            const glm::vec3 forward{
                std::cos(_pitch) * std::sin(_yaw),
                std::sin(_pitch),
                std::cos(_pitch) * std::cos(_yaw),
            };
            const glm::mat4 view = glm::lookAtRH(glm::vec3(0.0f), forward, glm::vec3(0.0f, 1.0f, 0.0f));

            const float aspect = height > 0 ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
            // *_ZO: the engine's clip space is +Y-up with a [0,1] depth range on every
            // backend (the Vulkan backend's -fvk-invert-y handles its inverted NDC).
            const glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(60.0f), aspect, 0.1f, 10.0f);

            return glm::inverse(projection * glm::mat4(glm::mat3(view)));
        }

    private:
        float _yaw = 0.0f;
        float _pitch = 0.0f;
    };

    // Six generated single-color faces, used when no image files are present.
    std::vector<VkmImageData> buildFallbackFaces()
    {
        std::vector<VkmImageData> faces(kVkmCubeFaceCount);
        for (size_t face = 0; face < kVkmCubeFaceCount; ++face)
        {
            const std::array<uint8_t, 4>& color = kFallbackFaceColors[face];

            VkmImageData& image = faces[face];
            image._width = kFallbackFaceExtent;
            image._height = kFallbackFaceExtent;
            image._pixels.resize(static_cast<size_t>(kFallbackFaceExtent) * kFallbackFaceExtent * color.size());
            for (size_t texel = 0; texel < image._pixels.size(); texel += color.size())
            {
                std::copy(color.begin(), color.end(), image._pixels.begin() + texel);
            }
        }
        return faces;
    }

    /*
    * Loads the six faces from `directory`, or returns the generated fallback when any of
    * them is missing. All-or-nothing on purpose: a half-loaded cubemap with three real faces
    * and three generated ones would be far more confusing than either alternative.
    */
    std::vector<VkmImageData> loadFaces(const std::string& directory, std::string* outSourceDescription)
    {
        std::vector<VkmImageData> faces(kVkmCubeFaceCount);
        for (size_t face = 0; face < kVkmCubeFaceCount; ++face)
        {
            const std::filesystem::path path = std::filesystem::path(directory) / kFaceFileNames[face];

            std::string error;
            if (!loadImageFromFile(path.string(), &faces[face], &error))
            {
                VKM_DEBUG_INFO(("Falling back to the generated cubemap: " + error).c_str());
                *outSourceDescription = "generated (no faces under '" + directory + "')";
                return buildFallbackFaces();
            }
        }

        // A cube texture needs one square extent shared by all six faces.
        for (size_t face = 1; face < kVkmCubeFaceCount; ++face)
        {
            if (faces[face]._width != faces[0]._width || faces[face]._height != faces[0]._height)
            {
                VKM_DEBUG_ERROR("Cube faces must all share one extent; falling back to the generated cubemap");
                *outSourceDescription = "generated (mismatched face extents)";
                return buildFallbackFaces();
            }
        }
        if (faces[0]._width != faces[0]._height)
        {
            VKM_DEBUG_ERROR("Cube faces must be square; falling back to the generated cubemap");
            *outSourceDescription = "generated (non-square faces)";
            return buildFallbackFaces();
        }

        *outSourceDescription = directory;
        return faces;
    }
} // namespace

class SkyboxApplication : public AppDelegate
{
public:
    SkyboxApplication() = default;
    virtual ~SkyboxApplication() = default;

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
        _pso = manager->getPipelineState("skybox_pso[default]", VkmPipelineStateOrigin::User);
        VKM_ASSERT(_pso != nullptr, "Failed to create skybox_pso[default]");

        loadCubemap();
    }

    virtual void preShutdown() override final
    {
        if (_engine == nullptr || _cubemap == VKM_INVALID_RESOURCE_HANDLE)
        {
            return;
        }

        VkmDriverBase* driver = _engine->getDriver();
        if (_cubemapSlot != INVALID_VALUE32)
        {
            driver->getBindlessResourceManager()->unregisterTexture(_cubemapSlot);
            _cubemapSlot = INVALID_VALUE32;
        }
        driver->getRenderResourcePool()->releaseResource(_cubemap);
        _cubemap = VKM_INVALID_RESOURCE_HANDLE;
    }

    virtual void update(const double deltaTime) override final
    {
        (void)deltaTime;
        _camera.update(_engine->getInputHandler());
    }

    virtual void render(uint32_t windowIndex, VkmRenderGraph* renderGraph, VkmResourceHandle backBuffer) override final
    {
        VkmSwapChainBase* swapChain = _engine->getSwapChain(windowIndex);
        const glm::uvec2 extent = swapChain->getExtent();

        VkmFrameBufferDescriptor frameBufferDesc;
        frameBufferDesc._renderPass._colorAttachmentCount = 1;
        frameBufferDesc._renderPass._colorAttachments[0]._attachmentId = 0;
        // The skybox covers every pixel, so the clear only matters on the frames before the
        // cubemap is ready.
        frameBufferDesc._renderPass._colorAttachments[0]._loadAction = VkmLoadAction::Clear;
        frameBufferDesc._renderPass._colorAttachments[0]._storeAction = VkmStoreAction::Store;
        frameBufferDesc._renderPass._colorAttachments[0]._clearColors[0] = 0.08f;
        frameBufferDesc._renderPass._colorAttachments[0]._clearColors[1] = 0.09f;
        frameBufferDesc._renderPass._colorAttachments[0]._clearColors[2] = 0.11f;
        frameBufferDesc._renderPass._colorAttachments[0]._clearColors[3] = 1.0f;

        frameBufferDesc._width = extent.x;
        frameBufferDesc._height = extent.y;
        frameBufferDesc._colorAttachments[0] = backBuffer;

        auto graphicsSubGraph = renderGraph->beginGraphicsSubGraph(frameBufferDesc, "SkyboxPass");
        if (_pso == nullptr || _cubemapSlot == INVALID_VALUE32)
        {
            return; // still a valid frame: the clear alone runs.
        }
        graphicsSubGraph->addReferencedResource(_cubemap);

        SkyboxPushConstants pushConstants{};
        pushConstants._inverseViewRotationProjection = _camera.getInverseViewRotationProjection(extent.x, extent.y);
        pushConstants._cubemapSlot = _cubemapSlot;

        VkmPipelineStateBase* pso = _pso;
        graphicsSubGraph->setRenderCallback([pso, pushConstants](VkmCommandBufferBase* commandBuffer) {
            commandBuffer->bindPipeline(pso);
            commandBuffer->setPushConstants(&pushConstants, sizeof(SkyboxPushConstants));
            // Three vertices, no buffers: the shader generates the covering triangle.
            commandBuffer->draw(3, 1, 0, 0);
        });
    }

    virtual const char* getAppName() const override final
    {
        return "SkyboxApplication";
    }

private:
    /*
    * Builds the cube texture and publishes it into the bindless texture array. Synchronous
    * and stalling by nature -- uploadToTexture blocks per face -- but this runs once at
    * startup, the same tradeoff model_viewer's scene load makes.
    */
    void loadCubemap()
    {
        VkmDriverBase* driver = _engine->getDriver();

        std::string sourceDescription;
        const std::vector<VkmImageData> faces = loadFaces(gv_cubemap_dir.get(), &sourceDescription);

        VkmTextureInfo textureInfo{};
        textureInfo._flags = static_cast<VkmResourceCreateInfo>(
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowShaderRead) |
            static_cast<uint32_t>(VkmResourceCreateInfo::AllowTransferDst));
        textureInfo._extent = glm::uvec3(faces[0]._width, faces[0]._height, 1);
        textureInfo._numMipLevels = 1;
        textureInfo._numArrayLayers = kVkmCubeFaceCount;
        textureInfo._format = VkmFormat::R8G8B8A8_UNORM;
        textureInfo._type = VkmTextureType::Cube;
        textureInfo._debugName = "SkyboxCubemap";

        VkmTexture* cubemap = driver->newTexture(textureInfo);
        if (cubemap == nullptr)
        {
            VKM_DEBUG_ERROR("Failed to create the skybox cube texture");
            return;
        }
        _cubemap = cubemap->getHandle();

        for (uint32_t face = 0; face < kVkmCubeFaceCount; ++face)
        {
            if (!driver->uploadToTexture(_cubemap, faces[face]._pixels.data(), faces[face].getByteSize(), 0, face))
            {
                VKM_DEBUG_ERROR("Failed to upload a skybox cube face");
                return;
            }
        }

        _cubemapSlot = driver->getBindlessResourceManager()->registerTexture(_cubemap);
        if (_cubemapSlot == INVALID_VALUE32)
        {
            VKM_DEBUG_ERROR("Failed to register the skybox cubemap into the bindless texture array");
            return;
        }

        VKM_DEBUG_LOG(("Loaded a " + std::to_string(faces[0]._width) + "x" + std::to_string(faces[0]._height) +
                       " cubemap from " + sourceDescription).c_str());
    }

private:
    VkmEngine* _engine{nullptr};
    VkmPipelineStateBase* _pso{nullptr};
    LookCamera _camera;
    VkmResourceHandle _cubemap{VKM_INVALID_RESOURCE_HANDLE};
    uint32_t _cubemapSlot{INVALID_VALUE32};
};

int main(int argc, char* argv[])
{
    VkmApplication app;

    int ret = app.entryPoint(new SkyboxApplication(), argc, argv);
    app.destroy();

    return ret;
}
