// Copyright (c) 2024 Snowapril

#pragma once

#include <vkm/base/common.h>

namespace vkm
{
    /*
    * @brief renderer backend driver base class
    * @details manage whole engine lifecycle and drive the render driver and other modules
    */
    class VkmDriverBase
    {
    public:
        VkmDriverBase();
        ~VkmDriverBase();

    private:
    };
}