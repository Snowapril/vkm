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
    *
    *   set 0  bindless   -- resource arrays, never change after registration
    *                        (bindless_resource_manager.h)
    *   set 1  per-frame  -- camera/view constants, written once per frame (this header)
    *   set 2  per-pass   -- reserved, not declared by any pipeline layout yet
    *   set 3  per-draw   -- reserved, not declared by any pipeline layout yet
    *
    * Per-draw data that fits travels as push constants instead of through set 3 (see
    * kVkmBindlessPushConstantSize). Unlike push constants, a set-1 read is visible to every
    * shader stage.
    *
    * These constants are the C++ half of a shader ABI whose other halves are
    * include/vkm/shaders/vkm_frame_constants.hlsli and vkm-compiler's Metal binding pins;
    * changing one without the others breaks shader/runtime agreement silently.
    */
    inline constexpr uint32_t kVkmBindlessSetIndex      = 0;
    inline constexpr uint32_t kVkmFrameConstantSetIndex = 1;
    inline constexpr uint32_t kVkmPerPassSetIndex       = 2; // reserved
    inline constexpr uint32_t kVkmPerDrawSetIndex       = 3; // reserved

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
    *
    * Every member is 16-byte aligned, so the glm layout matches HLSL cbuffer / WGSL uniform
    * packing with no padding members and the struct can be memcpy'd in as-is. Mirrors
    * VkmFrameConstants in include/vkm/shaders/vkm_frame_constants.hlsli.
    *
    * The default member initializers make a value-initialized instance identity rather than
    * all-zero, which is what gets published when no camera is active -- a shader reading it
    * then still produces something defined instead of collapsing every vertex to the origin.
    */
    struct VkmFrameConstants
    {
        glm::mat4 _view{ 1.0f };                                  // offset   0
        glm::mat4 _projection{ 1.0f };                            // offset  64
        glm::mat4 _viewProjection{ 1.0f };                        // offset 128
        glm::mat4 _inverseViewProjection{ 1.0f };                 // offset 192
        glm::vec4 _cameraPositionWorld{ 0.0f, 0.0f, 0.0f, 1.0f }; // offset 256, xyz = world eye
    };
    static_assert(sizeof(VkmFrameConstants) == 272,
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
    *
    * Holds one host-writable uniform buffer carved into FRAME_COUNT regions plus whatever the
    * backend needs to bind one of them (a descriptor set per slot on Vulkan, a bind group per
    * slot on WebGPU, a GPU address on Metal). Like the bindless managers, the buffer is a raw
    * native allocation rather than a VkmBuffer: no VkmBuffer is host-writable on any backend,
    * and no staging buffer can carry uniform usage.
    *
    * Each backend driver owns one implementation; reach it through
    * VkmDriverBase::getFrameConstantManager().
    */
    class VkmFrameConstantManagerBase
    {
    public:
        virtual ~VkmFrameConstantManagerBase() = default;

        virtual void destroy() = 0;

        /*
        * @brief Writes `constants` into frame slot `frameIndex` and makes that slot the one
        * subsequent pipeline binds publish.
        * @details The write is a plain host write with no GPU synchronization, so the caller
        * must have already waited out the slot's previous submit (VkmEngine::render() does
        * this via VkmRenderGraph::ensureCompleted()). Recording is single-threaded, the same
        * assumption the bindless managers make.
        */
        virtual void update(uint32_t frameIndex, const VkmFrameConstants& constants) = 0;

    protected:
        // Slot the most recent update() wrote, and therefore the one bind sites publish.
        uint32_t _activeFrameIndex = 0;
    };
} // namespace vkm
