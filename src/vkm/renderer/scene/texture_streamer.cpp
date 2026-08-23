// Copyright (c) 2026 Snowapril

#include <vkm/renderer/scene/texture_streamer.h>

#include <vkm/base/common.h>
#include <vkm/base/cpu_profiler.h>
#include <vkm/renderer/backend/common/bindless_resource_manager.h>
#include <vkm/renderer/backend/common/deferred_resource_reclaimer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/common/texture.h>

#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace vkm
{
    namespace
    {
        /*
        * Ticks a displaced texture and slot are held before release.
        *
        * update() runs before the frame it belongs to records anything, so the last frame that can
        * still sample the old slot is the one recorded on the previous tick. A frame's render()
        * waits on the graph FRAME_BUFFER_COUNT slots back, which puts the previous tick's frame
        * provably complete FRAME_BUFFER_COUNT ticks later. The extra tick is slack for a frame
        * whose render() was skipped entirely -- a minimized or live-resizing window still ticks
        * update(), so ticks and completed frames are not one-for-one.
        */
        constexpr uint64_t kRetireTickDelay = FRAME_BUFFER_COUNT + 1;

        // Extent of one level of a chain whose level 0 is `baseExtent`, which is what every
        // backend's copyBufferToTexture computes for that level.
        glm::uvec2 levelExtent(const glm::uvec2& baseExtent, uint32_t level)
        {
            return glm::uvec2(std::max(1u, baseExtent.x >> level), std::max(1u, baseExtent.y >> level));
        }
    } // namespace

    float vkmProjectedSphereDiameterPixels(const VkmTextureStreamingView& view, float distance, float worldRadius)
    {
        if (view._viewportHeight == 0u || view._fovYRadians <= 0.0f || worldRadius <= 0.0f)
        {
            return 0.0f;
        }

        const float halfFovTangent = std::tan(view._fovYRadians * 0.5f);
        if (halfFovTangent <= 0.0f)
        {
            return 0.0f;
        }

        // Guarding the distance rather than the result keeps a camera inside the bounds at the
        // finest level instead of dividing by zero.
        const float safeDistance = std::max(distance, 1e-3f);
        return (2.0f * worldRadius * static_cast<float>(view._viewportHeight)) /
               (2.0f * safeDistance * halfFovTangent);
    }

    uint32_t vkmStreamingBaseMipForProjection(float projectedPixels, uint32_t textureWidth,
                                              uint32_t totalMipCount, int32_t mipBias)
    {
        const uint32_t lastLevel = (totalMipCount == 0u) ? 0u : (totalMipCount - 1u);
        if (lastLevel == 0u || textureWidth == 0u)
        {
            return 0;
        }
        // Nothing to measure against: keep everything rather than guess.
        if (projectedPixels <= 0.0f)
        {
            return 0;
        }

        // One level per halving of texels-per-pixel, which is what a mip chain stores.
        const float levels = std::log2(static_cast<float>(textureWidth) / projectedPixels);
        const float biased = std::round(levels) + static_cast<float>(mipBias);
        if (biased <= 0.0f)
        {
            return 0;
        }
        if (biased >= static_cast<float>(lastLevel))
        {
            return lastLevel;
        }
        return static_cast<uint32_t>(biased);
    }

    uint32_t vkmStreamingBaseMipFromFeedback(uint32_t reported, uint32_t residentBaseMip,
                                             uint32_t totalMipCount)
    {
        const uint32_t lastLevel = (totalMipCount == 0u) ? 0u : (totalMipCount - 1u);
        const uint32_t absolute = residentBaseMip + std::min(reported, kVkmTextureFeedbackMaxLevel);
        return std::min(absolute, lastLevel);
    }

    uint32_t vkmSelectStreamingBaseMip(const VkmTextureStreamingView& view, float distance, float worldRadius,
                                       uint32_t textureWidth, uint32_t totalMipCount, int32_t mipBias)
    {
        return vkmStreamingBaseMipForProjection(vkmProjectedSphereDiameterPixels(view, distance, worldRadius),
                                                textureWidth, totalMipCount, mipBias);
    }

    std::string vkmMaterialTextureDebugName(const std::string& path, bool srgb)
    {
        // Hand-rolled rather than std::filesystem: this runs per texture at scene load on every
        // platform including wasm, and the last separator is the whole of what is needed.
        const size_t slash = path.find_last_of("/\\");
        const size_t start = (slash == std::string::npos) ? 0 : slash + 1;
        const size_t dot = path.find_last_of('.');
        const size_t end = (dot == std::string::npos || dot < start) ? path.size() : dot;

        std::string name = "SceneMaterialTexture:";
        name.append(path, start, end - start);
        name += srgb ? "(srgb)" : "(linear)";
        return name;
    }

    VkmTextureStreamer::~VkmTextureStreamer()
    {
        stopWorker();
    }

    uint32_t VkmTextureStreamer::addTexture(std::string path, bool srgb, const glm::uvec2& baseExtent,
                                            uint32_t totalMipCount, uint32_t residentBaseMip,
                                            VkmResourceHandle texture, uint32_t bindlessSlot)
    {
        Entry entry;
        entry._debugName = vkmMaterialTextureDebugName(path, srgb);
        entry._path = std::move(path);
        entry._srgb = srgb;
        entry._baseExtent = baseExtent;
        entry._totalMipCount = std::max(1u, totalMipCount);
        entry._residentBaseMip = residentBaseMip;
        entry._texture = texture;
        entry._bindlessSlot = bindlessSlot;
        entry._candidateBaseMip = residentBaseMip;

        _entries.push_back(std::move(entry));
        return static_cast<uint32_t>(_entries.size() - 1u);
    }

    void VkmTextureStreamer::addReference(uint32_t entryIndex, uint32_t materialIndex, uint32_t channel)
    {
        VKM_ASSERT(entryIndex < _entries.size(), "VkmTextureStreamer::addReference entry index is out of range");
        _entries[entryIndex]._references.emplace_back(materialIndex, channel);
        _materialCount = std::max(_materialCount, materialIndex + 1u);
    }

    uint32_t VkmTextureStreamer::findEntry(uint32_t materialIndex, uint32_t channel) const
    {
        for (size_t i = 0; i < _entries.size(); ++i)
        {
            for (const std::pair<uint32_t, uint32_t>& reference : _entries[i]._references)
            {
                if (reference.first == materialIndex && reference.second == channel)
                {
                    return static_cast<uint32_t>(i);
                }
            }
        }
        return INVALID_VALUE32;
    }

    uint32_t VkmTextureStreamer::getResidentBaseMip(uint32_t entryIndex) const
    {
        return (entryIndex < _entries.size()) ? _entries[entryIndex]._residentBaseMip : 0u;
    }

    uint32_t VkmTextureStreamer::getTotalMipCount(uint32_t entryIndex) const
    {
        return (entryIndex < _entries.size()) ? _entries[entryIndex]._totalMipCount : 1u;
    }

    VkmTextureStreamingStats VkmTextureStreamer::computeStats() const
    {
        VkmTextureStreamingStats stats;
        stats._textureCount = static_cast<uint32_t>(_entries.size());
        stats._rebuildsApplied = _rebuildsApplied;
        stats._rebuildInFlight = _build._active || _jobEntryIndex != INVALID_VALUE32;
        stats._pendingRetireCount = static_cast<uint32_t>(_retired.size());

        for (const Entry& entry : _entries)
        {
            const glm::uvec3 extent(entry._baseExtent.x, entry._baseExtent.y, 1);
            const VkmFormat format = entry._srgb ? VkmFormat::R8G8B8A8_SRGB : VkmFormat::R8G8B8A8_UNORM;

            stats._residentBytes += vkmMipRangeByteSize(extent, /*numArrayLayers=*/1, format,
                                                        entry._residentBaseMip,
                                                        entry._totalMipCount - entry._residentBaseMip);
            stats._fullChainBytes += vkmMipRangeByteSize(extent, /*numArrayLayers=*/1, format,
                                                         /*baseLevel=*/0, entry._totalMipCount);

            const uint32_t bucket = std::min(entry._residentBaseMip, kVkmStreamingHistogramLevels - 1u);
            ++stats._levelHistogram[bucket];

            if (entry._streamingFailed)
            {
                ++stats._failedCount;
            }
            if (entry._feedbackBaseMip != INVALID_VALUE32)
            {
                ++stats._feedbackCount;
            }
        }
        return stats;
    }

    void VkmTextureStreamer::start()
    {
#ifdef VKM_PLATFORM_WASM
        // No background thread there; update() runs the decode inline instead.
        return;
#else
        if (_running.exchange(true))
        {
            return;
        }
        _workerThread = std::thread(&VkmTextureStreamer::workerLoop, this);
#endif
    }

    void VkmTextureStreamer::applyFeedback(const uint32_t* feedback, uint32_t count)
    {
        for (Entry& entry : _entries)
        {
            // Cleared unconditionally: an entry nothing sampled this frame must fall back to the
            // estimate rather than keep answering with a reading from whenever it was last visible.
            entry._feedbackBaseMip = INVALID_VALUE32;

            if (feedback == nullptr || entry._bindlessSlot >= count)
            {
                continue;
            }
            const uint32_t reported = feedback[entry._bindlessSlot];
            if (reported == kVkmTextureFeedbackUnused)
            {
                continue;
            }

            /*
            * What the shader measured is relative to the texture it sampled, so the offset is the
            * chain level that texture's level 0 is. A rebuilt texture is physically only what it
            * holds, making that its resident base; a sparse one keeps its full extent whatever is
            * backed, so its level 0 is chain level 0 and adding anything would count twice.
            */
            const uint32_t sampledBaseLevel = entry._sparse ? 0u : entry._residentBaseMip;
            entry._feedbackBaseMip =
                vkmStreamingBaseMipFromFeedback(reported, sampledBaseLevel, entry._totalMipCount);
        }
    }

    void VkmTextureStreamer::stopWorker()
    {
        {
            // Under the lock, because the worker waits on an untimed predicate that reads this:
            // clearing it between that predicate's evaluation and the wait it precedes would lose
            // the notify below, and the join would then never return.
            std::lock_guard<std::mutex> lock(_mutex);
            if (_running.exchange(false) == false)
            {
                return;
            }
        }
        _cv.notify_all();
        if (_workerThread.joinable())
        {
            _workerThread.join();
        }
    }

    VkmTextureStreamer::Result VkmTextureStreamer::runJob(const Job& job)
    {
        Result result;
        result._entryIndex = job._entryIndex;
        result._baseMip = job._baseMip;

        VkmImageData base;
        std::string error;
        if (!loadImageFromFile(job._path, &base, &error))
        {
            VKM_DEBUG_WARN(("Streaming texture '" + job._path + "' could not be re-decoded (" + error +
                            "); it stays at the level it already holds").c_str());
            return result;
        }

        // The chain builds downward from level 0, so reaching level N means producing every level
        // above it and dropping them. That is the cost of holding no pixels between rebuilds.
        std::vector<VkmImageData> chain;
        vkmBuildMipChain(base, job._srgb, &chain);

        const uint32_t totalLevels = 1u + static_cast<uint32_t>(chain.size());
        if (job._baseMip >= totalLevels)
        {
            return result;
        }

        result._levels.reserve(totalLevels - job._baseMip);
        for (uint32_t level = job._baseMip; level < totalLevels; ++level)
        {
            result._levels.push_back((level == 0u) ? std::move(base) : std::move(chain[level - 1u]));
        }
        return result;
    }

    void VkmTextureStreamer::workerLoop()
    {
        VKM_PROFILE_SET_THREAD_NAME("TextureStreamer");

        while (_running.load(std::memory_order_relaxed))
        {
            Job job;
            {
                std::unique_lock<std::mutex> lock(_mutex);
                _cv.wait(lock, [this] { return !_running.load(std::memory_order_relaxed) || !_jobs.empty(); });
                if (!_running.load(std::memory_order_relaxed))
                {
                    break;
                }
                job = std::move(_jobs.front());
                _jobs.pop_front();
            }

            Result result = runJob(job);
            {
                std::lock_guard<std::mutex> lock(_mutex);
                _results.push_back(std::move(result));
            }
        }
    }

    void VkmTextureStreamer::selectTargets(const VkmTextureStreamingView& view,
                                           const std::vector<VkmTextureStreamingObject>& objects)
    {
        /*
        * Two passes rather than one pass per texture over every object. The level a texture wants
        * falls monotonically as its footprint grows, so the largest footprint any object of a
        * material projects decides that material's level for all four of its textures -- reducing
        * the objects once and then measuring each texture against the result is the same answer
        * for O(objects + references) instead of O(textures x objects x references).
        *
        * Largest footprint means closest: anything coarser would under-resolve that object, and a
        * texture is one resource however many objects share it.
        */
        _materialProjectedPixels.assign(_materialCount, 0.0f);
        for (const VkmTextureStreamingObject& object : objects)
        {
            if (object._materialIndex >= _materialCount)
            {
                continue;
            }
            const float distance = glm::length(object._worldCenter - view._cameraPosition) - object._worldRadius;
            const float projected = vkmProjectedSphereDiameterPixels(view, distance, object._worldRadius);
            float& best = _materialProjectedPixels[object._materialIndex];
            best = std::max(best, projected);
        }

        _desiredBaseMips.assign(_entries.size(), INVALID_VALUE32);
        for (size_t entryIndex = 0; entryIndex < _entries.size(); ++entryIndex)
        {
            const Entry& entry = _entries[entryIndex];
            if (entry._streamingFailed)
            {
                continue;
            }

            /*
            * What the screen actually sampled beats what a bounding sphere predicts, so where the
            * GPU reported a level the estimate is not consulted at all. The estimate cannot see UV
            * density, grazing angles or occlusion; the reading is the ground truth for all three.
            */
            if (entry._feedbackBaseMip != INVALID_VALUE32)
            {
                const int32_t biased =
                    static_cast<int32_t>(entry._feedbackBaseMip) + _settings._mipBias;
                const int32_t lastLevel = static_cast<int32_t>(entry._totalMipCount) - 1;
                _desiredBaseMips[entryIndex] =
                    static_cast<uint32_t>(std::clamp(biased, 0, std::max(0, lastLevel)));
                continue;
            }

            for (const std::pair<uint32_t, uint32_t>& reference : entry._references)
            {
                if (reference.first >= _materialCount)
                {
                    continue;
                }
                const float projected = _materialProjectedPixels[reference.first];
                if (projected <= 0.0f)
                {
                    continue; // nothing visible drew this material this tick
                }

                const uint32_t desired = vkmStreamingBaseMipForProjection(
                    projected, entry._baseExtent.x, entry._totalMipCount, _settings._mipBias);
                uint32_t& best = _desiredBaseMips[entryIndex];
                best = (best == INVALID_VALUE32) ? desired : std::min(best, desired);
            }
        }

        for (size_t entryIndex = 0; entryIndex < _entries.size(); ++entryIndex)
        {
            Entry& entry = _entries[entryIndex];
            const uint32_t desired = _desiredBaseMips[entryIndex];
            if (desired == INVALID_VALUE32)
            {
                // Nothing visible names it this tick; leave it where it is rather than evicting on
                // a frame the object happens to be culled out of the object list.
                entry._candidateBaseMip = entry._residentBaseMip;
                entry._candidateTickCount = 0;
                continue;
            }

            if (desired == entry._candidateBaseMip)
            {
                if (entry._candidateTickCount < _settings._stableTickCount)
                {
                    ++entry._candidateTickCount;
                }
            }
            else
            {
                entry._candidateBaseMip = desired;
                entry._candidateTickCount = 0;
            }
        }
    }

    void VkmTextureStreamer::queueJob()
    {
        uint32_t bestEntry = INVALID_VALUE32;
        uint32_t bestDistance = 0;

        for (size_t entryIndex = 0; entryIndex < _entries.size(); ++entryIndex)
        {
            const Entry& entry = _entries[entryIndex];
            if (entry._streamingFailed || entry._candidateBaseMip == entry._residentBaseMip ||
                entry._candidateTickCount < _settings._stableTickCount)
            {
                continue;
            }

            // Furthest from where it should be goes first, so a camera that jumps resolves the
            // worst-looking surfaces before the marginal ones.
            const uint32_t levelDistance = (entry._candidateBaseMip > entry._residentBaseMip)
                                               ? (entry._candidateBaseMip - entry._residentBaseMip)
                                               : (entry._residentBaseMip - entry._candidateBaseMip);
            if (bestEntry == INVALID_VALUE32 || levelDistance > bestDistance)
            {
                bestEntry = static_cast<uint32_t>(entryIndex);
                bestDistance = levelDistance;
            }
        }

        if (bestEntry == INVALID_VALUE32)
        {
            return;
        }

        const Entry& entry = _entries[bestEntry];
        Job job;
        job._entryIndex = bestEntry;
        job._path = entry._path;
        job._srgb = entry._srgb;
        job._baseMip = entry._candidateBaseMip;
        _jobEntryIndex = bestEntry;

#ifdef VKM_PLATFORM_WASM
        // No worker to hand it to: decode inline and publish the result the same way.
        Result result = runJob(job);
        _results.push_back(std::move(result));
#else
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _jobs.push_back(std::move(job));
        }
        _cv.notify_one();
#endif
    }

    bool VkmTextureStreamer::isSparseEntry(VkmDriverBase* driver, Entry* entry)
    {
        if (!entry->_sparseResolved)
        {
            const VkmTexture* texture =
                driver->getRenderResourcePool()->getResource<VkmTexture>(entry->_texture);
            entry->_sparse = (texture != nullptr) && texture->isSparse();
            entry->_sparseResolved = true;
        }
        return entry->_sparse;
    }

    void VkmTextureStreamer::publishEntry(const Entry& entry,
                                          std::vector<VkmTextureStreamingUpdate>* outUpdates) const
    {
        for (const std::pair<uint32_t, uint32_t>& reference : entry._references)
        {
            VkmTextureStreamingUpdate update;
            update._materialIndex = reference.first;
            update._channel = reference.second;
            update._bindlessSlot = entry._bindlessSlot;
            update._baseMip = entry._residentBaseMip;
            update._totalMipCount = entry._totalMipCount;
            update._minLod = entry._sparse ? entry._residentBaseMip : 0u;
            outUpdates->push_back(update);
        }
    }

    void VkmTextureStreamer::releaseSparseLevels(VkmDriverBase* driver,
                                                 std::vector<VkmTextureStreamingUpdate>* outUpdates)
    {
        for (size_t entryIndex = 0; entryIndex < _entries.size(); ++entryIndex)
        {
            Entry& entry = _entries[entryIndex];
            if (entry._streamingFailed || entry._candidateBaseMip <= entry._residentBaseMip ||
                entry._candidateTickCount < _settings._stableTickCount)
            {
                continue;
            }
            // Never the entry being filled: its target is the one the decode was started for, and
            // pulling levels out from under the fill would leave the two disagreeing.
            if (_build._active && _build._entryIndex == entryIndex)
            {
                continue;
            }
            if (_jobEntryIndex == entryIndex || !isSparseEntry(driver, &entry))
            {
                continue;
            }

            for (uint32_t level = entry._residentBaseMip; level < entry._candidateBaseMip; ++level)
            {
                driver->updateSparseMipResidency(entry._texture, level, /*resident=*/false);
            }
            entry._residentBaseMip = entry._candidateBaseMip;
            ++_rebuildsApplied;
            publishEntry(entry, outUpdates);
        }
    }

    void VkmTextureStreamer::abandonBuild(VkmDriverBase* driver)
    {
        if (_build._sparse)
        {
            // The texture belongs to the entry and keeps serving at the level it already had, so
            // only the levels this fill backed come off -- leaving them mapped would hold tiles
            // for pixels nothing will ever sample.
            for (uint32_t index = 0; index < _build._uploadedLevelCount; ++index)
            {
                driver->updateSparseMipResidency(_build._texture, _build._baseMip + index,
                                                 /*resident=*/false);
            }
        }
        else if (_build._texture != VKM_INVALID_RESOURCE_HANDLE)
        {
            // Never registered, so nothing can be sampling it and the immediate release is safe.
            driver->getRenderResourcePool()->releaseResource(_build._texture);
        }
        _build = PendingBuild{};
    }

    void VkmTextureStreamer::advanceBuild(VkmDriverBase* driver, std::vector<VkmTextureStreamingUpdate>* outUpdates)
    {
        if (!_build._active)
        {
            Result result;
            {
                std::lock_guard<std::mutex> lock(_mutex);
                if (_results.empty())
                {
                    return;
                }
                result = std::move(_results.front());
                _results.pop_front();
            }
            _jobEntryIndex = INVALID_VALUE32;

            if (result._entryIndex >= _entries.size())
            {
                return;
            }
            Entry& entry = _entries[result._entryIndex];
            if (result._levels.empty())
            {
                entry._streamingFailed = true;
                entry._candidateBaseMip = entry._residentBaseMip;
                entry._candidateTickCount = 0;
                return;
            }

            _build._active = true;
            _build._entryIndex = result._entryIndex;
            _build._baseMip = result._baseMip;
            _build._levels = std::move(result._levels);
            _build._uploadedLevelCount = 0;
            _build._sparse = isSparseEntry(driver, &entry);

            if (_build._sparse)
            {
                /*
                * Filling into the texture the entry already has. Only the levels finer than what it
                * holds are new -- everything from _residentBaseMip down is already backed and
                * already correct -- so the decode's coarser half is dropped rather than uploaded
                * over itself.
                */
                _build._texture = entry._texture;
                _build._levels.resize(entry._residentBaseMip - _build._baseMip);
            }
            else
            {
                const glm::uvec2 extent = levelExtent(entry._baseExtent, _build._baseMip);
                VkmTextureInfo info{};
                info._flags = static_cast<VkmResourceCreateInfo>(
                    static_cast<uint32_t>(VkmResourceCreateInfo::AllowShaderRead) |
                    static_cast<uint32_t>(VkmResourceCreateInfo::AllowTransferDst));
                info._extent = glm::uvec3(extent.x, extent.y, 1);
                info._numMipLevels = static_cast<uint32_t>(_build._levels.size());
                info._numArrayLayers = 1;
                info._format = entry._srgb ? VkmFormat::R8G8B8A8_SRGB : VkmFormat::R8G8B8A8_UNORM;
                // The same name the initial upload used, so the browser shows one row per asset that
                // survives a rebuild rather than a new anonymous one each time.
                info._debugName = entry._debugName.c_str();

                VkmTexture* texture = driver->newTexture(info);
                if (texture == nullptr)
                {
                    VKM_DEBUG_WARN(("Streaming texture '" + entry._path +
                                    "' could not be recreated; it stays at the level it already holds").c_str());
                    entry._streamingFailed = true;
                    _build = PendingBuild{};
                    return;
                }
                _build._texture = texture->getHandle();
            }
        }

        // Bounded per tick: on a device whose texture memory is not host-writable each of these
        // submits and blocks, so the whole rebuild would otherwise land in one frame.
        const uint32_t levelCount = static_cast<uint32_t>(_build._levels.size());
        const uint32_t budget = std::max(1u, _settings._maxLevelUploadsPerTick);
        const uint32_t limit = std::min(levelCount, _build._uploadedLevelCount + budget);
        for (; _build._uploadedLevelCount < limit; ++_build._uploadedLevelCount)
        {
            const VkmImageData& level = _build._levels[_build._uploadedLevelCount];
            // A rebuild's texture starts at level 0; a sparse fill writes into the middle of a full
            // chain, and the level must be backed before anything is copied into it.
            const uint32_t targetLevel =
                _build._sparse ? (_build._baseMip + _build._uploadedLevelCount) : _build._uploadedLevelCount;
            if (_build._sparse &&
                !driver->updateSparseMipResidency(_build._texture, targetLevel, /*resident=*/true))
            {
                VKM_DEBUG_WARN(("Streaming texture '" + _entries[_build._entryIndex]._path +
                                "' could not be backed; it stays at the level it already holds").c_str());
                _entries[_build._entryIndex]._streamingFailed = true;
                abandonBuild(driver);
                return;
            }
            if (!driver->uploadToTexture(_build._texture, level._pixels.data(), level.getByteSize(),
                                         targetLevel))
            {
                if (_build._sparse)
                {
                    driver->updateSparseMipResidency(_build._texture, targetLevel, /*resident=*/false);
                }
                VKM_DEBUG_WARN(("Streaming texture '" + _entries[_build._entryIndex]._path +
                                "' could not be uploaded; it stays at the level it already holds").c_str());
                _entries[_build._entryIndex]._streamingFailed = true;
                abandonBuild(driver);
                return;
            }
        }

        if (_build._uploadedLevelCount < levelCount)
        {
            return; // still filling; the old texture keeps serving until every level is in place
        }

        if (_build._sparse)
        {
            // Nothing to hand over: the texture, its view and its slot never changed, so there is
            // no window in which a submitted frame could sample a half-built resource and nothing
            // to retire. Only the level the material reads from moves.
            Entry& sparseEntry = _entries[_build._entryIndex];
            sparseEntry._residentBaseMip = _build._baseMip;
            ++_rebuildsApplied;
            publishEntry(sparseEntry, outUpdates);
            _build = PendingBuild{};
            return;
        }

        // Every level is in place, so the new texture can take over. A fresh slot rather than a
        // rewrite of the old one: the bindless set is UPDATE_AFTER_BIND but not
        // UPDATE_UNUSED_WHILE_PENDING, and a submitted frame is still sampling the old index.
        VkmBindlessResourceManagerBase* bindlessManager = driver->getBindlessResourceManager();
        const uint32_t newSlot = bindlessManager->registerTexture(_build._texture);
        if (newSlot == INVALID_VALUE32)
        {
            VKM_DEBUG_WARN(("Streaming texture '" + _entries[_build._entryIndex]._path +
                            "' could not be registered; the bindless texture array is exhausted").c_str());
            _entries[_build._entryIndex]._streamingFailed = true;
            abandonBuild(driver);
            return;
        }

        Entry& entry = _entries[_build._entryIndex];
        Retired retired;
        retired._texture = entry._texture;
        retired._bindlessSlot = entry._bindlessSlot;
        retired._readyTick = _tickCounter + kRetireTickDelay;
        _retired.push_back(retired);

        entry._texture = _build._texture;
        entry._bindlessSlot = newSlot;
        entry._residentBaseMip = _build._baseMip;
        ++_rebuildsApplied;

        publishEntry(entry, outUpdates);

        _build = PendingBuild{};
    }

    void VkmTextureStreamer::drainRetired(VkmDriverBase* driver, bool force)
    {
        if (_retired.empty())
        {
            return;
        }

        VkmBindlessResourceManagerBase* bindlessManager = driver->getBindlessResourceManager();
        VkmDeferredResourceReclaimer* reclaimer = driver->getDeferredReclaimer();

        size_t writeIndex = 0;
        for (size_t i = 0; i < _retired.size(); ++i)
        {
            const Retired& entry = _retired[i];
            if (!force && _tickCounter < entry._readyTick)
            {
                _retired[writeIndex++] = entry;
                continue;
            }

            if (entry._bindlessSlot != INVALID_VALUE32 && bindlessManager != nullptr)
            {
                bindlessManager->unregisterTexture(entry._bindlessSlot);
            }
            if (entry._texture != VKM_INVALID_RESOURCE_HANDLE)
            {
                reclaimer->requestRelease(entry._texture);
            }
        }
        _retired.resize(writeIndex);
    }

    void VkmTextureStreamer::update(VkmDriverBase* driver, const VkmTextureStreamingView& view,
                                    const std::vector<VkmTextureStreamingObject>& objects,
                                    std::vector<VkmTextureStreamingUpdate>* outUpdates)
    {
        VKM_ASSERT(driver != nullptr, "VkmTextureStreamer::update requires a driver");
        VKM_ASSERT(outUpdates != nullptr, "VkmTextureStreamer::update requires an output vector");

        if (!_settings._enabled || _entries.empty())
        {
            return;
        }

        ++_tickCounter;
        drainRetired(driver, /*force=*/false);
        advanceBuild(driver, outUpdates);
        selectTargets(view, objects);
        releaseSparseLevels(driver, outUpdates);

        // One rebuild in flight at a time: a decode holds a whole chain in memory and an upload
        // blocks, so letting them overlap would multiply both costs.
        if (!_build._active && _jobEntryIndex == INVALID_VALUE32)
        {
            queueJob();
        }
    }

    void VkmTextureStreamer::destroy(VkmDriverBase* driver)
    {
        stopWorker();

        if (driver == nullptr)
        {
            _entries.clear();
            _retired.clear();
            _jobs.clear();
            _results.clear();
            _build = PendingBuild{};
            _jobEntryIndex = INVALID_VALUE32;
            _desiredBaseMips.clear();
            _materialProjectedPixels.clear();
            _materialCount = 0;
            _rebuildsApplied = 0;
            _tickCounter = 0;
            return;
        }

        abandonBuild(driver);
        drainRetired(driver, /*force=*/true);

        VkmBindlessResourceManagerBase* bindlessManager = driver->getBindlessResourceManager();
        VkmDeferredResourceReclaimer* reclaimer = driver->getDeferredReclaimer();
        for (Entry& entry : _entries)
        {
            if (entry._bindlessSlot != INVALID_VALUE32 && bindlessManager != nullptr)
            {
                bindlessManager->unregisterTexture(entry._bindlessSlot);
            }
            if (entry._texture != VKM_INVALID_RESOURCE_HANDLE)
            {
                reclaimer->requestRelease(entry._texture);
            }
        }

        _entries.clear();
        _jobs.clear();
        _results.clear();
        _jobEntryIndex = INVALID_VALUE32;
        _desiredBaseMips.clear();
        _materialProjectedPixels.clear();
        _materialCount = 0;
        _rebuildsApplied = 0;
        _tickCounter = 0;
    }
} // namespace vkm
