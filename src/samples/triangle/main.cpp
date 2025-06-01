#include <iostream>
#include <cxxopts.hpp>

#include <vkm/base/common.h>
#include <vkm/platform/common/app_delegate.h>

#if defined(VKM_PLATFORM_WINDOWS)
#include <vkm/platform/windows/application.h>
#else // defined(VKM_PLATFORM_WINDOWS)
#include <vkm/platform/apple/application.h>
#endif // defined(VKM_PLATFORM_WINDOWS)

using namespace vkm;

class TriangleApplication : public AppDelegate
{
public:
    TriangleApplication() = default;
    virtual ~TriangleApplication() = default;

    virtual void postDriverReady() override final
    {
        VKM_DEBUG_LOG("TriangleApplication::postDriverReady");
    }

    virtual void preShutdown() override final
    {
        VKM_DEBUG_LOG("TriangleApplication::preShutdown");
    }

    virtual void update(const double deltaTime) override final
    {
        VKM_DEBUG_LOG("TriangleApplication::update");
    }

    virtual void render(VkmRenderGraph* renderGraph, VkmResourceHandle backBuffer) override final
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

        frameBufferDesc._width = 800; // Set the width of the framebuffer
        frameBufferDesc._height = 600; // Set the height of the framebuffer
        frameBufferDesc._colorAttachments[0] = backBuffer; // Attach the back buffer

        auto graphicsSubGraph = renderGraph->beginGraphicsSubGraph(frameBufferDesc);
        (void)graphicsSubGraph;
    }

    virtual const char* getAppName() const override final
    {
        return "TriangleApplication";
    }

private:
};

int main(int argc, char* argv[])
{
    VkmApplication app;
    
    int ret = app.entryPoint(new TriangleApplication(), argc, argv);
    app.destroy();

    return ret;
}
