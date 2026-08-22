// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/renderer/backend/common/buffer.h>
#include <vkm/renderer/backend/metal/metal_gpu_heap_allocator.h>

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

        uint64_t getAllocatedSize() const override { return _allocatedSize; }
        uint32_t getMemoryAlignment() const override { return _memoryAlignment; }

        virtual void* map() override final;
        virtual void unmap() override final;
        virtual uint64_t getGPUVirtualAddress() const override final;

        inline id<MTLBuffer> getBuffer() const { return _mtlBuffer; }

    private:
        id<MTLBuffer> _mtlBuffer{nullptr};
        // Valid only on the heap-placed path; a placement heap reclaims nothing on its own, so
        // the destructor must hand this range back.
        VkmGpuHeapAllocatorMetal::Placement _heapPlacement{};
        uint64_t _allocatedSize{0};
        uint32_t _memoryAlignment{256}; // sane default; overwritten with a real value at creation time
    };
} // namespace vkm
