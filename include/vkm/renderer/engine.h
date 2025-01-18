// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/platform/common/app_delegate.h>
#include <vkm/platform/common/window.h>
#include <memory>

namespace vkm
{
    class VkmDriverBase;
    class VkmTexture;
    class VkmSwapChainBase;

    struct VkmEngineLaunchOptions
    {
        bool enableValidationLayer;
    };
    constexpr const VkmEngineLaunchOptions DEFAULT_ENGINE_LAUNCH_OPTIONS = { true };

    /*
    * @brief Engine base class
    * @details manage whole engine lifecycle and drive the render driver and other modules
    */
    class VkmEngine
    {
    public:
        VkmEngine(VkmDriverBase* driver);
        ~VkmEngine();

        /*
        * @brief Initialize engine
        * @details initialize logger manager and other modules
        */
        bool initialize(AppDelegate* appDelegate, VkmEngineLaunchOptions options = DEFAULT_ENGINE_LAUNCH_OPTIONS);

        /*
        * @brief Run engine loop
        * @details run main loop of engine
        */
        void update(const double currentUpdateTime);

        /*
        * @brief Destroy engine
        * @details destroy all modules and logger manager
        */
        void destroy();

        /*
        * @brief Add swapchain to engine
        */
        void addSwapChain(const VkmWindowInfo& windowInfo);
        
    public:
        /*
         * @brief returns engine's main swapchain created
         */
        inline VkmSwapChainBase* getMainSwapChain() { return _mainSwapChain; }
        
    public:

        /*
        * @brief Parse engine launch options from command line arguments
        */
        static VkmEngineLaunchOptions parseEngineLaunchOptions(int argc, char* argv[]);

    private:
        VkmDriverBase* _driver;
        double _lastUpdateTime;

        VkmSwapChainBase* _mainSwapChain {nullptr}; // main swapchain. engine should have multiple swapchains but at now, only one swapchain is supported.

    private:
        std::unique_ptr<AppDelegate> _appDelegate;
    };
}
