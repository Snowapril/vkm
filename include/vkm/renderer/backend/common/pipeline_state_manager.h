// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/pipeline_state.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vkm
{
    class VkmDriverBase;
    class VkmPipelineStateBase;

    enum class VkmPipelineStateOrigin : uint8_t
    {
        Engine = 0, // loaded from resources/Pipelines/Engine/*.json
        User = 1,   // loaded from a caller-supplied directory (e.g. a sample's own source dir)
    };

    /*
    * @brief One PSO json file and everything needed to reload it. A single json expands into
    * N named variants (see expandPipelineStateOptions), so reload is per source file rather
    * than per variant -- the option set itself is part of what an edit can change.
    */
    struct VkmPipelineStateSource
    {
        std::string jsonPath; // empty for descriptors registered directly via loadPipelineState()
        std::string shaderCacheDir;
        VkmPipelineStateOrigin origin = VkmPipelineStateOrigin::User;
        std::vector<std::string> variantNames; // expanded, e.g. "triangle_pso[wireframe]"
        // json + every distinct shader source it references + the shared *.hlsli headers,
        // with the modification time seen at the last load/reload.
        std::vector<std::pair<std::string, std::filesystem::file_time_type>> watchedFiles;
        bool stale = false; // a watched file changed since the last load/reload
    };

    // Owns every live VkmPipelineStateBase created for this driver, categorized as
    // Engine-owned (shared, loaded from resources/Pipelines/Engine/) or User-owned
    // (loaded from a caller-supplied directory, e.g. a sample's own source dir).
    class VkmPipelineStateManager
    {
    public:
        explicit VkmPipelineStateManager(VkmDriverBase* driver);
        ~VkmPipelineStateManager();

        // Non-recursively scans `directory` for *.json, parses each with
        // parsePipelineStateFromFile, expands options via expandPipelineStateOptions, loads
        // each variant's .vfcache files from `shaderCacheDir`, creates the backend pipeline
        // object via _driver->newPipelineState(), and registers it under `origin`. Aborts on
        // the first file that fails to parse/expand/load, reporting which file via
        // *outError (fail-fast, consistent with the parser's own fail-fast style). If
        // `directory` doesn't exist, logs and returns true (a no-op).
        bool loadPipelineStatesFromDirectory(const std::string& directory, const std::string& shaderCacheDir,
            VkmPipelineStateOrigin origin, std::string* outError = nullptr);

        // Registers one already-parsed descriptor directly (mainly for unit tests). `jsonPath`
        // is the file it was parsed from when there is one; leaving it empty registers a
        // source that reloadSource() will refuse (there is nothing to re-read).
        bool loadPipelineState(const VkmPipelineStateDescriptor& desc, const std::string& shaderCacheDir,
            VkmPipelineStateOrigin origin, std::string* outError = nullptr, const std::string& jsonPath = std::string());

        // Looks up by name (must include the "[option]" suffix when applicable, exactly as
        // expandPipelineStateOptions() produced it). The single-arg overload checks Engine
        // first, then User; a name can never collide across origins since loadPipelineState()
        // rejects loading a name that already exists in the other origin's map.
        VkmPipelineStateBase* getPipelineState(const std::string& name) const;
        VkmPipelineStateBase* getPipelineState(const std::string& name, VkmPipelineStateOrigin origin) const;

        void destroyAll();

        // Every loaded PSO json, in load order. Indices are stable for the manager's lifetime.
        const std::vector<VkmPipelineStateSource>& getSources() const { return _sources; }
        // Index of the source that produced `name`, or npos if it came from a direct
        // loadPipelineState() registration.
        size_t findSourceIndexOfPipelineState(const std::string& name) const;

        /*
        * @brief Re-read one PSO json and rebuild its pipelines in place, so cached
        * VkmPipelineStateBase* stay valid (see VkmPipelineStateBase::reload).
        *
        * @param recompileShaders when true, run vkm-compiler over the json first so shader
        * edits are picked up; requires the build to have baked in a compiler path
        * (isShaderRecompilationAvailable()).
        *
        * Nothing is destroyed until every fallible step (recompile, parse, expand) has
        * passed, so a bad edit leaves the previously loaded pipelines untouched and running.
        * Variants that disappeared from the edited json stay registered -- destroying them
        * would dangle pointers callers still hold -- and are reported through *outError.
        */
        bool reloadSource(size_t sourceIndex, bool recompileShaders, std::string* outError = nullptr);
        // Reloads every source with a json path; reports the first failure but still attempts
        // the rest, so one broken file does not block the others.
        bool reloadAllSources(bool recompileShaders, std::string* outError = nullptr);

        // Re-stats every source's watched files and updates their `stale` flag. Returns true
        // when any flag changed.
        bool refreshStaleness();

        /*
        * @brief Throttled staleness poll, driven from VkmEngine::update() at the same cadence
        * as the memory inspector's sampling. A few dozen last_write_time() calls twice a
        * second is free, and a watcher thread would have to synchronize with a reload path
        * that calls waitIdle() and destroys GPU objects.
        *
        * Reloads sources that just went stale when auto-reload is enabled.
        */
        void pollSourceChanges(double deltaTime);

        // When set, pollSourceChanges() reloads a source as soon as one of its watched files
        // changes, instead of only flagging it. Off by default -- an editor that saves a
        // half-written file would otherwise reload it.
        void setAutoReloadOnChange(bool enabled) { _autoReloadOnChange = enabled; }
        bool isAutoReloadOnChange() const { return _autoReloadOnChange; }

        // Message from the most recent reload (compiler stderr, parse error, or a kept-alive
        // variant warning). Empty when the last reload succeeded cleanly.
        const std::string& getLastReloadError() const { return _lastReloadError; }

        // True when this build baked in a vkm-compiler path (dev builds; see
        // VKM_COMPILER_EXECUTABLE in CMakeLists.txt). False in Emscripten/installed builds,
        // where reloadSource() can still re-apply json changes but not recompile shaders.
        static bool isShaderRecompilationAvailable();

    private:
        bool reloadSourceInner(size_t sourceIndex, bool recompileShaders, std::string* outError);
        // Runs vkm-compiler over `source`'s json with exactly the arguments the build-time
        // ShaderCompile.cmake uses, so the produced .vfcache files are identical.
        bool recompileShaders(const VkmPipelineStateSource& source, std::string* outError) const;
        // Rebuilds `source.watchedFiles` (json + shader sources + shared *.hlsli) and stamps
        // each with its current modification time, clearing `stale`.
        void refreshWatchedFiles(VkmPipelineStateSource& source,
            const std::vector<VkmPipelineStateDescriptor>& variants) const;

    private:
        VkmDriverBase* _driver;
        std::unordered_map<std::string, std::unique_ptr<VkmPipelineStateBase>> _enginePipelineStates;
        std::unordered_map<std::string, std::unique_ptr<VkmPipelineStateBase>> _userPipelineStates;
        std::vector<VkmPipelineStateSource> _sources;
        double _secondsSinceStalenessPoll = 0.0;
        bool _autoReloadOnChange = false;
        std::string _lastReloadError;
    };
} // namespace vkm
