// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/staging_buffer.h>

namespace vkm
{
    VkmStagingBuffer::VkmStagingBuffer(VkmDriverBase* driver)
        : VkmRenderResource(driver)
    {
    }

    VkmStagingBuffer::~VkmStagingBuffer()
    {
    }

    bool VkmStagingBuffer::initializeStagingBufferCommon(VkmResourceHandle handle, const VkmStagingBufferInfo& info)
    {
        if (!initializeCommon(handle))
        {
            return false;
        }

        _stagingBufferInfo = info;
        return true;
    }
} // namespace vkm
