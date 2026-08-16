// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/renderer_common.h>

#include <glm/vec2.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <vector>

namespace vkm
{
    class VkmDriverBase;

    /*
    * @brief A grid of irradiance probes: the low-spec GI tier's storage.
    * @details Layout follows DDGI, but the probes are refreshed by rasterizing the scene from each
    * probe rather than by tracing rays, so the tier runs where there is no ray tracing. Storage and
    * sampling do not depend on how the probes were filled.
    * Two 2D atlases, one octahedral map per probe:
    *
    *   irradiance  RGBA16F  low-resolution (8x8 by default); the diffuse response per direction
    *   distance    RGBA16F  higher-resolution (16x16); r = mean distance, g = mean squared, ba unused
    *
    * The distance atlas wants two channels and gets four, the engine exposing no two-channel
    * format. It is what makes the volume usable indoors: irradiance alone leaks light through
    * walls, since a probe inside a wall still contributes to a surface outside it, while the mean
    * and mean-square let a lookup run a Chebyshev test and weight out probes the surface cannot
    * see. That test is sensitive to angular precision, hence the higher resolution.
    * Every probe's map carries a one-texel border replicating the opposite edge, so bilinear
    * filtering near an octahedral seam reads the correct neighbour rather than the far side of the
    * map; without it the seams show as bright or dark crosses. The border is why an 8x8 probe
    * occupies 10x10 texels, and why the addressing helpers below exist.
    * One copy of each atlas, not two. The update blends new samples in with hysteresis as a
    * SrcAlpha/OneMinusSrcAlpha blend against the atlas itself, which is also what makes a partial
    * update correct: probes are refreshed a few per frame, so most cells are not drawn on most
    * frames and must keep their value. A swapped pair of copies would leave every un-refreshed
    * probe alternating between two increasingly stale values.
    */

    /*
    * @brief The probe volume's parameters as a shader sees them.
    * @details Mirrors VkmProbeVolumeConstants in shaders/vkm_probe_volume.hlsli byte for byte.
    * Every member is 16-byte aligned so the glm layout matches HLSL cbuffer packing with no
    * padding members, as VkmFrameConstants also does.
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
    * @brief The six cube-face view-projections of a probe at the origin.
    * @details One buffer serves every probe, the probe's position dropping out of both passes:
    * `faceVP(pos) = P * R * T(-pos)`, with R depending only on the constant face direction and up
    * vector, so evaluating it at a world position is identical to evaluating the origin-built
    * matrix at a probe-relative one, and the blend pass's `pos + direction` cancels the translation
    * outright. What is genuinely per-probe is small enough to push.
    * Mirrors ProbeCaptureConstants in shaders/probe_capture.hlsl byte for byte.
    */
    struct VkmProbeCaptureConstants
    {
        glm::mat4 _faceViewProjection[6]{};
    };
    static_assert(sizeof(VkmProbeCaptureConstants) == 6 * 64,
                  "VkmProbeCaptureConstants must match ProbeCaptureConstants in probe_capture.hlsl");

    /*
    * @brief What the blend pass needs to turn a probe's capture into atlas contents.
    * @details Probe-independent for the reason above; the probe being blended arrives through push
    * constants. Mirrors ProbeBlendConstants in shaders/probe_blend.hlsl byte for byte.
    */
    struct VkmProbeBlendConstants
    {
        glm::mat4 _faceViewProjection[6]{};
        // x = octahedral resolution, y unused (hysteresis is pushed per probe),
        // z = capture faces per row, w = capture face size in texels
        glm::vec4 _blendParams{8.0f, 0.0f, 3.0f, 16.0f};
        // xy = the capture atlas extent in texels; zw unused
        glm::vec4 _captureAtlasSize{48.0f, 32.0f, 0.0f, 0.0f};
    };
    static_assert(sizeof(VkmProbeBlendConstants) == 6 * 64 + 32,
                  "VkmProbeBlendConstants must match ProbeBlendConstants in probe_blend.hlsl");

    /*
    * @brief The per-probe half of the capture pass's inputs (push constants, vertex stage).
    *
    * Mirrors FacePushConstants in shaders/probe_capture.hlsl.
    */
    struct VkmProbeCapturePushConstants
    {
        glm::vec3 _probePositionWorld{0.0f, 0.0f, 0.0f};
        uint32_t _faceIndex = 0u;
    };
    static_assert(sizeof(VkmProbeCapturePushConstants) == 16,
                  "VkmProbeCapturePushConstants must match FacePushConstants in probe_capture.hlsl");

    /*
    * @brief The per-probe half of the blend pass's inputs: push constants, vertex stage.
    * @details A fragment shader cannot read push constants on Vulkan, so the vertex shader forwards
    * both as flat interpolants. `_hysteresis` is per probe rather than per volume, so a probe's
    * first update can use 0 and land exactly on its capture instead of 97% of a cleared cell.
    * Mirrors ProbePushConstants in shaders/probe_blend.hlsl.
    */
    struct VkmProbeBlendPushConstants
    {
        // Index of the probe's first capture face tile in the shared capture atlas.
        uint32_t _captureTileBase = 0u;
        float _hysteresis = 0.0f;
    };
    static_assert(sizeof(VkmProbeBlendPushConstants) == 8,
                  "VkmProbeBlendPushConstants must match ProbePushConstants in probe_blend.hlsl");

    /*
    * @brief Builds the six cube-face view-projections for a probe.
    * @details Face order is +X, -X, +Y, -Y, +Z, -Z, matching the cubemap convention the engine uses
    * for skybox faces. The near plane is small and the far plane is the caller's: a probe's usable
    * range is what the Chebyshev test compares against, so clipping geometry closer than the far
    * plane would record a wall as "nothing there".
    * @param position Probe position. Callers filling VkmProbeCaptureConstants or
    * VkmProbeBlendConstants pass the origin, both passes being written against probe-relative
    * positions so one set of matrices serves the whole volume.
    * @param nearZ Near plane distance.
    * @param farZ Far plane distance.
    * @param outFaceViewProjections Receives the six matrices.
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

        inline bool isValid() const { return _driver != nullptr; }
        inline const Descriptor& getDescriptor() const { return _descriptor; }

        uint32_t getProbeCount() const;
        // Probe grid coordinate from a linear index, x varying fastest.
        glm::uvec3 getProbeCoord(uint32_t probeIndex) const;
        // The grid position plus this probe's manual offset, which is what every consumer wants:
        // the capture renders from here and the lookup measures visibility against here.
        glm::vec3 getProbePosition(uint32_t probeIndex) const;
        // Grid position alone, ignoring the offset -- for tooling that shows where a probe would
        // sit unedited.
        glm::vec3 getProbeGridPosition(uint32_t probeIndex) const;

        /*
        * @brief Per-probe manual displacement, in world units.
        * @details A probe that lands inside geometry captures that geometry's interior and hands
        * it to the lookup, which is the documented source of saturated patches beside hard black
        * ones on interior surfaces. Rather than detecting and relocating automatically, the
        * offsets are authored: tooling moves a probe into open space and the volume republishes
        * them. Zero for every probe until something sets one.
        * @param probeIndex Linear probe index.
        * @param offset World-space displacement from the grid position.
        */
        void setProbeOffset(uint32_t probeIndex, const glm::vec3& offset);
        glm::vec3 getProbeOffset(uint32_t probeIndex) const;
        void clearProbeOffsets();
        // Whether any probe carries a non-zero offset -- lets a consumer skip the upload entirely.
        inline bool hasProbeOffsets() const { return _hasProbeOffsets; }
        /*
        * @brief Uploads the offsets into the GPU texture the lookup reads.
        * @details Call after editing offsets; the capture path needs no upload because it pushes
        * getProbePosition() per probe. Cheap enough to call per edit -- one texel per probe.
        */
        bool uploadProbeOffsets();
        /*
        * @brief The offset texture the probe-lighting pass binds; valid (and zero) from
        * initialize().
        * @details One texel per probe, xyz = world offset, addressed by probeCellCoord() exactly as
        * the atlases are, so a shader reuses the addressing it already does. A texture rather than
        * a buffer because the lookup is a fragment shader and this engine's storage-buffer table
        * type is compute-visible only; 32-bit float because a half's ULP at Sponza's 3721-unit
        * scale is about a world unit, which is the size of the edits themselves.
        */
        inline VkmResourceHandle getProbeOffsetTexture() const { return _offsetTexture; }
        static VkmFormat getProbeOffsetFormat();
        // Texel extent of the offset texture: probeCounts.x * probeCounts.y by probeCounts.z.
        glm::uvec2 getProbeOffsetExtent() const;

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

        /*
        * @brief The parameters a shader needs to address this volume, filled from the descriptor.
        * @details `hysteresis` is a tuning value the volume does not otherwise own.
        * `normalBiasFraction` is a fraction of the smallest probe spacing, not a world distance:
        * a world constant is an assumption about scene scale, and 0.25 units is a quarter of a
        * room in one scene and nothing at all in a 3721-unit Sponza. The bias has to clear a
        * surface off its own probe's stored depth, and "how far apart the probes are" is the only
        * scale that means anything to that test.
        * @param normalBiasFraction Bias along the surface normal, as a fraction of min(spacing).
        * @param hysteresis Blend retention per probe refresh.
        */
        VkmProbeVolumeConstants makeConstants(float normalBiasFraction = 0.25f,
                                              float hysteresis = 0.97f) const;

    private:
        struct AtlasSet
        {
            VkmResourceHandle _irradiance{};
            VkmResourceHandle _distance{};
        };

        uint32_t irradianceCellSize() const;
        uint32_t distanceCellSize() const;
        glm::uvec2 probeCellCoord(uint32_t probeIndex) const;

        bool createSet(AtlasSet& set);
        void releaseSet(AtlasSet& set);

        VkmDriverBase* _driver = nullptr;
        Descriptor _descriptor{};
        AtlasSet _set{};

        // One float4 per probe (xyz = offset, w unused), indexed by linear probe index -- which is
        // also the row-major texel index of probeCellCoord(), so this vector uploads verbatim.
        std::vector<glm::vec4> _probeOffsets;
        VkmResourceHandle _offsetTexture{ VKM_INVALID_RESOURCE_HANDLE };
        bool _hasProbeOffsets = false;
    };
} // namespace vkm
