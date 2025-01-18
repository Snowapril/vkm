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

    virtual void onDriverInit() override final
    {
        VKM_DEBUG_LOG("TriangleApplication::onDriverInit");
    }

    virtual void onShutdown() override final
    {
        VKM_DEBUG_LOG("TriangleApplication::onShutdown");
    }

    virtual void onUpdate(const double deltaTime) override final
    {
        VKM_DEBUG_LOG("TriangleApplication::onUpdate");
    }

    virtual void onRender(VkmResourceHandle currentBackBuffer) override final
    {
        VKM_DEBUG_LOG("TriangleApplication::onRender");
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