// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/render_pass.h>
#include <vkm/renderer/backend/common/renderer_common.h>

#include <glm/vec2.hpp>

#include <array>
#include <cstdint>

namespace vkm
{
    class VkmDriverBase;

    /*
    * @brief The geometry buffer both GI tiers read: surface attributes rasterized once per frame,
    * plus last frame's copy for temporal reuse.
    *
    * @details Engine-level rather than sample-level because it is shared infrastructure -- the
    * probe/SSGI tier and ReSTIR both consume it, and so will any denoiser.
    *
    * Channel layout. The engine exposes no two-channel formats, so the two normals share one
    * RGBA16F target via octahedral encoding rather than costing a target each:
    *
    *   Normal              RGBA16F  xy = octahedral shading normal, zw = octahedral geometric normal
    *   BaseColorRoughness  RGBA8    rgb = base colour, a = roughness
    *   MotionMetallic      RGBA16F  xy = screen-space motion vector, z = metallic,
    *                                w = distance from the camera
    *   depth               D32      hardware depth, for depth testing only -- never bound as a
    *                                texture. WebGPU validates a depth-format view against a depth
    *                                sample type, so consumers reconstruct world positions from the
    *                                camera distance above instead of sampling this.
    *
    * Both normals are carried because they answer different questions: the shading normal is what
    * lighting and temporal-tap rejection use, while the *geometric* normal is what a secondary ray
    * must be offset along to avoid re-hitting its own surface. Using the shading normal for that
    * offset is a classic source of self-intersection on normal-mapped geometry.
    *
    * History is a full second copy of every target, flipped by advanceFrame(). Double-buffering
    * only depth and normal would save roughly a third of the memory, since those are all a
    * temporal tap strictly needs, but a whole second set keeps the accessors symmetric and leaves
    * material-similarity rejection possible without another format change. At 1080p the pair costs
    * roughly 100 MB.
    *
    * Not stored: a material ID. Nothing consumes one yet -- normal and roughness rejection covers
    * the temporal-tap cases -- so it waits for a consumer rather than occupying a channel now.
    */
    class VkmGBuffer
    {
    public:
        enum class Target : uint8_t
        {
            Normal = 0,
            BaseColorRoughness = 1,
            MotionMetallic = 2,
            Count = 3,
        };

        static constexpr uint32_t kTargetCount = static_cast<uint32_t>(Target::Count);

        // The formats a G-buffer PSO must declare for its colour attachments, in target order.
        static VkmFormat getTargetFormat(Target target);
        static VkmFormat getDepthFormat();

        VkmGBuffer() = default;
        // Releases anything still held. The driver must outlive the G-buffer, as for any other
        // object holding driver resources.
        ~VkmGBuffer();

        VkmGBuffer(const VkmGBuffer&) = delete;
        VkmGBuffer& operator=(const VkmGBuffer&) = delete;

        /*
        * @brief Allocates both the current and history sets at `extent`.
        * @details `extent` must be non-zero on both axes; a zero-sized swapchain (a minimized
        * window) should simply not drive a G-buffer that frame.
        */
        bool initialize(VkmDriverBase* driver, const glm::uvec2& extent);
        void destroy();

        /*
        * @brief Reallocates both sets when `extent` differs from the current one.
        * @details A no-op at the same extent, so a caller may invoke it every frame. The old
        * textures go through the deferred reclaimer, since frames still in flight may be reading
        * them. History contents do not survive a resize -- there is nothing meaningful to
        * reproject a differently-sized frame against, and consumers must treat the first frame
        * after a resize as a full disocclusion.
        */
        bool resize(const glm::uvec2& extent);

        // Flips current and history. Call once per frame, after the frame's passes are recorded.
        void advanceFrame();

        inline bool isValid() const { return _driver != nullptr && _extent.x > 0 && _extent.y > 0; }
        inline const glm::uvec2& getExtent() const { return _extent; }

        VkmResourceHandle getTexture(Target target) const;
        VkmResourceHandle getDepthTexture() const;
        // Last frame's copies. Equal to this frame's before the first advanceFrame(), so a
        // consumer reads defined (if not useful) data rather than an invalid handle.
        VkmResourceHandle getPrevTexture(Target target) const;
        VkmResourceHandle getPrevDepthTexture() const;

        /*
        * @brief A framebuffer descriptor binding every target plus depth, all cleared.
        * @details Clearing rather than loading is deliberate: a G-buffer is fully rewritten each
        * frame wherever geometry covers it, and pixels geometry does not cover must read as empty
        * rather than as last frame's surface.
        */
        VkmFrameBufferDescriptor makeFrameBufferDescriptor() const;

    private:
        struct TextureSet
        {
            std::array<VkmResourceHandle, kTargetCount> _targets{};
            VkmResourceHandle _depth{};
        };

        bool createSet(TextureSet& set, const glm::uvec2& extent, uint32_t setIndex);
        void releaseSet(TextureSet& set, bool deferred);

        VkmDriverBase* _driver = nullptr;
        glm::uvec2 _extent{0, 0};
        // [_currentSet] is this frame's, [1 - _currentSet] is last frame's.
        std::array<TextureSet, 2> _sets{};
        uint32_t _currentSet = 0;
    };
} // namespace vkm
