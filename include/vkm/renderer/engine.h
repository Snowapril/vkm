// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>

namespace vkm
{
    class VkmDriverBase;
    class VkmTexture;

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
        bool initialize(VkmEngineLaunchOptions options = DEFAULT_ENGINE_LAUNCH_OPTIONS);

        /*
        * @brief Run engine loop
        * @details run main loop of engine
        */
        void update(VkmTexture* backBuffer, const double currentUpdateTime);

        /*
        * @brief Destroy engine
        * @details destroy all modules and logger manager
        */
        void destroy();

        /*
        * @brief Parse engine launch options from command line arguments
        */
        static VkmEngineLaunchOptions parseEngineLaunchOptions(int argc, char* argv[]);

    private:
        VkmDriverBase* _driver;
        double _lastUpdateTime;
    };
}