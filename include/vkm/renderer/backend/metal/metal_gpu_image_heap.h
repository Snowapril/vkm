// Copyright (c) 2025 Snowapril

#pragma once

#import <Metal/Metal.h>

namespace vkm
{
    class VkmDriverMetal;

    /*
    * @brief One MTLHeapTypePlacement heap that aliasable textures are created into at chosen
    * offsets.
    *
    * @details The counterpart to VkmGpuHeapPoolMetal, and deliberately a different heap type.
    * That pool is MTLHeapTypeAutomatic, which picks offsets itself -- fine for buffers, but it
    * gives no way to say "put this texture exactly where that one was". Placement does:
    * -[MTLHeap newTextureWithDescriptor:offset:] documents that resources overlapping the
    * half-open range [offset, offset + size) are implicitly aliased, which is the same model
    * Vulkan's bind-at-offset gives and lets one packer drive both backends.
    *
    * The alternative -- MTLHeapTypeAutomatic plus -[MTLResource makeAliasable] -- was rejected
    * because it is irreversible and gives no control over which resource claims the bytes, so
    * reusing them would mean re-creating the MTLTexture every frame. Every immutable
    * VkmResourceTable holding that texture would then be stale.
    *
    * hazardTrackingMode is left at a placement heap's default of untracked: tracked would make
    * Metal serialize every resource in the heap against every other, which is the opposite of
    * what aliasing is for. The ordering that keeps aliases apart is the explicit acquisition
    * barrier instead (see VkmCommandBufferMetal::onAcquireAliasedTexture).
    */
    class VkmGpuImageHeapMetal
    {
    public:
        VkmGpuImageHeapMetal(VkmDriverMetal* driver);
        ~VkmGpuImageHeapMetal();

        bool initialize(uint64_t sizeBytes);
        void destroy();

        // Creates the texture in place at `offset`. Unlike Vulkan there is no separate bind
        // step: on Metal the object and its placement are one call.
        id<MTLTexture> createTexture(MTLTextureDescriptor* descriptor, uint64_t offset);

        inline id<MTLHeap> getHeap() const { return _mtlHeap; }
        inline uint64_t getSizeBytes() const { return _sizeBytes; }

    private:
        VkmDriverMetal* _driver;
        id<MTLHeap> _mtlHeap{nullptr};
        uint64_t _sizeBytes = 0;
    };
} // namespace vkm
