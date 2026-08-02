#include <iostream>
#include <cxxopts.hpp>

#include <vkm/base/common.h>
#include <vkm/platform/common/app_delegate.h>
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

class EmptyScreenApplication : public AppDelegate
{
public:
    EmptyScreenApplication() = default;
    virtual ~EmptyScreenApplication() = default;

    virtual void postDriverReady(VkmEngine* engine) override final
    {
        _engine = engine;
        VKM_DEBUG_LOG("EmptyScreenApplication::postDriverReady");
    }

    virtual void preShutdown() override final
    {
        VKM_DEBUG_LOG("EmptyScreenApplication::preShutdown");
    }

    virtual void update(const double deltaTime) override final
    {
        VKM_DEBUG_LOG("EmptyScreenApplication::update");
    }

    virtual void render(uint32_t windowIndex, VkmRenderGraph* renderGraph, VkmResourceHandle backBuffer) override final
    {
        VKM_DEBUG_LOG("EmptyScreenApplication::render");

        VkmFrameBufferDescriptor frameBufferDesc;
        frameBufferDesc._renderPass._colorAttachmentCount = 1;
        frameBufferDesc._renderPass._colorAttachments[0]._attachmentId = 0;
        frameBufferDesc._renderPass._colorAttachments[0]._loadAction = VkmLoadAction::Clear;
        frameBufferDesc._renderPass._colorAttachments[0]._storeAction = VkmStoreAction::Store;

        // Random clear color for each frame
        frameBufferDesc._renderPass._colorAttachments[0]._clearColors[0] = static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
        frameBufferDesc._renderPass._colorAttachments[0]._clearColors[1] = static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
        frameBufferDesc._renderPass._colorAttachments[0]._clearColors[2] = static_cast<float>(rand()) / static_cast<float>(RAND_MAX);
        frameBufferDesc._renderPass._colorAttachments[0]._clearColors[3] = 1.0f;


        // Read from the swapchain every frame rather than hardcoded: the window is resizable,
        // and on Vulkan these two become the render area and viewport.
        const glm::uvec2 extent = _engine->getSwapChain(windowIndex)->getExtent();
        frameBufferDesc._width = extent.x;
        frameBufferDesc._height = extent.y;
        frameBufferDesc._colorAttachments[0] = backBuffer; // Attach the back buffer

        auto graphicsSubGraph = renderGraph->beginGraphicsSubGraph(frameBufferDesc);
        (void)graphicsSubGraph;
    }

    virtual const char* getAppName() const override final
    {
        return "EmptyScreenApplication";
    }

private:
    VkmEngine* _engine{nullptr};
};

int main(int argc, char* argv[])
{
    VkmApplication app;
    
    int ret = app.entryPoint(new EmptyScreenApplication(), argc, argv);
    app.destroy();

    return ret;
}
