// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/buffer_view.h>

namespace vkm
{
    /*
    * @brief WebGPU has no buffer-view object at all -- bindings use offset+size directly.
    * This class is metadata-only: getOffset()/getSize() resolve the view's byte range
    * within its parent buffer; there is no native handle to expose.
    */
    class VkmBufferViewWebGPU final : public VkmBufferView
    {
    public:
        VkmBufferViewWebGPU(VkmDriverBase* driver);
        ~VkmBufferViewWebGPU();

        virtual bool initialize(VkmResourceHandle handle, const VkmBufferViewInfo& info) override final;
        virtual void setDebugName(const char* name) override final;

        inline uint64_t getOffset() const { return _bufferViewInfo._offset; }
        inline uint64_t getSize() const { return _bufferViewInfo._size; }
    };
} // namespace vkm
