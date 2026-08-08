// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/webgpu/webgpu_texture.h>
#include <vkm/renderer/backend/webgpu/webgpu_driver.h>
#include <vkm/renderer/backend/webgpu/webgpu_util.h>

#include <algorithm>

namespace vkm
{
    namespace
    {
        WGPUTextureUsage toWGPUTextureUsage(VkmResourceCreateInfo flags)
        {
            uint64_t usage = WGPUTextureUsage_None;
            if ((flags & VkmResourceCreateInfo::AllowTransferSrc) != 0) usage |= WGPUTextureUsage_CopySrc;
            if ((flags & VkmResourceCreateInfo::AllowTransferDst) != 0) usage |= WGPUTextureUsage_CopyDst;
            if ((flags & VkmResourceCreateInfo::AllowShaderRead) != 0) usage |= WGPUTextureUsage_TextureBinding;
            if ((flags & VkmResourceCreateInfo::AllowShaderWrite) != 0) usage |= WGPUTextureUsage_StorageBinding;
            if ((flags & (VkmResourceCreateInfo::AllowColorAttachment | VkmResourceCreateInfo::AllowDepthStencilAttachment | VkmResourceCreateInfo::AllowPresent)) != 0)
                usage |= WGPUTextureUsage_RenderAttachment;
            return static_cast<WGPUTextureUsage>(usage);
        }
    } // namespace

    VkmTextureWebGPU::VkmTextureWebGPU(VkmDriverBase* driver)
        : VkmTexture(driver)
    {
    }

    VkmTextureWebGPU::~VkmTextureWebGPU()
    {
        if (_wgpuTexture != nullptr && _externallyOwned == false)
        {
            wgpuTextureRelease(_wgpuTexture);
        }
        _wgpuTexture = nullptr;
    }

    bool VkmTextureWebGPU::initialize(VkmResourceHandle handle, const VkmTextureInfo& info)
    {
        if (!initializeTextureCommon(handle, info))
        {
            return false;
        }

        // Swapchain back buffers are supplied later via overrideExternalHandle; don't create our own.
        if ((info._flags & VkmResourceCreateInfo::DeferredCreation) == 0 &&
            (info._flags & VkmResourceCreateInfo::ExternalHandleOwner) == 0)
        {
            if (info._placementHint == VkmMemoryPlacementHint::Heap)
            {
                // Dawn/emdawnwebgpu exposes no placement/suballocation API -- always committed.
                VKM_DEBUG_WARN("VkmMemoryPlacementHint::Heap is not supported by WebGPU; texture will be committed");
            }
            if ((info._flags & VkmResourceCreateInfo::Transient) != 0)
            {
                // WebGPU has no memoryless/lazily-allocated concept at all; _isTransient stays
                // false, so the render-pass guard leaves this texture's store action alone.
                VKM_DEBUG_WARN("VkmResourceCreateInfo::Transient is not supported by WebGPU; texture will be device-backed");
            }

            VkmDriverWebGPU* driverWebGPU = static_cast<VkmDriverWebGPU*>(_driver);

            const WGPUTextureDescriptor textureDesc{
                .label         = toWGPUStringView("VkmTextureWebGPU"),
                .usage         = toWGPUTextureUsage(info._flags),
                .dimension     = WGPUTextureDimension_2D,
                .size          = WGPUExtent3D{info._extent.x, info._extent.y, std::max(1u, info._numArrayLayers)},
                .format        = toWGPUTextureFormat(info._format),
                .mipLevelCount = std::max(1u, info._numMipLevels),
                .sampleCount   = 1,
            };
            _wgpuTexture = wgpuDeviceCreateTexture(driverWebGPU->getDevice(), &textureDesc);

            // A queue write needs CopyDst, which AllowTransferDst already asked for. Reporting
            // host-writability here is what routes uploadToTexture through writeRegion() instead
            // of the staging path -- this backend has no buffer->texture copy.
            _isHostWritable = (_wgpuTexture != nullptr) &&
                              ((info._flags & VkmResourceCreateInfo::AllowTransferDst) != 0);
        }

        return true;
    }

    bool VkmTextureWebGPU::writeRegion(const void* data, uint64_t size, uint32_t mipLevel, uint32_t arrayLayer)
    {
        if (!_isHostWritable || _wgpuTexture == nullptr)
        {
            VKM_DEBUG_ERROR("writeRegion requires a host-writable texture");
            return false;
        }

        const uint32_t mipWidth = std::max(1u, _textureInfo._extent.x >> mipLevel);
        const uint32_t mipHeight = std::max(1u, _textureInfo._extent.y >> mipLevel);
        // Tightly packed, matching what the Metal and Vulkan host paths expect of the same source.
        const uint32_t bytesPerRow = mipWidth * vkmBytesPerTexel(_textureInfo._format);
        const uint64_t expectedSize = static_cast<uint64_t>(bytesPerRow) * mipHeight;
        if (size < expectedSize)
        {
            VKM_DEBUG_ERROR("writeRegion: the source is smaller than the destination mip level");
            return false;
        }

        WGPUTexelCopyTextureInfo destination{};
        destination.texture = _wgpuTexture;
        destination.mipLevel = mipLevel;
        destination.origin = WGPUOrigin3D{0, 0, arrayLayer};
        destination.aspect = WGPUTextureAspect_All;

        WGPUTexelCopyBufferLayout layout{};
        layout.offset = 0;
        layout.bytesPerRow = bytesPerRow;
        layout.rowsPerImage = mipHeight;

        const WGPUExtent3D writeSize{mipWidth, mipHeight, 1};
        VkmDriverWebGPU* driverWebGPU = static_cast<VkmDriverWebGPU*>(_driver);
        wgpuQueueWriteTexture(driverWebGPU->getQueue(), &destination, data,
                              static_cast<size_t>(expectedSize), &layout, &writeSize);
        return true;
    }

    bool VkmTextureWebGPU::overrideExternalHandle(void* externalHandle)
    {
        _wgpuTexture = static_cast<WGPUTexture>(externalHandle);
        _externallyOwned = true;
        return true;
    }

    void VkmTextureWebGPU::setDebugName(const char* name)
    {
        if (_wgpuTexture != nullptr)
        {
            wgpuTextureSetLabel(_wgpuTexture, toWGPUStringView(name));
        }
    }
} // namespace vkm
