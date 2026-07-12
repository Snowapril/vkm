// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/buffer_view.h>
#include <volk.h>

namespace vkm
{
    /*
    * @brief Buffer view. Only texel-buffer bindings need a real VkBufferView (created only
    * when _bufferViewInfo._format is set) -- regular UBO/SSBO bindings use offset+range
    * directly, so getBufferView() may legitimately return VK_NULL_HANDLE; getOffset()/
    * getSize() are always valid regardless.
    */
    class VkmBufferViewVulkan : public VkmBufferView
    {
    public:
        VkmBufferViewVulkan(VkmDriverBase* driver);
        ~VkmBufferViewVulkan();

        virtual bool initialize(VkmResourceHandle handle, const VkmBufferViewInfo& info) override final;
        virtual void setDebugName(const char* name) override final;

        inline VkBufferView getBufferView() const { return _vkBufferView; }
        inline uint64_t getOffset() const { return _absoluteOffset; }
        inline uint64_t getSize() const { return _bufferViewInfo._size; }

    private:
        VkBufferView _vkBufferView{VK_NULL_HANDLE};
        uint64_t _absoluteOffset{0}; // parent buffer's own pool offset (if pooled) + info._offset
    };
} // namespace vkm
