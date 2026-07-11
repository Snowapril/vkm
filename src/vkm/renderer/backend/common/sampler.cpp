// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/sampler.h>

namespace vkm
{
    VkmSampler::VkmSampler(VkmDriverBase* driver)
        : VkmRenderResource(driver)
    {
    }

    VkmSampler::~VkmSampler()
    {
    }

    bool VkmSampler::initializeSamplerCommon(VkmResourceHandle handle, const VkmSamplerInfo& info)
    {
        if (!initializeCommon(handle))
        {
            return false;
        }

        _samplerInfo = info;
        return true;
    }
} // namespace vkm
