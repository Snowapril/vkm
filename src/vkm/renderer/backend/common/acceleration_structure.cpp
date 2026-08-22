// Copyright (c) 2026 Snowapril

#include <vkm/renderer/backend/common/acceleration_structure.h>

namespace vkm
{
    VkmAccelerationStructure::VkmAccelerationStructure(VkmDriverBase* driver)
        : VkmRenderResource(driver)
    {
    }

    VkmAccelerationStructure::~VkmAccelerationStructure()
    {
    }

    bool VkmAccelerationStructure::initializeAccelerationStructureCommon(
        VkmResourceHandle handle, const VkmAccelerationStructureInfo& info)
    {
        if (!initializeCommon(handle))
        {
            return false;
        }

        _info = info;
        return true;
    }
} // namespace vkm
