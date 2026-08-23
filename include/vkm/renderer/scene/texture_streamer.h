// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/scene/image_loader.h>

#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace vkm
{
    class VkmDriverBase;

    /*
    * @brief How the camera sees the scene for one streaming tick.
    * @details A plain record rather than a VkmCamera, so the selection maths is testable without
    * one and the scene layer keeps no dependency on the camera header.
    */
    struct VkmTextureStreamingView
    {
        glm::vec3 _cameraPosition{ 0.0f };
        uint32_t _viewportHeight = 0;
        float _fovYRadians = 0.0f;
    };

    /*
    * @brief One drawable's contribution to mip selection.
    * @details World space, already transformed: the streamer never sees an object transform, so
    * the scene is the only place that has to know a bounding sphere is stored in object space.
    */
    struct VkmTextureStreamingObject
    {
        glm::vec3 _worldCenter{ 0.0f };
        float _worldRadius = 0.0f;
        uint32_t _materialIndex = 0;
    };

    /*
    * @brief One material texture slot the streamer has just re-pointed at a new resource.
    * @details The scene applies these into its own VkmMaterialData records; the streamer never
    * touches them, having no material array of its own.
    */
    struct VkmTextureStreamingUpdate
    {
        uint32_t _materialIndex = 0;
        // Index into VkmMaterialData::_textureSlots: 0 base colour, 1 metallic-roughness,
        // 2 normal, 3 emissive.
        uint32_t _channel = 0;
        uint32_t _bindlessSlot = INVALID_VALUE32;
        uint32_t _baseMip = 0;
        uint32_t _totalMipCount = 1;
    };

    // Levels a histogram row exists for. 16 covers a 32768-texel chain; anything deeper is folded
    // into the last row rather than dropped.
    inline constexpr uint32_t kVkmStreamingHistogramLevels = 16;

    /*
    * @brief What streaming currently costs, and what it would have cost without.
    * @details The pairing is the point: resident bytes alone say nothing, and the same figure beside
    * the full-chain baseline is the whole readout. Both cover only the textures the streamer owns,
    * so neither includes render targets, probe atlases or anything else in the texture category.
    */
    struct VkmTextureStreamingStats
    {
        uint32_t _textureCount = 0;
        // What every entry's currently resident levels occupy.
        uint64_t _residentBytes = 0;
        // What those same textures would occupy holding their whole chain -- the counterfactual.
        uint64_t _fullChainBytes = 0;
        // Rebuilds published since the scene was built, so a panel can show churn rather than
        // implying the state is static.
        uint32_t _rebuildsApplied = 0;
        /*
        * Live GPU bytes exceed _residentBytes while these are non-zero: a rebuild in flight holds
        * the old texture and the new one at once, and a retired entry is still allocated until its
        * delay expires. Reported so a reader can account for the difference against the pool's
        * category total rather than mistrusting both numbers.
        */
        bool _rebuildInFlight = false;
        uint32_t _pendingRetireCount = 0;
        /*
        * Textures pinned at whatever they hold because a rebuild failed. They sit at
        * resident == full chain forever and so quietly drag the saving down; reported separately
        * rather than left to look like textures the camera simply wants at level 0.
        */
        uint32_t _failedCount = 0;
        // How many textures sit at each base level; index 0 is "whole chain resident".
        std::array<uint16_t, kVkmStreamingHistogramLevels> _levelHistogram{};
    };

    struct VkmTextureStreamingSettings
    {
        bool _enabled = true;
        /*
        * Added to the selected level: negative keeps more detail resident than the screen strictly
        * needs, positive trades sharpness for memory. 0 matches texel density to pixel density.
        * The selection measures a bounding sphere rather than the real UV parameterisation, so
        * this is the knob that absorbs the difference for a given scene.
        */
        int32_t _mipBias = 0;
        // Consecutive ticks a differing target must hold before a rebuild is queued. Re-reading
        // and re-decoding the file is the expensive half of a rebuild, so damping matters here.
        uint32_t _stableTickCount = 8;
        /*
        * Mip levels uploaded per tick, which is what bounds the worst-case hitch: wherever the
        * destination's memory is not host-writable, VkmDriverBase::uploadToTexture submits and
        * blocks once per level. A rebuild is therefore spread over as many ticks as it needs, and
        * the new texture is published only once every one of its levels is in place.
        */
        uint32_t _maxLevelUploadsPerTick = 2;
    };

    /*
    * @brief The mip level whose texel density matches how large a bounded object appears on screen.
    * @details Free-standing so the selection can be tested without a driver, a scene or a camera.
    * The object's projected diameter in pixels is compared against the texture's width; each
    * halving of that ratio costs one level, which is exactly what a mip chain encodes.
    * @param view Camera for this tick. A zero viewport height or field of view yields level 0,
    * there being no projection to measure against.
    * @param distance Distance from the camera to the near side of the bounding sphere. Clamped
    * away from zero internally.
    * @param worldRadius The object's bounding-sphere radius, in world units.
    * @param textureWidth Width of the texture's level 0, in texels.
    * @param totalMipCount Levels the full chain has, counting level 0.
    * @param mipBias Added to the result before clamping.
    * @return The base level to keep resident, in [0, totalMipCount - 1].
    */
    uint32_t vkmSelectStreamingBaseMip(const VkmTextureStreamingView& view, float distance, float worldRadius,
                                       uint32_t textureWidth, uint32_t totalMipCount, int32_t mipBias);

    /*
    * @brief The debug name a material texture carries, so the texture browser can tell them apart.
    * @details Every material texture used to be named "SceneMaterialTexture", which made the
    * browser's list unreadable and a rebuild impossible to follow. This keeps that prefix -- the
    * browser's name filter isolates the group with it -- and appends the image's file stem plus its
    * colour space, the two things that make an entry unique.
    * @param path Image file the texture was decoded from.
    * @param srgb Colour space the chain was built in; one file used both ways is two textures.
    * @return The name, e.g. "SceneMaterialTexture:sponza_curtain_diff(srgb)".
    */
    std::string vkmMaterialTextureDebugName(const std::string& path, bool srgb);

    /*
    * @brief How wide a bounding sphere appears on screen, in pixels.
    * @details The half of the selection that depends only on the object, so a material shared by
    * many objects can be reduced to its largest value once and then measured against each of its
    * textures -- the level falls monotonically as this grows, so the largest wins for all of them.
    * @param view Camera for this tick.
    * @param distance Distance from the camera to the near side of the sphere. Clamped internally.
    * @param worldRadius The sphere's radius, in world units.
    * @return The projected diameter in pixels, or 0 where there is no projection to measure.
    */
    float vkmProjectedSphereDiameterPixels(const VkmTextureStreamingView& view, float distance, float worldRadius);

    /*
    * @brief The half of the selection that depends on the texture: which level matches a footprint.
    * @param projectedPixels What vkmProjectedSphereDiameterPixels returned.
    * @param textureWidth Width of the texture's level 0, in texels.
    * @param totalMipCount Levels the full chain has, counting level 0.
    * @param mipBias Added to the result before clamping.
    * @return The base level to keep resident, in [0, totalMipCount - 1].
    */
    uint32_t vkmStreamingBaseMipForProjection(float projectedPixels, uint32_t textureWidth,
                                              uint32_t totalMipCount, int32_t mipBias);

    /*
    * @brief Keeps each material texture at the mip range the camera actually needs, by rebuilding
    * the texture rather than by narrowing a view.
    *
    * @details A streamed texture holds levels [base, last] of its chain and nothing above them, so
    * its allocation shrinks with distance -- a view whose mip range merely excluded level 0 would
    * still pay for it. Changing the level therefore means a new VkmTexture, a new bindless slot,
    * and a rewrite of every material record naming the old one; UVs being normalized, no shader
    * change comes with it.
    *
    * One rebuild is in flight at a time. That bounds worker memory to a single decoded chain and
    * keeps the upload cost per tick to _maxLevelUploadsPerTick, at the price of converging over
    * several frames after a large camera move.
    *
    * Threading: a single worker does CPU work only -- decoding the file and rebuilding the chain.
    * Every driver call happens on the thread that calls update(). On WASM, where no worker can be
    * spawned, the same work runs inline in update().
    *
    * Lifetime: the displaced texture and slot are held for several ticks before release. Material
    * textures are not declared to the render graph, so they carry no recorded GPU usage for
    * VkmDeferredResourceReclaimer to wait on, and the slot allocator hands a released slot straight
    * back out -- a submitted frame still sampling either is the hazard that delay covers.
    */
    class VkmTextureStreamer
    {
    public:
        VkmTextureStreamer() = default;
        ~VkmTextureStreamer();

        VkmTextureStreamer(const VkmTextureStreamer&) = delete;
        VkmTextureStreamer& operator=(const VkmTextureStreamer&) = delete;

        inline void setSettings(const VkmTextureStreamingSettings& settings) { _settings = settings; }
        inline const VkmTextureStreamingSettings& getSettings() const { return _settings; }

        /*
        * @brief Takes ownership of one already-created texture and starts tracking it.
        * @details Called once per unique (path, colour space) as the scene uploads it. The texture
        * must already hold levels [residentBaseMip, totalMipCount - 1] and be registered at
        * bindlessSlot.
        * @param path File the pixels came from; re-read whenever the resident range changes.
        * @param srgb Colour space the chain was built in, which decides how levels are averaged.
        * @param baseExtent Extent of level 0, which every resident extent is derived from.
        * @param totalMipCount Levels the full chain has.
        * @param residentBaseMip Level the texture currently starts at.
        * @param texture The live texture.
        * @param bindlessSlot Slot it is registered at.
        * @return Index of the new entry, for addReference().
        */
        uint32_t addTexture(std::string path, bool srgb, const glm::uvec2& baseExtent, uint32_t totalMipCount,
                            uint32_t residentBaseMip, VkmResourceHandle texture, uint32_t bindlessSlot);

        // Records that one material channel samples an entry, so a rebuild knows what to rewrite.
        void addReference(uint32_t entryIndex, uint32_t materialIndex, uint32_t channel);

        /*
        * @brief The entry a material channel samples, or INVALID_VALUE32 where it samples none.
        * @details Lets the scene publish the initial mip words without repeating this bookkeeping.
        */
        uint32_t findEntry(uint32_t materialIndex, uint32_t channel) const;

        inline uint32_t getEntryCount() const { return static_cast<uint32_t>(_entries.size()); }
        uint32_t getResidentBaseMip(uint32_t entryIndex) const;
        uint32_t getTotalMipCount(uint32_t entryIndex) const;

        /*
        * @brief Sums what the streamed textures occupy now against what their full chains would.
        * @details Cheap enough to call every frame -- it walks the entry list and does no I/O and
        * no allocation.
        * Takes no lock, and must therefore be called from the same thread that calls update():
        * the mutex guards only the decode worker's job and result queues, while the entry list this
        * reads is owned outright by the updating thread.
        */
        VkmTextureStreamingStats computeStats() const;

        // Spawns the decode worker. A no-op on WASM, where update() decodes inline.
        void start();

        /*
        * @brief Advances the streaming state by one frame: picks targets, moves an in-flight
        * rebuild along, and releases what has aged out.
        * @details The one entry point that touches the driver, and the only one meant to be called
        * per frame. Does nothing while disabled, so the cost of the feature being off is a branch.
        * Must be called once per rendered frame and before that frame records anything, the retire
        * delay being counted in calls to this.
        * @param driver Driver the textures belong to.
        * @param view Camera for this tick.
        * @param objects Every drawable, in world space.
        * @param outUpdates Receives one record per material channel whose slot changed. Appended
        * to, never cleared.
        */
        void update(VkmDriverBase* driver, const VkmTextureStreamingView& view,
                    const std::vector<VkmTextureStreamingObject>& objects,
                    std::vector<VkmTextureStreamingUpdate>* outUpdates);

        /*
        * @brief Stops the worker and releases every texture and slot the streamer owns.
        * @details Releases immediately rather than on the retire delay: a caller tearing the scene
        * down has already drained the queue.
        */
        void destroy(VkmDriverBase* driver);

    private:
        struct Entry
        {
            std::string _path;
            /*
            * Debug name every rebuild of this texture reuses, so the texture browser shows one
            * followable row per asset rather than a wall of identical ones. Owned here because
            * VkmTextureInfo::_debugName is a borrowed pointer the texture keeps a copy of.
            * Deliberately stable across rebuilds: the browser's own extent and mip columns are
            * what show the level moving.
            */
            std::string _debugName;
            bool _srgb = false;
            glm::uvec2 _baseExtent{ 0, 0 };
            uint32_t _totalMipCount = 1;
            uint32_t _residentBaseMip = 0;
            VkmResourceHandle _texture{ VKM_INVALID_RESOURCE_HANDLE };
            uint32_t _bindlessSlot = INVALID_VALUE32;
            // (material index, channel) pairs naming this entry.
            std::vector<std::pair<uint32_t, uint32_t>> _references;
            // Level the selection last asked for, and how many consecutive ticks it has asked it.
            uint32_t _candidateBaseMip = 0;
            uint32_t _candidateTickCount = 0;
            // Set once a rebuild has failed, which pins the entry at whatever it already holds.
            // A file that cannot be re-read will not start reading correctly on a later tick, and
            // retrying would re-pay the decode every time the camera asked again.
            bool _streamingFailed = false;
        };

        struct Job
        {
            uint32_t _entryIndex = 0;
            std::string _path;
            bool _srgb = false;
            uint32_t _baseMip = 0;
        };

        struct Result
        {
            uint32_t _entryIndex = 0;
            uint32_t _baseMip = 0;
            // Levels [_baseMip, totalMipCount - 1], in chain order. Empty when the decode failed.
            std::vector<VkmImageData> _levels;
        };

        // A decoded chain being uploaded into its new texture, a bounded number of levels per tick.
        struct PendingBuild
        {
            bool _active = false;
            uint32_t _entryIndex = 0;
            uint32_t _baseMip = 0;
            std::vector<VkmImageData> _levels;
            VkmResourceHandle _texture{ VKM_INVALID_RESOURCE_HANDLE };
            uint32_t _uploadedLevelCount = 0;
        };

        struct Retired
        {
            VkmResourceHandle _texture{ VKM_INVALID_RESOURCE_HANDLE };
            uint32_t _bindlessSlot = INVALID_VALUE32;
            uint64_t _readyTick = 0;
        };

        void stopWorker();
        void selectTargets(const VkmTextureStreamingView& view,
                           const std::vector<VkmTextureStreamingObject>& objects);
        // Queues at most one decode, for the entry whose target differs and has held longest.
        void queueJob();
        void advanceBuild(VkmDriverBase* driver, std::vector<VkmTextureStreamingUpdate>* outUpdates);
        void abandonBuild(VkmDriverBase* driver);
        void drainRetired(VkmDriverBase* driver, bool force);
        void workerLoop();
        // Decodes one job into a result. Runs on the worker, or inline on WASM.
        static Result runJob(const Job& job);

        VkmTextureStreamingSettings _settings;
        std::vector<Entry> _entries;
        std::vector<Retired> _retired;
        PendingBuild _build;
        // Entry a decode is running for, or INVALID_VALUE32. One at a time, engine-wide.
        uint32_t _jobEntryIndex = INVALID_VALUE32;
        uint64_t _tickCounter = 0;
        // Rebuilds published, for computeStats. Cumulative over the scene's life.
        uint32_t _rebuildsApplied = 0;
        // One past the largest material index any entry references, which sizes the reduction below.
        uint32_t _materialCount = 0;
        // Scratch for selectTargets, kept across ticks so a per-frame pass allocates nothing.
        std::vector<uint32_t> _desiredBaseMips;
        std::vector<float> _materialProjectedPixels;

        mutable std::mutex _mutex;
        std::condition_variable _cv;
        std::deque<Job> _jobs;
        std::deque<Result> _results;

        std::thread _workerThread;
        std::atomic<bool> _running{ false };
    };
} // namespace vkm
