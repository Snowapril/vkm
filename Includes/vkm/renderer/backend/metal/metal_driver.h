// Copyright (c) 2024 Snowapril

#pragma once

#include <vkm/renderer/backend/common/driver.h>

class MTLDevice;

namespace vkm
{
    /*
    * @brief renderer backend driver base class
    * @details 
    */
    class VkmDriverMetal : public VkmDriverBase
    {
    public:
        VkmDriverMetal(MTLDevice* mtlDevice);
        ~VkmDriverMetal();

    private:
        MTLDevice* _mtlDevice;
    };
}