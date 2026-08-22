// Copyright (c) 2026 Snowapril

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
        * @details Reports the outcome, not the request: true only when the buffer asked for
        * VkmMemoryAccessHint::HostWrite and the backend could honor it. Vulkan re-checks the
        * allocation's real memory properties, and WebGPU never honors it.
        */
        inline bool isHostWritable() const { return _isHostWritable; }

        /*
        * @brief The CPU pointer to this buffer's memory. Only call this when isHostWritable().
        * @details Host-writable buffers stay mapped for their whole lifetime, so this is an
        * accessor rather than a state change.
        * @return The mapped pointer, or nullptr when the buffer is not host-writable.
        */
        virtual void* map()
        {
            VKM_DEBUG_ERROR("map is not implemented for this buffer");
            return nullptr;
        }

        /*
        * @brief Makes the CPU's writes to the mapped range visible to the GPU.
        * @details A flush point, not a teardown: the pointer map() returned stays valid.
        */
        virtual void unmap() {}

        /*
        * @brief This buffer's address in the GPU's address space.
        * @return The address, or 0 on backends without one: WebGPU always, and Vulkan without
        * VkmDriverCapabilityFlags::BufferDeviceAddress.
        */
        virtual uint64_t getGPUVirtualAddress() const { return 0; }

        /*
        * @brief Creates a view of this buffer. The only supported way to obtain a VkmBufferView.
        * @param info View description. Its _buffer field is always overwritten with this buffer's
        * own handle, whatever the caller passed in.
        * @return The new view, owned by this buffer.
        */
        VkmBufferView* createView(VkmBufferViewInfo info);

        std::vector<VkmResourceHandle> getOwnedChildHandles() const override { return _ownedViewHandles; }

    protected:
        bool initializeBufferCommon(VkmResourceHandle handle, const VkmBufferInfo& info);

    protected:
        VkmBufferInfo _bufferInfo;
        std::vector<VkmResourceHandle> _ownedViewHandles;
        bool _isHostWritable = false;
    };
} // namespace vkm
