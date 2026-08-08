#include <iostream>
#include <array>
#include <cxxopts.hpp>

#if defined(VKM_ENABLE_IMGUI)
#include <imgui.h>
#endif

#include <vkm/base/common.h>
#include <vkm/platform/common/app_delegate.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/bindless_resource_manager.h>
#include <vkm/renderer/backend/common/swapchain.h>
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

using namespace vkm;

namespace
{
    // Matches triangle.hlsl's VertexData struct, padded to match DXC's default (std430-like)
    // storage-buffer layout for -spirv targets: a float3 is 16-byte aligned, so the
    // following float4 must start at offset 16, not 12.
    struct TriangleVertex
    {
        float position[3];
        float _pad0;
        float color[4];
    };
}

class TriangleApplication : public AppDelegate
{
public:
    TriangleApplication() = default;
    virtual ~TriangleApplication() = default;

    virtual void postDriverReady(VkmEngine* engine) override final
    {
        VKM_DEBUG_LOG("TriangleApplication::postDriverReady");

        _engine = engine;

        VkmPipelineStateManager* manager = engine->getPipelineStateManager();
        std::string err;
        if (!manager->loadPipelineStatesFromDirectory(SAMPLE_DIR, SAMPLE_SHADER_CACHE_DIR, VkmPipelineStateOrigin::User, &err))
        {
            VKM_DEBUG_ERROR(("Failed to load sample pipeline states: " + err).c_str());
            return;
        }
        VkmPipelineStateBase* defaultPso = manager->getPipelineState("triangle_pso[default]", VkmPipelineStateOrigin::User);
        VKM_ASSERT(defaultPso != nullptr, "Failed to create triangle_pso[default]");
        // The wireframe variant is optional: not every backend supports wireframe fill
        // (WebGPU has no polygon-mode concept), so its PSO may not exist.
        VkmPipelineStateBase* wireframePso = manager->getPipelineState("triangle_pso[wireframe]", VkmPipelineStateOrigin::User);
        VKM_DEBUG_LOG(("Loaded PSO '" + defaultPso->getName() + "'" +
                       (wireframePso != nullptr ? " and '" + wireframePso->getName() + "'" : "")).c_str());
        _pso = defaultPso;

        // Written straight to SV_POSITION with no transform, so these are clip-space
        // coordinates: +Y is up (the engine convention, matching HLSL/D3D, Metal and WebGPU;
        // the Vulkan backend flips its viewport to match). Wound counter-clockwise to agree
        // with the "front_face": "counter_clockwise" declared in renderpass.json.
        const std::array<TriangleVertex, 3> vertices{
            TriangleVertex{{ 0.0f,  0.5f, 0.0f}, 0.0f, {1.0f, 0.0f, 0.0f, 1.0f}}, // top, red
            TriangleVertex{{-0.5f, -0.5f, 0.0f}, 0.0f, {0.0f, 0.0f, 1.0f, 1.0f}}, // bottom-left, blue
            TriangleVertex{{ 0.5f, -0.5f, 0.0f}, 0.0f, {0.0f, 1.0f, 0.0f, 1.0f}}, // bottom-right, green
        };
        const std::array<uint32_t, 3> indices{0, 1, 2};

        VkmDriverBase* driver = engine->getDriver();

        // AllowShaderWrite maps to VK_BUFFER_USAGE_STORAGE_BUFFER_BIT (see
        // toVkBufferUsageFlags) -- used here even though these buffers are shader-read-only,
        // since AllowShaderRead maps to UNIFORM_BUFFER_BIT, which can't back an
        // unbounded/bindless storage-buffer array element. AllowTransferSrc is required by
        // the WebGPU backend, whose registerBuffer() copies the contents into its bindless
        // mega-buffer (a no-op cost on Vulkan/Metal).
        VkmBufferInfo vertexBufferInfo{};
        vertexBufferInfo._flags = VkmResourceCreateInfo::AllowShaderWrite | VkmResourceCreateInfo::AllowTransferDst | VkmResourceCreateInfo::AllowTransferSrc;
        vertexBufferInfo._size = sizeof(vertices);
        vertexBufferInfo._debugName = "TriangleVertexBuffer";
        VkmBuffer* vertexBuffer = driver->newBuffer(vertexBufferInfo);
        VKM_ASSERT(vertexBuffer != nullptr, "Failed to create triangle vertex buffer");
        driver->uploadToBuffer(vertexBuffer->getHandle(), vertices.data(), sizeof(vertices));

        VkmBufferInfo indexBufferInfo{};
        indexBufferInfo._flags = VkmResourceCreateInfo::AllowShaderWrite | VkmResourceCreateInfo::AllowTransferDst | VkmResourceCreateInfo::AllowTransferSrc;
        indexBufferInfo._size = sizeof(indices);
        indexBufferInfo._debugName = "TriangleIndexBuffer";
        VkmBuffer* indexBuffer = driver->newBuffer(indexBufferInfo);
        VKM_ASSERT(indexBuffer != nullptr, "Failed to create triangle index buffer");
        driver->uploadToBuffer(indexBuffer->getHandle(), indices.data(), sizeof(indices));

        VkmBindlessResourceManagerBase* bindlessManager = driver->getBindlessResourceManager();
        _vertexBufferSlot = bindlessManager->registerBuffer(vertexBuffer->getHandle(), VkmBindlessArrayType::Buffer);
        _indexBufferSlot = bindlessManager->registerBuffer(indexBuffer->getHandle(), VkmBindlessArrayType::IndexBuffer);
        VKM_ASSERT(_vertexBufferSlot != UINT32_MAX && _indexBufferSlot != UINT32_MAX, "Failed to register triangle buffers into the bindless set");

        _vertexBufferHandle = vertexBuffer->getHandle();
        _indexBufferHandle = indexBuffer->getHandle();
    }

    virtual void preShutdown() override final
    {
        VKM_DEBUG_LOG("TriangleApplication::preShutdown");
    }

    virtual void update(const double deltaTime) override final
    {
        VKM_DEBUG_LOG("TriangleApplication::update");

#if defined(VKM_ENABLE_IMGUI)
        // Demonstrates that sample/app code can call plain ImGui:: functions during
        // update()/render() -- the engine owns NewFrame()/Render(), nothing else is needed.
        ImGui::ShowDemoWindow();
#endif
    }

    virtual void render(uint32_t windowIndex, VkmRenderGraph* renderGraph, VkmResourceHandle backBuffer) override final
    {
        VKM_DEBUG_LOG("TriangleApplication::render");

        VkmFrameBufferDescriptor frameBufferDesc;
        frameBufferDesc._renderPass._colorAttachmentCount = 1;
        frameBufferDesc._renderPass._colorAttachments[0]._attachmentId = 0;
        frameBufferDesc._renderPass._colorAttachments[0]._loadAction = VkmLoadAction::Clear;
        frameBufferDesc._renderPass._colorAttachments[0]._storeAction = VkmStoreAction::Store;
        frameBufferDesc._renderPass._colorAttachments[0]._clearColors[0] = 1.0f; // R
        frameBufferDesc._renderPass._colorAttachments[0]._clearColors[1] = 0.0f; // G
        frameBufferDesc._renderPass._colorAttachments[0]._clearColors[2] = 0.0f; // B
        frameBufferDesc._renderPass._colorAttachments[0]._clearColors[3] = 1.0f; // A

        // Read from the swapchain every frame rather than hardcoded: the window is resizable,
        // and on Vulkan these two become the render area and viewport, so a stale size renders
        // the triangle into a corner of the back buffer.
        const glm::uvec2 extent = _engine->getSwapChain(windowIndex)->getExtent();
        frameBufferDesc._width = extent.x;
        frameBufferDesc._height = extent.y;
        frameBufferDesc._colorAttachments[0] = backBuffer; // Attach the back buffer

        auto graphicsSubGraph = renderGraph->beginGraphicsSubGraph(frameBufferDesc, "TrianglePass");

        // The triangle reads its vertex/index data through the bindless set, so declare
        // those buffers as referenced resources for GPU-usage tracking and debug tooling
        // (e.g. the render graph capture's inputs list / buffer viewer).
        graphicsSubGraph->addReferencedResource(_vertexBufferHandle, VkmResourceAccess::ShaderStorageRead);
        graphicsSubGraph->addReferencedResource(_indexBufferHandle, VkmResourceAccess::ShaderStorageRead);

        VkmPipelineStateBase* pso = _pso;
        const uint32_t pushConstants[2] = {_vertexBufferSlot, _indexBufferSlot};
        graphicsSubGraph->setRenderCallback([pso, pushConstants](VkmCommandBufferBase* commandBuffer) {
            commandBuffer->bindPipeline(pso);
            commandBuffer->setPushConstants(pushConstants, sizeof(pushConstants));
            commandBuffer->draw(3, 1, 0, 0);
        });
    }

    virtual const char* getAppName() const override final
    {
        return "TriangleApplication";
    }

private:
    VkmEngine* _engine{nullptr};
    VkmPipelineStateBase* _pso{nullptr};
    uint32_t _vertexBufferSlot{UINT32_MAX};
    uint32_t _indexBufferSlot{UINT32_MAX};
    VkmResourceHandle _vertexBufferHandle{VKM_INVALID_RESOURCE_HANDLE};
    VkmResourceHandle _indexBufferHandle{VKM_INVALID_RESOURCE_HANDLE};
};

int main(int argc, char* argv[])
{
    VkmApplication app;

    int ret = app.entryPoint(new TriangleApplication(), argc, argv);
    app.destroy();

    return ret;
}
