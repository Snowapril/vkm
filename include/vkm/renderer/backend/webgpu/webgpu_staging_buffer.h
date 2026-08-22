// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/renderer/backend/common/staging_buffer.h>
#include <webgpu/webgpu.h>

#include <vector>

namespace vkm
{
    /*
    * @brief WebGPU staging buffers come in two shapes, and they cannot be one buffer.
    * @details Readback, VkmResourceCreateInfo::AllowTransferDst, is `CopyDst | MapRead`: the GPU
    * copies into it and the CPU maps it to read. Mapping is genuinely async, bridged with the same
    * Asyncify emscripten_sleep-spin VkmDriverWebGPU::initializeInner uses.
    * Upload is `CopySrc | CopyDst` and is not mappable at all. It cannot be: a staging buffer is
    * written both by map()+memcpy and by writeDirect(), which is a wgpuQueueWriteBuffer and so
    * needs CopyDst, and WebGPU allows MapWrite to combine only with CopySrc. So an upload buffer
    * keeps a CPU shadow: map() hands back the shadow, unmap()/flush() push it with
    * wgpuQueueWriteBuffer, and writeDirect() writes both. That also keeps the async round trip out
    * of the upload path.
    * unmap() invalidates the pointer a readback map() returned; callers must not cache it.
    */
    class VkmStagingBufferWebGPU final : public VkmStagingBuffer
    {
    public:
        VkmStagingBufferWebGPU(VkmDriverBase* driver);
        ~VkmStagingBufferWebGPU();

        virtual bool initialize(VkmResourceHandle handle, const VkmStagingBufferInfo& info) override final;
        virtual void* map() override final;
        virtual void unmap() override final;
        virtual void flush(uint64_t offset, uint64_t size) override final;
        virtual void writeDirect(uint64_t offset, const void* data, uint64_t size) override final;
        virtual void setDebugName(const char* name) override final;

        uint64_t getAllocatedSize() const override { return _stagingBufferInfo._size; }
        uint32_t getMemoryAlignment() const override { return 256; }

        inline WGPUBuffer getBuffer() const { return _wgpuBuffer; }

        // True for the readback shape; the upload shape is queue-written and never mapped.
        inline bool isReadback() const
        {
            return (_stagingBufferInfo._flags & VkmResourceCreateInfo::AllowTransferDst) != 0;
        }

    private:
        // wgpuQueueWriteBuffer requires both the offset and the size to be 4-byte multiples, so a
        // write is widened to the enclosing aligned range. Correct because the shadow always holds
        // the buffer's whole contents, so the extra bytes are re-sent unchanged rather than
        // clobbered with stale data.
        void flushShadow(uint64_t offset, uint64_t size);

        WGPUBuffer _wgpuBuffer{nullptr};
        // Whether the next readback map() has to go through wgpuBufferMapAsync. Starts true: the
        // buffer is created unmapped, because WebGPU rejects any GPU or queue use of a mapped one.
        bool _needsRemap{true};
        // Upload shape only: the CPU-side contents map() exposes and the queue writes push.
        std::vector<uint8_t> _shadow;
    };
} // namespace vkm
