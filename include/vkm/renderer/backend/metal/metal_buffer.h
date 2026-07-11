// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/buffer.h>

@protocol MTLBuffer;

namespace vkm
{
    class VkmBufferMetal : public VkmBuffer
    {
    public:
        VkmBufferMetal(VkmDriverBase* driver);
        ~VkmBufferMetal();

        virtual bool initialize(VkmResourceHandle handle, const VkmBufferInfo& info) override final;
        virtual bool overrideExternalHandle(void* externalHandle) override final;
        virtual void setDebugName(const char* name) override final;

        inline id<MTLBuffer> getBuffer() const { return _mtlBuffer; }

    private:
        // Committed and pooled buffers are both independent id<MTLBuffer> objects on Metal
        // (unlike Vulkan's shared-pool-buffer-plus-offset model) -- no offset concept needed.
        id<MTLBuffer> _mtlBuffer{nullptr};
    };
} // namespace vkm
