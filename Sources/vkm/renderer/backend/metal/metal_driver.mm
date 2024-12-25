// Copyright (c) 2024 Snowapril

#include <vkm/renderer/backend/metal/metal_driver.h>

namespace vkm
{
    VkmDriverMetal::VkmDriverMetal(MTLDevice* mtlDevice)
        : VkmDriverBase(), _mtlDevice(mtlDevice)
    {
        
    }

    VkmDriverMetal::~VkmDriverMetal()
    {

    }
}