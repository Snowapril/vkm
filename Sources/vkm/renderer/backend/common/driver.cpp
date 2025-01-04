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

    bool VkmDriverBase::initialize()
    {
        return initializeInner();
    }

    void VkmDriverBase::destroy()
    {
        destroyInner();
    }
}