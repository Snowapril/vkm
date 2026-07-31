// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/renderer_common.h>

#include <vector>
#include <cstdint>

namespace vkm
{
    // Which bindless array a registered buffer belongs to. Each maps to a fixed binding
    // within the engine-global bindless resource set ("set 0"); every backend implements
    // the same convention (Vulkan: descriptor set, Metal: argument buffer, WebGPU:
    // mega-buffer emulation -- see each VkmBindlessResourceManager* implementation).
    enum class VkmBindlessArrayType : uint8_t
    {
        Texture     = 0, // set 0, binding 0 (sampled image array) -- sampled textures
        Buffer      = 1, // set 0, binding 1 (storage buffer array) -- vertex-pulling data
        IndexBuffer = 2, // set 0, binding 2 (storage buffer array) -- index-pulling data
    };

    /*
    * @brief Engine-global buffers that occupy a fixed binding rather than an array slot.
    *
    * These are not array entries for two reasons. Shaders must reach them without a runtime slot
    * index, because the GPU-driven scene draw path pushes no constants at all -- an indirect draw
    * carries only its object index, in firstInstance. And the indirect-argument buffer needs a
    * read-write storage binding, which WebGPU cannot combine with a vertex-visible one (a WebGPU
    * binding has exactly one type, and writable storage is not allowed in the vertex stage), so it
    * physically cannot share a binding with the read-only pools.
    *
    * Each occupies set 0 binding kVkmBindlessFirstSingletonBinding + its enum value.
    */
    enum class VkmBindlessSingletonBuffer : uint8_t
    {
        ObjectData       = 0, // StructuredBuffer<ObjectData>, read-only,  VS + CS
        FrameData        = 1, // StructuredBuffer<FrameData>,  read-only,  VS/PS + CS
        IndirectArgument = 2, // RWStructuredBuffer<uint>,     read-write, CS only
        // The culling pass's output and the emit pass's input: a per-batch visible count followed
        // by that batch's compacted object indices.
        VisibleList      = 3, // RWStructuredBuffer<uint>,     read-write, CS only
        Count            = 4,
    };

    // Engine-global bindless binding convention. These constants are the single source of
    // truth shared by the shader compiler (vkm-compiler MSL/WGSL generation) and every
    // backend runtime; changing one side without the other breaks shader/runtime ABI.
    inline constexpr uint32_t kVkmBindlessTextureCapacity     = 4096; // set 0, binding 0
    inline constexpr uint32_t kVkmBindlessBufferCapacity      = 4096; // set 0, binding 1
    inline constexpr uint32_t kVkmBindlessIndexBufferCapacity = 4096; // set 0, binding 2

    /*
    * @brief set 0, binding 3 -- the one engine-wide sampler, owned by the bindless manager.
    * @details Binding 0 is a sampled-image array with no sampler attached (which is the shape
    * HLSL wants: DXC always emits a separate texture and SamplerState, and spirv-cross
    * outright refuses combined image samplers inside an argument-buffer runtime array).
    * Rather than a sampler array nothing would index yet, there is a single
    * linear/clamp-to-edge sampler -- what a cubemap wants, and enough for any texture the
    * engine samples today.
    */
    inline constexpr uint32_t kVkmBindlessSamplerBinding = 3;

    // set 0, bindings 4.. -- one per VkmBindlessSingletonBuffer, in that enum's order. Derived from
    // the sampler binding so adding another fixed binding above only has to move one constant.
    inline constexpr uint32_t kVkmBindlessFirstSingletonBinding = kVkmBindlessSamplerBinding + 1;

    /*
    * @brief Only ONE texture type may be declared at binding 0 per shader.
    * @details Vulkan's VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE carries no dimensionality -- the
    * shader's declaration does -- so different pipelines may legally declare binding 0 as
    * TextureCube[] or Texture2D[] as they like, provided each only indexes slots holding
    * views of the type it declared (slots come from one shared allocator, so that is a
    * convention, not something the runtime enforces). Declaring *both* in a single shader is
    * what breaks: spirv-cross aliases the second array onto the first and skips its argument-
    * buffer padding, silently producing a structurally wrong MSL layout.
    */

    // Push-constant range every pipeline declares: vertex stage, offset 0 (see
    // VkmPipelineStateVulkan::createInner). Draws push only as much as their own shader
    // struct needs -- at minimum {uint32 vertexBufferIndex, uint32 indexBufferIndex}, up to
    // this size for shaders that also carry per-draw transforms (see the model_viewer
    // sample). 128 bytes is the push-constant size Vulkan guarantees on every device, and
    // it fits within the Metal/WebGPU push-constant ring's 256-byte entry stride.
    inline constexpr uint32_t kVkmBindlessPushConstantSize = 128;

    // Metal (argument buffers Tier 2): [[buffer(0)]]/[[buffer(1)]] remain the vertex-stream
    // indices; the set-0 argument buffer and the push-constant buffer are pinned after them.
    // Within the Tier-2 argument buffer each resource occupies one 8-byte entry (GPU address
    // or MTLResourceID) at entry index = argument-buffer id; the three arrays are laid out
    // back-to-back: textures at ids [0, 4096), buffers at [4096, 8192), index buffers at
    // [8192, 12288), and the single engine sampler at 12288.
    inline constexpr uint32_t kVkmMetalBindlessArgumentBufferIndex = 2;
    inline constexpr uint32_t kVkmMetalPushConstantBufferIndex     = 3;
    // Set 1's constant buffer (see common/frame_constants.h) follows them. vkm-compiler
    // declares set 1 discrete, so this is a plain buffer binding rather than a second
    // argument buffer. Kept here so the argument table's bind count stays derivable from the
    // one buffer-index map.
    inline constexpr uint32_t kVkmMetalFrameConstantBufferIndex   = 4;

    /*
    * @brief Largest set-2 binding index a PSO may declare.
    *
    * An engine-wide limit rather than a Metal-only one, so a PSO that loads on Vulkan loads
    * everywhere: Metal has to reserve argument-table slots up front, and a per-backend cap would
    * turn a valid pipeline into a runtime failure on one backend only.
    */
    inline constexpr uint32_t kVkmPerPassResourceMaxBindings = 16;

    /*
    * @brief Set 2 (per-pass) on Metal: discrete bindings, one Metal index per declared binding.
    *
    * Discrete for the same reason set 1 is -- it keeps set 2 out of the padding walk
    * pad_argument_buffer_resources drives, which needs a registered base type for every id it
    * steps over and cannot cope with the sparse binding indices a PSO is allowed to declare.
    *
    * Metal keeps separate index spaces for buffers, textures and samplers. The buffer space
    * continues after set 1's; the texture and sampler spaces start at 0, because set 0's bindless
    * textures and sampler live *inside* its argument buffer rather than in argument-table slots,
    * leaving both spaces otherwise unused.
    */
    inline constexpr uint32_t kVkmMetalPerPassBufferIndexBase  = kVkmMetalFrameConstantBufferIndex + 1;
    inline constexpr uint32_t kVkmMetalPerPassTextureIndexBase = 0;
    inline constexpr uint32_t kVkmMetalPerPassSamplerIndexBase = 0;

    inline constexpr uint32_t kVkmMetalArgumentTableBufferBindCount =
        kVkmMetalPerPassBufferIndexBase + kVkmPerPassResourceMaxBindings;
    inline constexpr uint32_t kVkmMetalArgumentTableTextureBindCount =
        kVkmMetalPerPassTextureIndexBase + kVkmPerPassResourceMaxBindings;
    inline constexpr uint32_t kVkmMetalArgumentTableSamplerBindCount =
        kVkmMetalPerPassSamplerIndexBase + kVkmPerPassResourceMaxBindings;
    inline constexpr uint32_t kVkmMetalBindlessTextureIdBase     = 0;
    inline constexpr uint32_t kVkmMetalBindlessBufferIdBase      = kVkmBindlessTextureCapacity;
    inline constexpr uint32_t kVkmMetalBindlessIndexBufferIdBase = kVkmBindlessTextureCapacity + kVkmBindlessBufferCapacity;
    inline constexpr uint32_t kVkmMetalBindlessSamplerId =
        kVkmBindlessTextureCapacity + kVkmBindlessBufferCapacity + kVkmBindlessIndexBufferCapacity;
    // The singleton buffers follow the sampler entry, one 8-byte entry each, in
    // VkmBindlessSingletonBuffer order.
    inline constexpr uint32_t kVkmMetalBindlessSingletonIdBase = kVkmMetalBindlessSamplerId + 1;
    inline constexpr uint32_t kVkmMetalBindlessArgumentEntryCount =
        kVkmMetalBindlessSingletonIdBase + static_cast<uint32_t>(VkmBindlessSingletonBuffer::Count);

    // Fixed-capacity slot allocator for one bindless array: LIFO free-list plus a monotonic
    // high-water mark (a plain free-list, not VkmOffsetAllocator's byte-range coalescing,
    // which is the wrong shape for uniform-size array-element allocation).
    class VkmBindlessSlotAllocator
    {
    public:
        explicit VkmBindlessSlotAllocator(uint32_t capacity);

        // Pops the most recently released slot if any, otherwise bumps the high-water mark.
        // Returns UINT32_MAX if capacity is exhausted.
        uint32_t allocate();
        void release(uint32_t slot);

    private:
        uint32_t _capacity;
        uint32_t _nextSlot = 0;
        std::vector<uint32_t> _freeSlots;
    };

    // Backend-agnostic interface to the engine-global bindless resource set. Each backend
    // driver owns one implementation (see VkmDriverBase::getBindlessResourceManager()).
    class VkmBindlessResourceManagerBase
    {
    public:
        virtual ~VkmBindlessResourceManagerBase() = default;

        virtual void destroy() = 0;

        // Registers an existing storage buffer resource into the given bindless array and
        // publishes it at the returned slot. Returns UINT32_MAX if the array is exhausted.
        virtual uint32_t registerBuffer(VkmResourceHandle bufferHandle, VkmBindlessArrayType arrayType) = 0;
        virtual void unregisterBuffer(uint32_t slot, VkmBindlessArrayType arrayType) = 0;

        /*
        * @brief Publishes a texture into the bindless texture array (set 0, binding 0) and
        * returns its slot, or UINT32_MAX when the array is exhausted.
        * @details Takes the texture rather than a view: each backend publishes the texture's
        * own default view/native object, which VkmTextureInfo::_type already gave the right
        * dimensionality, so a cubemap needs no separate view object. The texture must have
        * been uploaded (or otherwise made shader-readable) before a draw samples it.
        * Backends without VkmDriverCapabilityFlags::TextureUpload return UINT32_MAX.
        */
        virtual uint32_t registerTexture(VkmResourceHandle textureHandle) = 0;
        virtual void unregisterTexture(uint32_t slot) = 0;

        /*
        * @brief Publishes a storage buffer at one of the fixed singleton bindings, or unbinds it
        * when passed VKM_INVALID_RESOURCE_HANDLE.
        *
        * Called at scene build/teardown time rather than per frame: on WebGPU a bind group is
        * immutable, so this recreates bind group 0.
        */
        virtual bool setSingletonBuffer(VkmBindlessSingletonBuffer which, VkmResourceHandle bufferHandle) = 0;
    };
} // namespace vkm
