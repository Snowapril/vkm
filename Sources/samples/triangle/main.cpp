#include <iostream>
#include <cxxopts.hpp>

#include <vkm/base/common.h>

#if defined(VKM_PLATFORM_WINDOWS)
#include <vkm/platform/windows/application.h>
#else // defined(VKM_PLATFORM_WINDOWS)
#include <vkm/platform/apple/application.h>
#endif // defined(VKM_PLATFORM_WINDOWS)

using namespace vkm;

int main(int argc, char* argv[])
{
    VkmApplication app;
    
    int ret = app.entryPoint(argc, argv);
    app.destroy();

    return ret;
}