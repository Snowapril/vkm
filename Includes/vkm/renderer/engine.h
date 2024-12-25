// Copyright (c) 2024 Snowapril

#pragma once

#include <vkm/base/common.h>

namespace vkm
{
    class VkmDriverBase;
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
        bool initialize();

        /*
        * @brief Run engine loop
        * @details run main loop of engine
        */
        void runLoop();

        /*
        * @brief Destroy engine
        * @details destroy all modules and logger manager
        */
        void destroy();

    private:
        VkmDriverBase* _driver;
    };
}