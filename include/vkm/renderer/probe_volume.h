// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/renderer_common.h>

#include <glm/vec2.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>

namespace vkm
{
    class VkmDriverBase;

    /*
    * @brief A grid of irradiance probes: the low-spec GI tier's storage.
    *
    * @details Layout follows DDGI, but the probes are refreshed by rasterizing the scene from each
    * probe rather than by tracing rays, because this tier must run where there is no ray tracing
    * (WebGPU, MoltenVK) and on mobile. Storage and sampling do not depend on how the probes were
    * filled, so once acceleration structures exist the same volume can be refreshed with rays --
    * and ReSTIR can query it for multi-bounce radiance -- without changing anything here.
    *
    * Two atlases, both 2D, with one octahedral map per probe:
    *
    *   irradiance  RGBA16F  low-resolution (8x8 by default); the diffuse response per direction
    *   distance    RGBA16F  higher-resolution (16x16); r = mean distance, g = mean squared, ba unused
    *
    * The distance atlas wants two channels and gets four, because the engine exposes no
    * two-channel format -- the same constraint that made the G-buffer pack two normals into one
    * RGBA16F target. Adding R16G16_SFLOAT would touch all three backends' format converters to
    * save about 2.5 MB at the default grid, which is not the trade to make first.
    *
    * The distance atlas is what makes this usable indoors. Sampling irradiance alone leaks light
    * through walls, because a probe inside a wall still contributes to a surface outside it. The
    * mean and mean-square let a lookup run a Chebyshev test -- effectively a variance shadow map
    * per probe -- and weight out probes the surface cannot see. It is stored at higher resolution
    * than irradiance because that test is sensitive to angular precision in a way the diffuse
    * response is not.
    *
    * Every probe's map carries a one-texel border replicating the opposite edge, so bilinear
    * filtering near an octahedral seam reads the correct neighbour instead of the far side of the
    * map. Without it the seams show as bright or dark crosses on every lit surface. The border is
    * why an 8x8 probe occupies 10x10 texels, and why the addressing helpers below exist rather
    * than callers multiplying by the resolution.
    *
    * Both atlases are double-buffered: an update blends new samples into the previous frame's
    * values with hysteresis, so it must read one copy while writing the other.
    */
    /*
    * @brief The probe volume's parameters as a shader sees them.
    *
    * Mirrors VkmProbeVolumeConstants in shaders/vkm_probe_volume.hlsli byte for byte. Every member
    * is 16-byte aligned so the glm layout matches HLSL cbuffer packing with no padding members,
    * the same discipline VkmFrameConstants follows.
    */
    struct VkmProbeVolumeConstants
    {
        glm::vec4 _originAndSpacingX{0.0f, 0.0f, 0.0f, 1.0f};  // xyz = grid origin, w = spacing.x
        glm::vec4 _spacingYZ{1.0f, 1.0f, 0.0f, 0.0f};          // x = spacing.y, y = spacing.z
        glm::uvec4 _probeCounts{1u, 1u, 1u, 0u};               // xyz = probes per axis
        // x = irradiance resolution, y = distance resolution, z = normal bias, w = hysteresis
        glm::vec4 _atlasParams{8.0f, 16.0f, 0.25f, 0.97f};
    };
    static_assert(sizeof(VkmProbeVolumeConstants) == 64,
                  "VkmProbeVolumeConstants must match the struct in shaders/vkm_probe_volume.hlsli");

    /*
    * @brief One probe's six face view-projections, as the capture shader sees them.
    *
    * Mirrors ProbeCaptureConstants in shaders/probe_capture.hlsl byte for byte.
    */
    struct VkmProbeCaptureConstants
    {
        glm::mat4 _faceViewProjection[6]{};
        glm::vec4 _probePositionWorld{0.0f, 0.0f, 0.0f, 1.0f};
    };
    static_assert(sizeof(VkmProbeCaptureConstants) == 6 * 64 + 16,
                  "VkmProbeCaptureConstants must match ProbeCaptureConstants in probe_capture.hlsl");

    /*
    * @brief What the blend pass needs to turn one probe's capture into atlas contents.
    *
    * Mirrors ProbeBlendConstants in shaders/probe_blend.hlsl byte for byte.
    */
    struct VkmProbeBlendConstants
    {
        glm::mat4 _faceViewProjection[6]{};
        glm::vec4 _probePositionWorld{0.0f, 0.0f, 0.0f, 1.0f};
        // x = octahedral resolution, y = hysteresis, z = capture faces per row,
        // w = capture face size in texels
        glm::vec4 _blendParams{8.0f, 0.97f, 3.0f, 16.0f};
    };
    static_assert(sizeof(VkmProbeBlendConstants) == 6 * 64 + 32,
                  "VkmProbeBlendConstants must match ProbeBlendConstants in probe_blend.hlsl");

    /*
    * @brief Builds the six cube-face view-projections for a probe at `position`.
    *
    * @details Face order is +X, -X, +Y, -Y, +Z, -Z, matching the cubemap convention the engine
    * already uses for skybox faces. The near plane is deliberately small and the far plane is the
    * caller's: a probe's usable range is what the Chebyshev test will later compare against, so
    * clipping geometry closer than the far plane would record a wall as "nothing there".
    */
    void vkmBuildProbeFaceViewProjections(const glm::vec3& position, float nearZ, float farZ,
                                          glm::mat4 outFaceViewProjections[6]);

    class VkmProbeVolume
    {
    public:
        struct Descriptor
        {
            // Probe counts per axis. The total is what sizes the atlases.
            glm::uvec3 _probeCounts{16u, 8u, 16u};
            // World-space distance between adjacent probes.
            glm::vec3 _spacing{1.0f, 1.0f, 1.0f};
            // World position of probe (0, 0, 0).
            glm::vec3 _origin{0.0f, 0.0f, 0.0f};
            // Per-probe octahedral resolutions, excluding the border.
            uint32_t _irradianceResolution = 8u;
            uint32_t _distanceResolution = 16u;
        };

        // One texel on every side, replicating the opposite edge so bilinear taps near a seam are
        // correct.
        static constexpr uint32_t kBorderTexels = 1u;

        static VkmFormat getIrradianceFormat();
        static VkmFormat getDistanceFormat();

        VkmProbeVolume() = default;
        // Releases anything still held; the driver must outlive the volume.
        ~VkmProbeVolume();

        VkmProbeVolume(const VkmProbeVolume&) = delete;
        VkmProbeVolume& operator=(const VkmProbeVolume&) = delete;

        bool initialize(VkmDriverBase* driver, const Descriptor& descriptor);
        void destroy();

        // Flips which atlas copy is current. Call once per update.
        void advanceFrame();

        inline bool isValid() const { return _driver != nullptr; }
        inline const Descriptor& getDescriptor() const { return _descriptor; }

        uint32_t getProbeCount() const;
        // Probe grid coordinate from a linear index, x varying fastest.
        glm::uvec3 getProbeCoord(uint32_t probeIndex) const;
        glm::vec3 getProbePosition(uint32_t probeIndex) const;

        /*
        * @brief Atlas dimensions in texels, borders included.
        * @details Probes are laid out with the X and Y grid axes flattened along the atlas's width
        * and the Z axis along its height, so a probe's texels stay contiguous and a whole
        * XY "slice" of the grid is one atlas row of cells.
        */
        glm::uvec2 getIrradianceAtlasExtent() const;
        glm::uvec2 getDistanceAtlasExtent() const;

        // Top-left texel of a probe's cell in the given atlas, border included. Adding
        // kBorderTexels reaches the first interior texel.
        glm::uvec2 getIrradianceProbeTexelOrigin(uint32_t probeIndex) const;
        glm::uvec2 getDistanceProbeTexelOrigin(uint32_t probeIndex) const;

        VkmResourceHandle getIrradianceTexture() const;
        VkmResourceHandle getDistanceTexture() const;
        // The copy an update reads while writing the current one.
        VkmResourceHandle getPrevIrradianceTexture() const;
        VkmResourceHandle getPrevDistanceTexture() const;

        // The parameters a shader needs to address this volume, filled from the descriptor.
        // `normalBias` and `hysteresis` are tuning values the volume does not otherwise own.
        VkmProbeVolumeConstants makeConstants(float normalBias = 0.25f, float hysteresis = 0.97f) const;

    private:
        struct AtlasSet
        {
            VkmResourceHandle _irradiance{};
            VkmResourceHandle _distance{};
        };

        uint32_t irradianceCellSize() const;
        uint32_t distanceCellSize() const;
        glm::uvec2 probeCellCoord(uint32_t probeIndex) const;

        bool createSet(AtlasSet& set, uint32_t setIndex);
        void releaseSet(AtlasSet& set);

        VkmDriverBase* _driver = nullptr;
        Descriptor _descriptor{};
        std::array<AtlasSet, 2> _sets{};
        uint32_t _currentSet = 0;
    };
} // namespace vkm
