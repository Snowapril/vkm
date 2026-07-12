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
    };
} // namespace vkm
