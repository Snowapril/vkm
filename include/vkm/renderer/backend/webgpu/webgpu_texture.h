// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/texture.h>
#include <webgpu/webgpu.h>

namespace vkm
{
    class VkmTextureWebGPU final : public VkmTexture
    {
    public:
        VkmTextureWebGPU(VkmDriverBase* driver);
        ~VkmTextureWebGPU();

        virtual bool initialize(VkmResourceHandle handle, const VkmTextureInfo& info) override final;
        virtual bool overrideExternalHandle(void* externalHandle) override final;

        /*
        * @brief Uploads one mip level through wgpuQueueWriteTexture.
        *
        * @details WebGPU's queue write is exactly this class's "host write": it takes CPU bytes
        * and a queue, with no staging buffer and no command buffer. It is also the only route
        * that accepts a **tightly packed** source -- the 256-byte bytesPerRow alignment WebGPU
        * demands applies to buffer<->texture copies (GPUTexelCopyBufferInfo), not to a queue
        * write -- so an image whose width is not a multiple of 64 uploads without repacking.
        */
        virtual bool writeRegion(const void* data, uint64_t size, uint32_t mipLevel, uint32_t arrayLayer) override final;
        virtual void setDebugName(const char* name) override final;

        // WebGPU/Dawn exposes no allocation-introspection API -- best-effort passthrough of
        // the requested size; 256 is an arbitrary but conventional alignment (matches the
        // Metal backend's own choice for the same "unknown" reason, kept consistent).
        uint64_t getAllocatedSize() const override { return computeTextureByteSize(_textureInfo); }
        uint32_t getMemoryAlignment() const override { return 256; }

        inline WGPUTexture getWGPUTexture() const { return _wgpuTexture; }

    private:
        WGPUTexture _wgpuTexture{nullptr};
        bool _externallyOwned{false}; // true for swapchain-provided textures; skip destroy on release
    };
} // namespace vkm
