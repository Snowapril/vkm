// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/probe_volume.h>
#include <vkm/renderer/probe_volume_updater.h>
#include <vkm/renderer/restir.h>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace vkm
{
    class VkmDriverBase;
    class VkmGBuffer;
    class VkmPipelineStateBase;
    class VkmPipelineStateManager;
    class VkmRenderGraph;
    class VkmResourceTableBase;
    class VkmScene;
    struct VkmFrameData;

    // The two interchangeable techniques behind the section-5 interface (restir.md): both fill
    // the same indirect-radiance texture and the composite neither knows nor cares which.
    enum class VkmGiTechnique : uint32_t
    {
        ProbeVolume = 0, // raster-updated probe grid + optional SSGI contact term; runs everywhere
        Restir = 1,      // ReSTIR GI; needs VkmDriverCapabilityFlags::RayTracing
    };

    // Creation-time choices, fixed for the system's lifetime.
    struct VkmGiSystemDescriptor
    {
        glm::uvec3 _probeCounts{ 20u, 10u, 20u };
        // Probes refreshed per frame; clamped against the push-constant ring in prepareScene().
        uint32_t _probeBudget = 32;
        float _probeHysteresis = 0.9f;
        // How far a lookup steps off its surface before consulting the probes, as a fraction of
        // the probe spacing. Thin two-sided geometry needs this: a curtain sits exactly at the
        // depth its own probe recorded, so a query from the surface reads as self-occluded.
        float _probeNormalBiasFraction = 0.25f;
        // The scene cull view the probe refresh owns; the camera keeps view 0.
        uint32_t _probeCullView = 1;
        /*
        * @brief The shadow atlas the probe capture consults, if the caller has one.
        * @details Forwarded to VkmProbeVolumeUpdater, not owned: shadows are not GI, and the
        * atlas serves the caller's deferred pass too. Handles rather than a reference because
        * the capture's per-pass table is immutable and must name them at initialize(). Leave
        * them invalid and probes capture unshadowed, as they did before shadows existed.
        */
        VkmResourceHandle _shadowAtlasTexture{ VKM_INVALID_RESOURCE_HANDLE };
        VkmResourceHandle _shadowAtlasConstants{ VKM_INVALID_RESOURCE_HANDLE };
    };

    // Per-frame-mutable settings, read at record() time. UI-safe to poke between frames.
    struct VkmGiOptions
    {
        VkmGiTechnique _technique = VkmGiTechnique::ProbeVolume;
        // Probe tier: the additive screen-space contact term.
        bool _ssgi = true;
        // ReSTIR tier: the traced passes see emissive geometry and the environment only (the
        // scene's directional light has no area to hit — restir.md section 12), so this is what
        // lights the indirect term until an area-light representation exists.
        float _environmentRadiance = 1.0f;
        // ReSTIR tier: 8.7's roughness blend toward the pixel's own fresh sample.
        bool _restirMisBlend = true;
        VkmRestirDebugView _restirDebugView = VkmRestirDebugView::Lighting;
    };

    /*
    * @brief The engine's GI: both techniques, their passes and resources, and the runtime switch
    * between them, producing one indirect-radiance texture.
    * @details The section-5 interface made concrete. The system owns everything a technique needs
    * — probe volume, updater, SSGI, the ReSTIR pass with its lifecycle — and a caller owns the
    * shared frame infrastructure it plugs into: the G-buffer, the direct-lighting target, and the
    * composite that consumes getIndirectTexture(). ReSTIR degrades gracefully: where the device
    * reports no ray tracing, or the pipelines or acceleration structures fail, the probe tier is
    * the only entry and _technique is clamped to it.
    * Call order per lifetime: initialize → prepareScene (after VkmScene::build) → resize; per
    * frame: record between the direct-lighting pass and the composite, then advanceFrame right
    * after the G-buffer's own advanceFrame — the ReSTIR tables come in parity pairs keyed to
    * that flip count.
    */
    class VkmGiSystem
    {
    public:
        VkmGiSystem() = default;
        ~VkmGiSystem() = default;

        VkmGiSystem(const VkmGiSystem&) = delete;
        VkmGiSystem& operator=(const VkmGiSystem&) = delete;

        /*
        * @brief Creates the technique-independent pieces: pipelines, sampler, constant buffers,
        * and — where the device reports ray tracing — the ray-tracing PSO cache, loaded exactly
        * once. `gbuffer` must outlive the system.
        */
        bool initialize(VkmDriverBase* driver, VkmPipelineStateManager* pipelineStateManager,
                        VkmGBuffer* gbuffer, const VkmGiSystemDescriptor& descriptor,
                        std::string* outError);

        /*
        * @brief Fits the probe grid to the scene's bounds, builds the probe updater and its
        * material tables, and attempts the acceleration structures that make ReSTIR selectable.
        * @details Must follow VkmScene::build(). An acceleration-structure failure downgrades to
        * the probe technique rather than failing the call. `scene` must outlive the system.
        */
        bool prepareScene(VkmScene* scene, std::string* outError);

        /*
        * @brief Recreates the extent-sized pieces: the indirect target, the per-pass tables and
        * the ReSTIR pass. Call after the G-buffer has been resized to the same extent.
        * @param directTarget The caller's direct-lighting target, sampled by the SSGI term.
        */
        bool resize(const glm::uvec2& extent, VkmResourceHandle directTarget, std::string* outError);

        /*
        * @brief Records the active technique's passes into `renderGraph`, filling the indirect
        * target. Call between the direct-lighting pass and whatever consumes the texture.
        */
        void record(VkmRenderGraph* renderGraph, const VkmFrameData& frameData, uint32_t frameIndex);

        // Call once per frame, immediately after VkmGBuffer::advanceFrame().
        void advanceFrame();

        void destroy();

        inline VkmResourceHandle getIndirectTexture() const { return _indirectTarget; }
        // Whether the ReSTIR technique can be selected on this device and scene.
        inline bool isRestirAvailable() const { return _restirAvailable && _restir != nullptr; }
        // Mutable: the UI pokes these between frames. A technique the device cannot run is
        // clamped back at record() time rather than rejected here.
        inline VkmGiOptions& options() { return _options; }
        inline const VkmGiOptions& options() const { return _options; }

        inline const VkmProbeVolume& getProbeVolume() const { return _volume; }
        inline const VkmProbeVolumeUpdater& getProbeUpdater() const { return _updater; }
        // The volume's shader constants, filled once in prepareScene(). Exposed so a debug view of
        // the grid addresses it with the same origin, spacing and counts the lookup uses.
        inline VkmResourceHandle getProbeVolumeBuffer() const { return _volumeBuffer; }

        /*
        * @brief Moves one probe off its grid position and republishes the volume.
        * @details The single entry point for probe placement: it uploads the offset texture the
        * lookup reads and invalidates the probe, so the refresh that reaches it takes its capture
        * whole rather than blending it into what was measured at the old position. Editing the
        * volume directly would do the first and skip the second.
        * @param probeIndex Linear probe index; out of range is a no-op returning false.
        * @param offset World-space displacement from the probe's grid position.
        * @return Whether the offset was applied and uploaded.
        */
        /*
        * @brief Tells the probe capture which light to shade with and where its shadow tile is.
        * @details Must follow VkmShadowAtlas::allocate, which is what assigns the tile. Without
        * it the capture shades unshadowed, which is what it did before shadows existed.
        */
        void setShadowSun(const VkmPunctualLight& sun, uint32_t tilesPerRow, uint32_t tileSize);

        bool setProbeOffset(uint32_t probeIndex, const glm::vec3& offset);
        // Returns every probe to its grid position, invalidating all of them.
        bool clearProbeOffsets();

    private:
        struct Retired
        {
            uint64_t _retiredAtFrame = 0;
            std::vector<VkmResourceTableBase*> _tables;
            std::unique_ptr<VkmRestirPass> _restir;
        };

        VkmRestirOptions makeRestirOptions() const;
        void recordProbeTier(VkmRenderGraph* renderGraph, const VkmFrameData& frameData);
        void recordRestirTier(VkmRenderGraph* renderGraph, uint32_t frameIndex);
        void drainRetired();
        void releaseTables();

        VkmDriverBase* _driver = nullptr;
        VkmPipelineStateManager* _pipelineStateManager = nullptr;
        VkmGBuffer* _gbuffer = nullptr;
        VkmScene* _scene = nullptr;
        VkmGiSystemDescriptor _descriptor{};
        VkmGiOptions _options{};

        VkmProbeVolume _volume;
        VkmProbeVolumeUpdater _updater;
        std::unique_ptr<VkmRestirPass> _restir;
        std::vector<Retired> _retired;

        VkmPipelineStateBase* _probeLightingPipeline = nullptr;
        VkmPipelineStateBase* _ssgiPipeline = nullptr;
        VkmResourceTableBase* _probeLightingTable = nullptr;
        VkmResourceTableBase* _ssgiTable = nullptr;

        VkmResourceHandle _sampler{ VKM_INVALID_RESOURCE_HANDLE };
        VkmResourceHandle _volumeBuffer{ VKM_INVALID_RESOURCE_HANDLE };
        VkmResourceHandle _ssgiBuffer{ VKM_INVALID_RESOURCE_HANDLE };
        VkmResourceHandle _indirectTarget{ VKM_INVALID_RESOURCE_HANDLE };

        glm::uvec2 _extent{ 0, 0 };
        uint64_t _frameCounter = 0;
        uint32_t _gbufferParity = 0;
        bool _rtPipelinesLoaded = false;
        bool _restirAvailable = false;
        bool _sceneReady = false;
    };
} // namespace vkm
