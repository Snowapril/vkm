// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/probe_volume.h>
#include <vkm/renderer/scene/scene.h>
#include <vkm/renderer/scene/vertex_layout.h>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace vkm
{
    class VkmDriverBase;
    class VkmPerPassResourceTableBase;
    class VkmPipelineStateBase;
    class VkmPipelineStateManager;
    class VkmRenderGraph;

    /*
    * @brief Refreshes a VkmProbeVolume a few probes per frame, by rasterizing the scene from them.
    *
    * @details VkmProbeVolume is storage and VkmProbeVolume's shaders are the read path; this is the
    * write path's frame loop, and it is the piece that makes the low-spec GI tier a running
    * technique rather than three passes that exist. Per call it appends five subgraphs:
    *
    *   transfer  scene object/frame data upload
    *   compute   scene cull, aimed at this frame's probes rather than at a camera
    *   graphics  capture -- every budgeted probe's six cube faces, one viewport each, ONE pass
    *   compute   barrier, so the capture can be sampled
    *   graphics  x2  blend the captures into the irradiance and distance atlases
    *
    * **Amortization is the whole design.** Rasterizing a probe's surroundings re-renders the scene
    * six times, so refreshing every probe every frame is not affordable at any useful grid size --
    * `_budget` probes are refreshed per frame instead, round-robin, and the rest keep last round's
    * values. That is what the blend pass's hysteresis is for, and it is also this tier's known
    * weak point: a light change takes a full round to reach every probe and then several more
    * rounds to converge, so the propagation latency is a real number worth measuring rather than
    * estimating (see TestProbeVolumeUpdaterShared.hpp).
    *
    * The volume is referenced, not owned: it is also the read path's input (probe_lighting.hlsl)
    * and outlives any particular updater.
    *
    * What this does *not* do is composite anything. Whoever samples the atlases afterwards needs
    * its own barrierTextureForShaderRead on them, because this class cannot know when that read
    * happens.
    */
    class VkmProbeVolumeUpdater
    {
    public:
        struct Descriptor
        {
            // Probes refreshed per frame. The round length is ceil(probeCount / budget) frames, and
            // that multiplies every convergence time, so this is the tier's main tuning knob.
            uint32_t _budget = 32u;
            // Weight of the *existing* atlas value in an update. Higher converges more smoothly and
            // more slowly; the error after k refreshes of a probe is hysteresis^k. A probe's first
            // ever refresh ignores this and takes the capture whole, so a cold start costs nothing.
            float _hysteresis = 0.97f;
            // One cube face's edge, in texels, inside the shared capture atlas.
            uint32_t _captureFaceSize = 16u;
            // Probe range. The far plane bounds what the Chebyshev test can ever report as "seen",
            // so it should cover the scene the probes sit in.
            float _nearZ = 0.05f;
            float _farZ = 100.0f;
            // Which of VkmScene's cull views this refresh owns. A frame that also renders a camera
            // view must give them different indices: a probe looks in every direction, so the two
            // cannot share a cull result.
            uint32_t _cullViewIndex = 0u;
        };

        // Face tiles per capture-atlas row. Six faces land as a 3x2 block per probe, which keeps the
        // atlas close to square for any budget.
        static constexpr uint32_t kCaptureFacesPerRow = 3u;

        /*
        * @brief The largest budget the push-constant ring can carry.
        *
        * @details Metal and WebGPU hand out a push-constant ring entry per setPushConstants() call
        * and never reset it per frame (1024 entries, VkmBindlessResourceManagerMetal). The capture
        * pass pushes once per (probe, face, batch) and the blend pass once per (probe, atlas), so a
        * frame costs at least 6*budget + 2*budget entries, and FRAME_COUNT frames may be in flight.
        * Above this the ring wraps onto entries a running frame still references.
        */
        static constexpr uint32_t kMaxBudget = 32u;

        VkmProbeVolumeUpdater() = default;
        ~VkmProbeVolumeUpdater();

        VkmProbeVolumeUpdater(const VkmProbeVolumeUpdater&) = delete;
        VkmProbeVolumeUpdater& operator=(const VkmProbeVolumeUpdater&) = delete;

        bool initialize(VkmDriverBase* driver, VkmPipelineStateManager* pipelineStateManager,
                        VkmProbeVolume* volume, const Descriptor& descriptor,
                        std::string* outError = nullptr);
        void destroy();

        inline bool isValid() const { return _driver != nullptr; }

        /*
        * @brief Appends this frame's probe refresh to `renderGraph`.
        *
        * @details `frameData`'s light direction and material slot are the caller's; its frustum
        * planes are overwritten with the bounding box of the probes being refreshed, because a
        * probe looks in all six directions and the six faces share one cull result.
        */
        void record(VkmRenderGraph* renderGraph, VkmScene* scene, const VkmFrameData& frameData);

        // Probes refreshed by the most recent record(), and their indices. The count can be smaller
        // than the budget on the last frame of a round: the slice is clamped rather than wrapped, so
        // a round refreshes every probe exactly once.
        inline uint32_t getUpdateCount() const { return static_cast<uint32_t>(_slice.size()); }
        inline uint32_t getProbeIndexForSlot(uint32_t slot) const { return _slice[slot]; }

        // Frames a full sweep of the grid takes at the current budget.
        uint32_t getRoundLengthInFrames() const;

        inline const Descriptor& getDescriptor() const { return _descriptor; }
        glm::uvec2 getCaptureAtlasExtent() const;

        /*
        * @brief Frames for a probe's value to fall within `errorFraction` of a step change.
        *
        * @details The blend is a fixed-ratio geometric decay -- after k refreshes a probe retains
        * hysteresis^k of its old value -- so this is exact rather than a heuristic, and it is what
        * turns a hysteresis and a budget into the number that actually matters. The result is the
        * upper bound of the interval: a probe refreshed just before the change waits a whole round
        * for its first update, so the true figure lies in (result - roundLength, result].
        */
        static uint32_t framesToConverge(uint32_t probeCount, uint32_t budget, float hysteresis,
                                         float errorFraction);

    private:
        // Selects the next `min(budget, remaining)` probes and advances the cursor. Split out from
        // record() so the schedule can be tested without a GPU.
        void advanceSlice();

        bool createCaptureTargets(std::string* outError);
        bool createConstantBuffers(std::string* outError);
        bool createTables(VkmPipelineStateManager* pipelineStateManager, std::string* outError);

        // Where a probe's face tiles start in the capture atlas, as a tile index.
        inline uint32_t captureTileBase(uint32_t slot) const { return slot * 6u; }

        VkmDriverBase* _driver = nullptr;
        VkmProbeVolume* _volume = nullptr;
        Descriptor _descriptor{};

        VkmResourceHandle _captureColor{};
        VkmResourceHandle _captureDepth{};
        VkmResourceHandle _sampler{};
        VkmResourceHandle _captureConstants{};
        VkmResourceHandle _irradianceBlendConstants{};
        VkmResourceHandle _distanceBlendConstants{};

        // One capture PSO and table per vertex layout, because a table is validated against the
        // pipeline it was built for and each layout permutation is a different pipeline.
        std::array<VkmPipelineStateBase*, static_cast<size_t>(VkmVertexLayoutPreset::Count)> _capturePipelines{};
        std::array<VkmPerPassResourceTableBase*, static_cast<size_t>(VkmVertexLayoutPreset::Count)> _captureTables{};
        VkmPipelineStateBase* _irradianceBlendPipeline = nullptr;
        VkmPipelineStateBase* _distanceBlendPipeline = nullptr;
        VkmPerPassResourceTableBase* _irradianceBlendTable = nullptr;
        VkmPerPassResourceTableBase* _distanceBlendTable = nullptr;

        std::vector<uint32_t> _slice;
        // Resolved during record() rather than read from _everRefreshed inside the render callback:
        // the callback runs at execute(), by which point every probe in the slice has already been
        // marked refreshed, so it would never see a first refresh as one.
        std::vector<float> _sliceHysteresis;
        // A probe's first refresh blends against a cleared cell, so it takes the capture whole
        // instead of 3% of it. Without this every probe would spend a full convergence just
        // climbing out of black.
        std::vector<bool> _everRefreshed;
        uint32_t _cursor = 0;
        // The atlases are cleared by the first blend pass's load action rather than by a pass of
        // their own. It has to happen: on Vulkan the first render pass transitions them from
        // UNDEFINED, which discards whatever they held.
        bool _atlasesCleared = false;
    };
} // namespace vkm
