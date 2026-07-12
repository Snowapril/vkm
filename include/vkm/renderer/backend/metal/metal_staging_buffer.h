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
        virtual void writeDirect(uint64_t offset, const void* data, uint64_t size) override final;
        virtual void setDebugName(const char* name) override final;

        uint64_t getAllocatedSize() const override { return _allocatedSize; }
        uint32_t getMemoryAlignment() const override { return _memoryAlignment; }

        inline id<MTLBuffer> getBuffer() const { return _mtlBuffer; }

    private:
        id<MTLBuffer> _mtlBuffer{nullptr};
        uint64_t _allocatedSize{0};
        uint32_t _memoryAlignment{256}; // sane default; overwritten with a real value at creation time
    };
} // namespace vkm
