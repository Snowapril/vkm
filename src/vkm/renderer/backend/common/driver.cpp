// Copyright (c) 2024 Snowapril

#include <vkm/renderer/backend/common/driver.h>

namespace vkm
{
    VkmDriverBase::VkmDriverBase()
    {
    }

    VkmDriverBase::~VkmDriverBase()
    {
    }

    bool VkmDriverBase::initialize(const VkmEngineLaunchOptions* options)
    {
        return initializeInner(options);
    }

    void VkmDriverBase::destroy()
    {
        destroyInner();
    }
}