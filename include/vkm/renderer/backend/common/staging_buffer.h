// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/render_resource.h>

namespace vkm
{
    class VkmStagingBuffer : public VkmRenderResource
    {
    public:
        VkmStagingBuffer(VkmDriverBase* driver);
        ~VkmStagingBuffer();

        virtual bool initialize(VkmResourceHandle handle, const VkmStagingBufferInfo& info) = 0;
        virtual void* map() = 0;
        virtual void unmap() = 0;
        virtual void flush(uint64_t offset, uint64_t size) = 0;

        inline const VkmStagingBufferInfo& getStagingBufferInfo() const { return _stagingBufferInfo; }
        VkmResourceType getResourceType() const override { return VkmResourceType::StagingBuffer; }

    protected:
        bool initializeStagingBufferCommon(VkmResourceHandle handle, const VkmStagingBufferInfo& info);

    protected:
        VkmStagingBufferInfo _stagingBufferInfo;
        void* _mappedPointer{nullptr};
    };
} // namespace vkm
