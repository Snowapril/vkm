// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/buffer.h>

namespace vkm
{
    VkmBuffer::VkmBuffer(VkmDriverBase* driver)
        : VkmRenderResource(driver)
    {
    }

    VkmBuffer::~VkmBuffer()
    {
    }

    bool VkmBuffer::initializeBufferCommon(VkmResourceHandle handle, const VkmBufferInfo& info)
    {
        if (!initializeCommon(handle))
        {
            return false;
        }

        _bufferInfo = info;
        return true;
    }
} // namespace vkm
