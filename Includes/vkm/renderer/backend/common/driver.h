// Copyright (c) 2024 Snowapril

#pragma once

#include <vkm/base/common.h>

namespace vkm
{
    struct VkmEngineLaunchOptions;

    /*
    * @brief renderer backend driver base class
    * @details manage whole engine lifecycle and drive the render driver and other modules
    */
    class VkmDriverBase
    {
    public:
        VkmDriverBase();
        ~VkmDriverBase();

        bool initialize(const VkmEngineLaunchOptions* options);
        void destroy();
        
    protected:
        virtual bool initializeInner(const VkmEngineLaunchOptions* options) = 0;
        virtual void destroyInner() = 0;

    private:
    };
}