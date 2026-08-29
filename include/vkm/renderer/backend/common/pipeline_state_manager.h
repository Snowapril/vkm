// Copyright (c) 2026 Snowapril

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
    * @brief One PSO json file and everything needed to reload it.
    * @details A single json expands into N named variants (see expandPipelineStateOptions), so
    * reload is per source file rather than per variant -- the option set is itself part of what an
    * edit can change.
    */
    struct VkmPipelineStateSource
    {
        std::string jsonPath; // empty for descriptors registered directly via loadPipelineState()
        std::string shaderCacheDir;
        // Directory a shader stage's relative filepath resolves against. Empty means the json's own
        // directory, which is vkm-compiler's default and what the samples rely on; the engine and
        // ray-tracing directories keep their json and their HLSL apart and so name it explicitly.
        std::string shaderRoot;
        VkmPipelineStateOrigin origin = VkmPipelineStateOrigin::User;
        std::vector<std::string> variantNames; // expanded, e.g. "triangle_pso[wireframe]"
        // json + every distinct shader source it references + the shared *.hlsli headers,
        // with the modification time seen at the last load/reload.
        std::vector<std::pair<std::string, std::filesystem::file_time_type>> watchedFiles;
        bool stale = false; // a watched file changed since the last load/reload
    };

    /*
    * @brief Owns every live VkmPipelineStateBase created for this driver.
    * @details Categorized as Engine-owned, loaded from resources/Pipelines/Engine/, or User-owned,
    * loaded from a caller-supplied directory such as a sample's own source dir.
    */
    class VkmPipelineStateManager
    {
    public:
        explicit VkmPipelineStateManager(VkmDriverBase* driver);
        ~VkmPipelineStateManager();

        /*
        * @brief Loads every PSO json in a directory, non-recursively.
        * @details Parses each with parsePipelineStateFromFile, expands options via
        * expandPipelineStateOptions, loads each variant's .vfcache files, creates the backend
        * pipeline object via _driver->newPipelineState(), and registers it. Fail-fast: aborts on
        * the first file that fails to parse, expand or load.
        * @param directory Directory to scan. A missing directory logs and succeeds as a no-op.
        * @param shaderCacheDir Directory holding the variants' .vfcache files.
        * @param origin Which map to register the results under.
        * @param outError Receives the failing file and reason. May be null.
        * @param shaderRoot Directory a stage's relative filepath resolves against. Empty resolves
        * against each json's own directory. Must match the SHADER_ROOT the build compiled these
        * shaders with, or a runtime recompile looks for them somewhere they are not.
        * @return False on the first file that could not be loaded.
        */
        bool loadPipelineStatesFromDirectory(const std::string& directory, const std::string& shaderCacheDir,
            VkmPipelineStateOrigin origin, std::string* outError = nullptr,
            const std::string& shaderRoot = std::string());

        /*
        * @brief Registers one already-parsed descriptor directly. Mainly for unit tests.
        * @param desc Descriptor to register.
        * @param shaderCacheDir Directory holding its .vfcache files.
        * @param origin Which map to register it under.
        * @param outError Receives the failure reason. May be null.
        * @param jsonPath File it was parsed from. Leaving it empty registers a source that
        * reloadSource() refuses, there being nothing to re-read.
        * @param shaderRoot Directory a stage's relative filepath resolves against. Empty resolves
        * against the json's own directory.
        * @return False if the pipeline could not be created.
        */
        bool loadPipelineState(const VkmPipelineStateDescriptor& desc, const std::string& shaderCacheDir,
            VkmPipelineStateOrigin origin, std::string* outError = nullptr, const std::string& jsonPath = std::string(),
            const std::string& shaderRoot = std::string());

        /*
        * @brief Looks up a loaded pipeline by name.
        * @details The single-argument overload checks Engine first, then User. A name cannot
        * collide across origins, loadPipelineState() rejecting a name already in the other map.
        * @param name Pipeline name, including the "[option]" suffix exactly as
        * expandPipelineStateOptions() produced it.
        * @return The pipeline, or nullptr when no such name is registered.
        */
        VkmPipelineStateBase* getPipelineState(const std::string& name) const;
        VkmPipelineStateBase* getPipelineState(const std::string& name, VkmPipelineStateOrigin origin) const;

        void destroyAll();

        // Every loaded PSO json, in load order. Indices are stable for the manager's lifetime.
        const std::vector<VkmPipelineStateSource>& getSources() const { return _sources; }
        // Index of the source that produced `name`, or npos if it came from a direct
        // loadPipelineState() registration.
        size_t findSourceIndexOfPipelineState(const std::string& name) const;

        /*
        * @brief Re-reads one PSO json and rebuilds its pipelines in place, so cached
        * VkmPipelineStateBase* stay valid (see VkmPipelineStateBase::reload).
        * @details Nothing is destroyed until every fallible step -- recompile, parse, expand -- has
        * passed, so a bad edit leaves the loaded pipelines untouched and running. Variants that
        * disappeared from the edited json stay registered, destroying them would dangle pointers
        * callers still hold.
        * @param sourceIndex Index into getSources().
        * @param recompileShaders Run vkm-compiler over the json first, so shader edits are picked
        * up. Requires isShaderRecompilationAvailable().
        * @param outError Receives the failure reason, or a kept-alive variant warning. May be null.
        * @return False if the source could not be reloaded.
        */
        bool reloadSource(size_t sourceIndex, bool recompileShaders, std::string* outError = nullptr);

        /*
        * @brief Reloads every source with a json path.
        * @param recompileShaders Run vkm-compiler over each json first.
        * @param outError Receives the first failure. May be null.
        * @return False if any source failed. The rest are still attempted, so one broken file does
        * not block the others.
        */
        bool reloadAllSources(bool recompileShaders, std::string* outError = nullptr);

        /*
        * @brief Re-stats every source's watched files and updates their `stale` flag.
        * @return True when any flag changed.
        */
        bool refreshStaleness();

        /*
        * @brief Throttled staleness poll, driven from VkmEngine::update().
        * @details Runs at the same cadence as the memory inspector's sampling. A watcher thread
        * would have to synchronize with a reload path that calls waitIdle() and destroys GPU
        * objects. Reloads sources that just went stale when auto-reload is enabled.
        * @param deltaTime Seconds since the last call.
        */
        void pollSourceChanges(double deltaTime);

        /*
        * @brief Whether pollSourceChanges() reloads a stale source or only flags it.
        * @details Off by default: an editor that saves a half-written file would otherwise trigger
        * a reload of it.
        * @param enabled True to reload as soon as a watched file changes.
        */
        void setAutoReloadOnChange(bool enabled) { _autoReloadOnChange = enabled; }
        bool isAutoReloadOnChange() const { return _autoReloadOnChange; }

        // Message from the most recent reload (compiler stderr, parse error, or a kept-alive
        // variant warning). Empty when the last reload succeeded cleanly.
        const std::string& getLastReloadError() const { return _lastReloadError; }

        /*
        * @brief Whether this build baked in a vkm-compiler path (VKM_COMPILER_EXECUTABLE).
        * @return False in Emscripten and installed builds, where reloadSource() can still re-apply
        * json changes but not recompile shaders.
        */
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
