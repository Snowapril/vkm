// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/renderer/probe_volume.h>
#include <vkm/renderer/scene/scene.h>
#include <vkm/renderer/scene/scene_material_tables.h>
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
    class VkmResourceTableBase;
    class VkmPipelineStateBase;
    class VkmPipelineStateManager;
    class VkmRenderGraph;

    /*
    * @brief Refreshes a VkmProbeVolume a few probes per frame, by rasterizing the scene from them.
    *
    * @details VkmProbeVolume is storage and its shaders are the read path; this is the write path's
    * frame loop. Per call it appends five subgraphs:
    *
    *   transfer  scene object/frame data upload
    *   compute   scene cull, aimed at this frame's probes rather than at a camera
    *   graphics  capture -- every budgeted probe's six cube faces, one viewport each, ONE pass
    *   compute   barrier, so the capture can be sampled
    *   graphics  x2  blend the captures into the irradiance and distance atlases
    *
    * Amortization is the whole design. Rasterizing a probe's surroundings re-renders the scene six
    * times, so `_budget` probes are refreshed per frame, round-robin, and the rest keep last
    * round's values -- which is what the blend pass's hysteresis is for. It is also the tier's weak
    * point: a light change takes a full round to reach every probe and several more to converge.
    * The volume is referenced, not owned: it is also the read path's input and outlives any
    * particular updater.
    * This composites nothing. Whoever samples the atlases afterwards needs its own
    * ShaderSampledRead declaration on them, this class not knowing when that read happens.
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
            /*
            * Probe range. Leave these at 0 to derive them from the volume: they are world-space
            * distances, so a fixed value silently assumes a scene scale. A far plane shorter than
            * the room clips away everything a probe should have seen, the capture comes back nearly
            * empty, the distance moments are meaningless, and the Chebyshev test then rejects every
            * probe around a surface, which the lookup reports as black.
            * Derived: far = the grid's diagonal, a probe being able to see across the volume it
            * belongs to; near = a small fraction of the probe spacing, the smallest feature the
            * grid can resolve.
            */
            float _nearZ = 0.0f;
            float _farZ = 0.0f;
            // Which of VkmScene's cull views this refresh owns. A frame that also renders a camera
            // view must give them different indices: a probe looks in every direction, so the two
            // cannot share a cull result.
            uint32_t _cullViewIndex = 0u;
            /*
            * @brief The shadow atlas the capture consults, if the caller has one.
            * @details Handles rather than a VkmShadowAtlas reference, and optional: per-pass
            * tables are immutable, so what the capture binds has to be known at initialize(),
            * and an updater used without shadows must still bind something. Leave these invalid
            * and the updater creates a 1x1 sentinel of its own, which reads as "nothing
            * occludes" -- so no caller is forced to own an atlas to refresh probes.
            */
            VkmResourceHandle _shadowAtlasTexture{ VKM_INVALID_RESOURCE_HANDLE };
            VkmResourceHandle _shadowAtlasConstants{ VKM_INVALID_RESOURCE_HANDLE };
        };

        // Face tiles per capture-atlas row. Six faces land as a 3x2 block per probe, which keeps the
        // atlas close to square for any budget.
        static constexpr uint32_t kCaptureFacesPerRow = 3u;

        /*
        * @brief The largest budget the push-constant ring can carry.
        * @details Metal and WebGPU hand out a push-constant ring entry per setPushConstants() call.
        * The capture pass pushes once per (probe, face, batch) and the blend pass once per
        * (probe, atlas), so a single-batch frame costs 6*budget + 2*budget entries. Each frame slot
        * gets its own region of kVkmPushConstantRingEntryCount (1024) entries, rewound every frame,
        * so this bounds one frame's pushes: 1024 / 8 = 128.
        * A scene with more than one draw batch costs proportionally more and has to lower the
        * budget itself, the capture's push count scaling with a batch count only the caller knows.
        */
        static constexpr uint32_t kMaxBudget = 128u;

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

        /*
        * @brief Builds the per-material set-3 tables the capture pass binds, where the backend
        * needs them.
        *
        * @details Separate from initialize() because the scene does not exist yet there, and it
        * must follow VkmScene::build(), which is where the material textures are created. A no-op
        * on a backend whose capture shader reaches materials through the bindless array, so every
        * caller can call it unconditionally.
        */
        bool buildMaterialTables(const VkmScene& scene, std::string* outError = nullptr);

        // Probes refreshed by the most recent record(), and their indices. The count can be smaller
        // than the budget on the last frame of a round: the slice is clamped rather than wrapped, so
        // a round refreshes every probe exactly once.
        inline uint32_t getUpdateCount() const { return static_cast<uint32_t>(_slice.size()); }
        inline uint32_t getProbeIndexForSlot(uint32_t slot) const { return _slice[slot]; }

        /*
        * @brief Forgets that a probe was ever refreshed, so its next refresh takes its capture
        * whole instead of blending it into what is stored.
        * @details For a probe that moved. Its cell holds irradiance and distances measured from
        * somewhere it no longer is, and blending at 0.9 hysteresis would leave that wrong depth
        * dominating the Chebyshev test for tens of rounds.
        * @param probeIndex Linear probe index; out-of-range indices are ignored.
        */
        void invalidateProbe(uint32_t probeIndex);

        /*
        * @brief Tells the capture which light to shade with and where its shadow tile is.
        * @details Separate from initialize() because a shadow tile is assigned by
        * VkmShadowAtlas::allocate, which cannot run until the scene is built -- long after the
        * capture's immutable table was created. Only the constant buffer's contents change here,
        * never its binding.
        * @param sun The directional light, with its _shadowTile filled in.
        * @param tilesPerRow Shadow atlas tiles per row; 0 disables the lookup.
        * @param tileSize Shadow atlas tile size in texels.
        */
        void setShadowSun(const VkmPunctualLight& sun, uint32_t tilesPerRow, uint32_t tileSize);

        // Frames a full sweep of the grid takes at the current budget.
        uint32_t getRoundLengthInFrames() const;

        inline const Descriptor& getDescriptor() const { return _descriptor; }
        glm::uvec2 getCaptureAtlasExtent() const;

        /*
        * @brief Frames for a probe's value to fall within a fraction of a step change.
        * @details Exact rather than a heuristic: the blend is a fixed-ratio geometric decay, a
        * probe retaining hysteresis^k of its old value after k refreshes.
        * @param probeCount Probes in the volume.
        * @param budget Probes refreshed per frame.
        * @param hysteresis Weight of the existing value in an update.
        * @param errorFraction Remaining error to converge within.
        * @return The upper bound of the interval. A probe refreshed just before the change waits a
        * whole round for its first update, so the true figure lies in (result - roundLength, result].
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
        std::array<VkmResourceTableBase*, static_cast<size_t>(VkmVertexLayoutPreset::Count)> _captureTables{};
        // One set-3 table per material, per capture permutation. Empty on a backend whose shader
        // samples materials through the bindless array; see VkmSceneMaterialTables.
        std::array<VkmSceneMaterialTables, static_cast<size_t>(VkmVertexLayoutPreset::Count)> _materialTables{};
        VkmPipelineStateBase* _irradianceBlendPipeline = nullptr;
        VkmPipelineStateBase* _distanceBlendPipeline = nullptr;
        VkmResourceTableBase* _irradianceBlendTable = nullptr;
        VkmResourceTableBase* _distanceBlendTable = nullptr;

        std::vector<uint32_t> _slice;
        // Resolved during record() rather than read from _everRefreshed inside the render callback:
        // the callback runs at execute(), by which point every probe in the slice has already been
        // marked refreshed, so it would never see a first refresh as one.
        std::vector<float> _sliceHysteresis;
        // A probe's first refresh blends against a cleared cell, so it takes the capture whole
        // instead of 3% of it. Without this every probe would spend a full convergence just
        // climbing out of black.
        // Bound when the caller supplied no atlas: one texel holding the far sentinel, so the
        // lookup reads "nothing occludes" rather than the table having an unbound entry.
        VkmResourceHandle _shadowStubTexture{ VKM_INVALID_RESOURCE_HANDLE };
        VkmResourceHandle _shadowStubConstants{ VKM_INVALID_RESOURCE_HANDLE };
        VkmProbeCaptureConstants _captureConstantValues{};

        std::vector<bool> _everRefreshed;
        uint32_t _cursor = 0;
        // The atlases are cleared by the first blend pass's load action rather than by a pass of
        // their own. It has to happen: on Vulkan the first render pass transitions them from
        // UNDEFINED, which discards whatever they held.
        bool _atlasesCleared = false;
    };
} // namespace vkm
