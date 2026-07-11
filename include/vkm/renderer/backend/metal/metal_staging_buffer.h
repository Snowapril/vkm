// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/staging_buffer.h>

@protocol MTLBuffer;

namespace vkm
{
    class VkmStagingBufferMetal : public VkmStagingBuffer
    {
    public:
        VkmStagingBufferMetal(VkmDriverBase* driver);
        ~VkmStagingBufferMetal();

        virtual bool initialize(VkmResourceHandle handle, const VkmStagingBufferInfo& info) override final;
        virtual void* map() override final;
        virtual void unmap() override final;
        virtual void flush(uint64_t offset, uint64_t size) override final;
        virtual void setDebugName(const char* name) override final;

        inline id<MTLBuffer> getBuffer() const { return _mtlBuffer; }

    private:
        id<MTLBuffer> _mtlBuffer{nullptr};
    };
} // namespace vkm
