// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>

#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <cstdint>

namespace vkm
{
    /*
    * @brief The engine's descriptor-set convention, ordered by update frequency so a set is
    * rebound only as often as its contents actually change.
    * @details
    *   set 0  bindless   -- resource arrays, never change after registration
    *                        (bindless_resource_manager.h)
    *   set 1  per-frame  -- camera/view constants, written once per frame (this header)
    *   set 2  per-pass   -- declared by a PSO, bound once per pass (resource_table.h)
    *   set 3  per-draw   -- declared by a PSO, bound per draw (resource_table.h)
    * Per-draw data that fits travels as push constants instead of through set 3 (see
    * kVkmBindlessPushConstantSize); set 3 is for what does not fit or is not a constant at all,
    * such as the per-material texture table WebGPU needs, having no bindless arrays. Unlike push
    * constants, a set-1 read is visible to every shader stage.
    * These four exhaust WebGPU's default maxBindGroups of 4, so anything further has to be folded
    * into one of them.
    * They are the C++ half of a shader ABI whose other halves are
    * include/vkm/shaders/vkm_frame_constants.hlsli and vkm-compiler's Metal binding pins; changing
    * one without the others breaks shader/runtime agreement silently.
    */
    inline constexpr uint32_t kVkmBindlessSetIndex      = 0;
    inline constexpr uint32_t kVkmFrameConstantSetIndex = 1;
    inline constexpr uint32_t kVkmPerPassSetIndex       = 2;
    inline constexpr uint32_t kVkmPerDrawSetIndex       = 3;

    /*
    * @brief Which PSO-declared descriptor set a declaration or a resource table belongs to.
    * @details Sets 2 and 3 differ only by their set index, by which declaration they validate
    * against, and on Metal by which argument-table index bases they occupy. JSON parsing,
    * validation and the backend table objects are shared, hence a parameter rather than a
    * duplicated hierarchy.
    */
    enum class VkmResourceSetKind : uint8_t
    {
        PerPass = 0, // set 2, bound once per pass
        PerDraw = 1, // set 3, bound per draw
    };

    inline constexpr uint32_t vkmResourceSetIndex(VkmResourceSetKind kind)
    {
        return (kind == VkmResourceSetKind::PerPass) ? kVkmPerPassSetIndex : kVkmPerDrawSetIndex;
    }

    inline constexpr const char* vkmResourceSetKindName(VkmResourceSetKind kind)
    {
        return (kind == VkmResourceSetKind::PerPass) ? "per-pass" : "per-draw";
    }

    inline constexpr uint32_t kVkmFrameConstantBinding = 0;

    /*
    * @brief Required alignment of one frame slot's region inside the frame-constant buffer.
    * @details 256 is a multiple of every backend's minimum uniform-buffer offset alignment
    * (WebGPU's default minUniformBufferOffsetAlignment is exactly 256; no known Vulkan device
    * reports more, which the Vulkan implementation asserts) and is the same granularity the
    * Metal/WebGPU push-constant rings already use.
    */
    inline constexpr uint32_t kVkmFrameConstantAlignment = 256;

    /*
    * @brief Per-frame camera constants published at set 1, binding 0.
    * @details Every member is 16-byte aligned, so the glm layout matches HLSL cbuffer / WGSL
    * uniform packing with no padding members and the struct can be memcpy'd in as-is. Mirrors
    * VkmFrameConstants in include/vkm/shaders/vkm_frame_constants.hlsli.
    * The default member initializers make a value-initialized instance identity rather than
    * all-zero, which is what gets published when no camera is active, so a shader reading it still
    * produces something defined instead of collapsing every vertex to the origin.
    */
    struct VkmFrameConstants
    {
        glm::mat4 _view{ 1.0f };                                  // offset   0
        glm::mat4 _projection{ 1.0f };                            // offset  64
        glm::mat4 _viewProjection{ 1.0f };                        // offset 128
        glm::mat4 _inverseViewProjection{ 1.0f };                 // offset 192

        /*
        * @brief Last frame's _viewProjection, for reprojecting a world position into the previous
        * frame's screen space.
        * @details Filled by the engine rather than the camera, "previous" being a property of the
        * frame loop. Equal to _viewProjection on the first frame after a camera becomes active, so
        * reprojection is the identity rather than a jump from an identity matrix.
        */
        glm::mat4 _prevViewProjection{ 1.0f };                    // offset 256

        glm::vec4 _cameraPositionWorld{ 0.0f, 0.0f, 0.0f, 1.0f }; // offset 320, xyz = world eye

        // xy = viewport size in pixels, zw = its reciprocal. Screen-space passes need both, and
        // the reciprocal is worth carrying rather than recomputing per invocation.
        glm::vec4 _viewportSize{ 0.0f, 0.0f, 0.0f, 0.0f };        // offset 336

        /*
        * @brief x = monotonically increasing frame counter; yzw reserved.
        * @details Distinct from the frame slot index (0..FRAME_COUNT-1) the engine cycles through:
        * a stochastic pass needs a value that never repeats, to decorrelate its sampling between
        * frames. A uvec4 rather than a bare uint so the struct keeps its 16-byte member alignment.
        */
        glm::uvec4 _frameIndex{ 0u, 0u, 0u, 0u };                 // offset 352

        /*
        * @brief Last frame's _cameraPositionWorld, for interpreting history data measured from the
        * previous camera — the G-buffer's camera distance is radial from the eye that rendered it.
        * @details Filled by the engine beside _prevViewProjection, with the same first-frame
        * seeding: equal to the current position until a previous frame exists.
        */
        glm::vec4 _prevCameraPositionWorld{ 0.0f, 0.0f, 0.0f, 1.0f }; // offset 368
    };
    static_assert(sizeof(VkmFrameConstants) == 384,
                  "VkmFrameConstants must match VkmFrameConstants in shaders/vkm_frame_constants.hlsli");

    // Byte stride between two frame slots' regions: the struct rounded up to the alignment
    // every backend's uniform-buffer offsets must respect. Derived rather than hardcoded so
    // adding a member can never silently overlap the next slot.
    inline constexpr uint32_t kVkmFrameConstantStride =
        ((sizeof(VkmFrameConstants) + kVkmFrameConstantAlignment - 1) / kVkmFrameConstantAlignment) *
        kVkmFrameConstantAlignment;

    inline constexpr uint64_t kVkmFrameConstantBufferSize =
        static_cast<uint64_t>(FRAME_COUNT) * kVkmFrameConstantStride;

    /*
    * @brief Backend-agnostic owner of the engine-global per-frame constant set ("set 1").
    * @details Holds one host-writable uniform buffer carved into FRAME_COUNT regions plus whatever
    * the backend needs to bind one of them: a descriptor set per slot on Vulkan, a bind group per
    * slot on WebGPU, a GPU address on Metal. Like the bindless managers, the buffer is a raw native
    * allocation rather than a VkmBuffer -- no VkmBuffer is host-writable on any backend, and no
    * staging buffer can carry uniform usage.
    * Each backend driver owns one implementation; reach it through
    * VkmDriverBase::getFrameConstantManager().
    */
    class VkmFrameConstantManagerBase
    {
    public:
        virtual ~VkmFrameConstantManagerBase() = default;

        virtual void destroy() = 0;

        /*
        * @brief Writes constants into a frame slot and makes that slot the one subsequent pipeline
        * binds publish.
        * @details A plain host write with no GPU synchronization, so the caller must already have
        * waited out the slot's previous submit; VkmEngine::render() does this via
        * VkmRenderGraph::ensureCompleted(). Recording is single-threaded, as the bindless managers
        * also assume.
        * @param frameIndex Slot to write.
        * @param constants Values to publish.
        */
        virtual void update(uint32_t frameIndex, const VkmFrameConstants& constants) = 0;

    protected:
        // Slot the most recent update() wrote, and therefore the one bind sites publish.
        uint32_t _activeFrameIndex = 0;
    };
} // namespace vkm
