// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/frame_constants.h>
#include <vkm/renderer/backend/common/pipeline_state.h>
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
    * Rather than a sampler array nothing would index yet, there is a single linear/repeat
    * sampler. Repeat because it is glTF's default wrap and material textures are the main
    * consumer; a cube lookup derives in-range face coordinates, so the address mode does not
    * apply there either way. A material wanting clamp addresses wrong at the edges -- per-texture
    * samplers are the fix, and are not built.
    */
    inline constexpr uint32_t kVkmBindlessSamplerBinding = 3;

    // set 0, bindings 4.. -- one per VkmBindlessSingletonBuffer, in that enum's order. Derived from
    // the sampler binding so adding another fixed binding above only has to move one constant.
    inline constexpr uint32_t kVkmBindlessFirstSingletonBinding = kVkmBindlessSamplerBinding + 1;

    /*
    * @brief set 0, the binding after the singletons -- the one scene acceleration structure.
    *
    * @details Not a `VkmBindlessSingletonBuffer` despite sitting beside them: its descriptor type
    * is an acceleration structure on Vulkan and an `MTLResourceID` on Metal, so nothing about the
    * singleton buffer path applies to it beyond the binding arithmetic. `setAccelerationStructure`
    * is its setter.
    *
    * **One structure, not an array.** SPIRV-Cross rejects `OpConvertUToAccelerationStructureKHR`,
    * so there is no pointer-style bindless acceleration structure on the Metal path (restir.md
    * section 4.2); a shader reaches the scene's top-level structure by name or not at all.
    *
    * Present only where the device reports `VkmDriverCapabilityFlags::RayTracing`. On Vulkan the
    * descriptor type is invalid without `VK_KHR_acceleration_structure` enabled, so the set-0
    * layout genuinely differs between an RT and a non-RT device -- which costs nothing, since a
    * shader declaring this binding is a ray-query shader and cannot be created on a non-RT device
    * anyway. WebGPU has no such API at all.
    */
    inline constexpr uint32_t kVkmBindlessAccelerationStructureBinding =
        kVkmBindlessFirstSingletonBinding + static_cast<uint32_t>(VkmBindlessSingletonBuffer::Count);

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

    /*
    * @brief How many push-constant allocations the Metal and WebGPU rings hold **per frame slot**.
    *
    * @details Those backends have no push-constant instruction, so every setPushConstants() call
    * takes a ring entry and the draw references it by dynamic offset. The ring is partitioned into
    * FRAME_BUFFER_COUNT regions of this many entries, one per frame slot, and
    * VkmBindlessResourceManagerBase::beginFrame() rewinds the cursor to its region base. That is
    * what makes reuse safe: a slot's region is only rewritten after that slot's render graph has
    * been waited on, which is the same guarantee descriptor set 1's per-slot region relies on.
    *
    * Named here rather than only inside each backend because it is still a budget callers have to
    * plan against -- it now bounds one frame's pushes rather than all frames in flight together,
    * but a pass that pushes per (item, sub-item, batch), like the probe capture, can reach it.
    */
    inline constexpr uint32_t kVkmPushConstantRingEntryCount = 1024;

    // Total entries the ring buffer holds: one region per frame slot.
    inline constexpr uint32_t kVkmPushConstantRingTotalEntryCount =
        kVkmPushConstantRingEntryCount * FRAME_BUFFER_COUNT;

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
    inline constexpr uint32_t kVkmResourceTableMaxBindings = 16;

    /*
    * @brief Sets 2 (per-pass) and 3 (per-draw) on Metal: discrete bindings, one Metal index per
    * declared binding.
    *
    * Discrete for the same reason set 1 is -- it keeps them out of the padding walk
    * pad_argument_buffer_resources drives, which needs a registered base type for every id it
    * steps over and cannot cope with the sparse binding indices a PSO is allowed to declare.
    *
    * Metal keeps separate index spaces for buffers, textures and samplers, and has **no descriptor
    * set index at all** -- which is the one place the two sets genuinely differ rather than merely
    * being parameterized. Their separation is carried entirely by these bases, so set 3 starts
    * where set 2's kVkmResourceTableMaxBindings end in every one of the three spaces. Getting this
    * wrong aliases set 3 onto set 2 silently.
    *
    * The buffer space continues after set 1's; the texture and sampler spaces start at 0, because
    * set 0's bindless textures and sampler live *inside* its argument buffer rather than in
    * argument-table slots, leaving both spaces otherwise unused.
    */
    /*
    * @brief How many resources of each kind ONE set may declare, on Metal.
    *
    * @details MTL4ArgumentTable enforces hard caps -- `max buffer bind count must not be more than
    * 31` and `max sampler state bind count must not be more than 16`, per its descriptor
    * validation -- and sets 2 and 3 have to share them. These are what the sums below fit into.
    *
    * They bound resources *per type*, not binding indices, because the index assignment is dense
    * per type (see VkmMetalTableIndexAssigner): a PSO declaring five textures, one sampler and one
    * uniform at bindings 0..6 uses sampler slot 0, not slot 5. Assigning `base + bindingIndex`
    * instead would have made the sampler cap bind at 8 binding indices per set -- one more than
    * gi_composite already declares.
    */
    inline constexpr uint32_t kVkmMetalTableTextureCapacity = 16;
    inline constexpr uint32_t kVkmMetalTableSamplerCapacity = 8;
    inline constexpr uint32_t kVkmMetalTableBufferCapacity  = 13;

    inline constexpr uint32_t kVkmMetalPerPassBufferIndexBase  = kVkmMetalFrameConstantBufferIndex + 1;
    inline constexpr uint32_t kVkmMetalPerPassTextureIndexBase = 0;
    inline constexpr uint32_t kVkmMetalPerPassSamplerIndexBase = 0;

    inline constexpr uint32_t kVkmMetalPerDrawBufferIndexBase  = kVkmMetalPerPassBufferIndexBase + kVkmMetalTableBufferCapacity;
    inline constexpr uint32_t kVkmMetalPerDrawTextureIndexBase = kVkmMetalPerPassTextureIndexBase + kVkmMetalTableTextureCapacity;
    inline constexpr uint32_t kVkmMetalPerDrawSamplerIndexBase = kVkmMetalPerPassSamplerIndexBase + kVkmMetalTableSamplerCapacity;

    inline constexpr uint32_t kVkmMetalArgumentTableBufferBindCount =
        kVkmMetalPerDrawBufferIndexBase + kVkmMetalTableBufferCapacity;
    inline constexpr uint32_t kVkmMetalArgumentTableTextureBindCount =
        kVkmMetalPerDrawTextureIndexBase + kVkmMetalTableTextureCapacity;
    inline constexpr uint32_t kVkmMetalArgumentTableSamplerBindCount =
        kVkmMetalPerDrawSamplerIndexBase + kVkmMetalTableSamplerCapacity;

    // The caps MTL4ArgumentTableDescriptor validation enforces. Exceeding one aborts at device
    // level with "Argument Table Descriptor Validation", and only with Metal API Validation on --
    // which is exactly how this was found, and why it is asserted here instead.
    static_assert(kVkmMetalArgumentTableBufferBindCount <= 31, "MTL4ArgumentTable allows at most 31 buffer binds");
    static_assert(kVkmMetalArgumentTableSamplerBindCount <= 16, "MTL4ArgumentTable allows at most 16 sampler binds");

    /*
    * @brief Assigns a set's Metal argument-table indices, densely per resource type.
    *
    * @details Metal keeps separate index spaces for buffers, textures and samplers, and has no
    * descriptor set index at all -- which set a binding belongs to is carried entirely by these
    * indices. Walking the declaration in order and taking the next index of that resource's type
    * is what keeps the scarce sampler space proportional to the samplers actually declared.
    *
    * Both vkm-compiler's add_msl_resource_binding pins and VkmResourceTableMetal's bind-time
    * writes drive this same assigner over the same declaration, in the same order. They must agree
    * exactly: a mismatch is a shader reading a slot nobody wrote, with no diagnostic.
    */
    class VkmMetalTableIndexAssigner
    {
    public:
        explicit VkmMetalTableIndexAssigner(VkmResourceSetKind kind)
            : _textureIndex((kind == VkmResourceSetKind::PerPass) ? kVkmMetalPerPassTextureIndexBase
                                                                 : kVkmMetalPerDrawTextureIndexBase)
            , _samplerIndex((kind == VkmResourceSetKind::PerPass) ? kVkmMetalPerPassSamplerIndexBase
                                                                  : kVkmMetalPerDrawSamplerIndexBase)
            , _bufferIndex((kind == VkmResourceSetKind::PerPass) ? kVkmMetalPerPassBufferIndexBase
                                                                 : kVkmMetalPerDrawBufferIndexBase)
        {
        }

        // The Metal index for the next declared resource of `type`, in declaration order.
        uint32_t next(VkmTableResourceType type)
        {
            switch (type)
            {
                case VkmTableResourceType::SampledTexture: return _textureIndex++;
                case VkmTableResourceType::Sampler:        return _samplerIndex++;
                case VkmTableResourceType::StorageBuffer:
                case VkmTableResourceType::UniformBuffer:  return _bufferIndex++;
            }
            return 0;
        }

    private:
        uint32_t _textureIndex;
        uint32_t _samplerIndex;
        uint32_t _bufferIndex;
    };
    inline constexpr uint32_t kVkmMetalBindlessTextureIdBase     = 0;
    inline constexpr uint32_t kVkmMetalBindlessBufferIdBase      = kVkmBindlessTextureCapacity;
    inline constexpr uint32_t kVkmMetalBindlessIndexBufferIdBase = kVkmBindlessTextureCapacity + kVkmBindlessBufferCapacity;
    inline constexpr uint32_t kVkmMetalBindlessSamplerId =
        kVkmBindlessTextureCapacity + kVkmBindlessBufferCapacity + kVkmBindlessIndexBufferCapacity;
    // The singleton buffers follow the sampler entry, one 8-byte entry each, in
    // VkmBindlessSingletonBuffer order.
    inline constexpr uint32_t kVkmMetalBindlessSingletonIdBase = kVkmMetalBindlessSamplerId + 1;
    // The acceleration structure follows them, in the same 8-byte entry shape -- an argument
    // buffer holds an MTLResourceID there rather than a GPU address, which is the same width.
    inline constexpr uint32_t kVkmMetalBindlessAccelerationStructureId =
        kVkmMetalBindlessSingletonIdBase + static_cast<uint32_t>(VkmBindlessSingletonBuffer::Count);
    inline constexpr uint32_t kVkmMetalBindlessArgumentEntryCount =
        kVkmMetalBindlessAccelerationStructureId + 1;

    /*
    * @brief Hands out push-constant ring entries from the current frame slot's region.
    *
    * @details The ring is FRAME_BUFFER_COUNT regions of kVkmPushConstantRingEntryCount entries.
    * beginFrame() rewinds the cursor to a slot's region base, which is only safe once that slot's
    * render graph has been waited on -- see VkmBindlessResourceManagerBase::beginFrame().
    *
    * Shared by the Metal and WebGPU managers rather than duplicated in each: the arithmetic is
    * identical (only the entry stride and what the entry index is turned into differ), and it is
    * the part worth testing without a device.
    */
    class VkmPushConstantRingAllocator
    {
    public:
        // Rewinds to `frameSlot`'s region. Out-of-range slots are clamped to region 0, which is
        // also the region a manager that never gets a beginFrame() call keeps using.
        void beginFrame(uint32_t frameSlot);

        // Returns the absolute entry index to write. Wrapping inside a region means this frame
        // pushed more than kVkmPushConstantRingEntryCount times and is reusing entries it still
        // references -- unlike a wrap across frames, no wait can make that safe, so it is a
        // budget overflow rather than an ordering assumption. `outOverflowed` reports the wrap.
        uint32_t allocate(bool* outOverflowed = nullptr);

    private:
        uint32_t _cursor = 0;
        uint32_t _regionBase = 0;
    };

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

        /*
        * @brief Publishes the scene's top-level acceleration structure at
        * kVkmBindlessAccelerationStructureBinding, or unbinds it when passed
        * VKM_INVALID_RESOURCE_HANDLE.
        *
        * Called at scene build/teardown time, like setSingletonBuffer -- a rebuilt structure keeps
        * the same handle, so a per-frame rebuild does not have to republish it.
        *
        * Fails on a backend without VkmDriverCapabilityFlags::RayTracing, where the binding does
        * not exist.
        */
        virtual bool setAccelerationStructure(VkmResourceHandle accelerationStructureHandle) = 0;

        /*
        * @brief Rewinds this frame slot's push-constant ring region.
        *
        * Called once per frame by VkmEngine::render(), after that slot's render graph has been
        * waited on -- the same point, and for the same reason, that the frame constants of
        * descriptor set 1 are written. Overwriting the region before that wait would clobber
        * entries a running frame still references.
        *
        * Not pure virtual, and Vulkan deliberately inherits the no-op: it has a real
        * push-constant instruction, so there is no ring and nothing to rewind. A manager that
        * never receives this call keeps using region 0, which is what the unit tests do.
        */
        virtual void beginFrame(uint32_t frameSlot) { (void)frameSlot; }
    };
} // namespace vkm
