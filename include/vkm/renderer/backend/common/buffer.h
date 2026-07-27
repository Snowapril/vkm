// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/render_resource.h>

#include <vector>

namespace vkm
{
    class VkmBufferView;

    class VkmBuffer : public VkmRenderResource
    {
    public:
        VkmBuffer(VkmDriverBase* driver);
        ~VkmBuffer();

        virtual bool initialize(VkmResourceHandle handle, const VkmBufferInfo& info) = 0;
        virtual bool overrideExternalHandle(void* externalHandle) = 0;

        inline const VkmBufferInfo& getBufferInfo() const { return _bufferInfo; }
        VkmResourceType getResourceType() const override { return VkmResourceType::Buffer; }

        /*
        * @brief Whether this buffer's memory can be written by the CPU directly, as opposed to
        * through a staging buffer and a queue-submitted copy.
        * @details Only ever true when the buffer asked for it with
        * VkmMemoryAccessHint::HostWrite, and even then it reports the outcome rather than the
        * intent -- Vulkan re-checks the allocation's real memory properties, and WebGPU cannot
        * offer it at all (a WGPUBuffer's usage flags fix its map mode for life, and MapWrite
        * only combines with CopySrc).
        */
        inline bool isHostWritable() const { return _isHostWritable; }

        /*
        * @brief The CPU pointer to this buffer's memory, or nullptr when it is not
        * host-writable. Only meaningful once isHostWritable() is true.
        * @details Host-writable buffers stay mapped for their whole lifetime on every backend
        * that supports them, so this is an accessor rather than a state change. Not pure -- a
        * backend that never reports isHostWritable() has nothing to implement, and this default
        * is the matching "should be unreachable" guard.
        */
        virtual void* map()
        {
            VKM_DEBUG_ERROR("map is not implemented for this buffer");
            return nullptr;
        }

        /*
        * @brief Makes the CPU's writes to the mapped range visible to the GPU. The pointer
        * map() returned stays valid afterwards -- this is the flush point, not a teardown.
        */
        virtual void unmap() {}

        /*
        * @brief This buffer's address in the GPU's address space, or 0 where there is no such
        * concept: WebGPU always, and Vulkan without
        * VkmDriverCapabilityFlags::BufferDeviceAddress.
        */
        virtual uint64_t getGPUVirtualAddress() const { return 0; }

        /*
        * @brief Create a view of this buffer. This is the ONLY way to create a
        * VkmBufferView -- VkmDriverBase::newBufferView() is protected and friended to
        * this class specifically so callers cannot bypass ownership tracking. info._buffer
        * is always overwritten with this buffer's own handle, regardless of what the
        * caller passed in.
        */
        VkmBufferView* createView(VkmBufferViewInfo info);

        std::vector<VkmResourceHandle> getOwnedChildHandles() const override { return _ownedViewHandles; }

    protected:
        bool initializeBufferCommon(VkmResourceHandle handle, const VkmBufferInfo& info);

    protected:
        VkmBufferInfo _bufferInfo;
        std::vector<VkmResourceHandle> _ownedViewHandles;
        // Safe default: a backend that never opts in keeps the staging upload path.
        bool _isHostWritable = false;
    };
} // namespace vkm
