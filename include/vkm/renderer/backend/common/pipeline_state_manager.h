// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/pipeline_state.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

namespace vkm
{
    class VkmDriverBase;
    class VkmPipelineStateBase;

    enum class VkmPipelineStateOrigin : uint8_t
    {
        Engine = 0, // loaded from resources/Pipelines/Engine/*.json
        User = 1,   // loaded from a caller-supplied directory (e.g. a sample's own source dir)
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

        // Registers one already-parsed descriptor directly (mainly for unit tests).
        bool loadPipelineState(const VkmPipelineStateDescriptor& desc, const std::string& shaderCacheDir,
            VkmPipelineStateOrigin origin, std::string* outError = nullptr);

        // Looks up by name (must include the "[option]" suffix when applicable, exactly as
        // expandPipelineStateOptions() produced it). The single-arg overload checks Engine
        // first, then User; a name can never collide across origins since loadPipelineState()
        // rejects loading a name that already exists in the other origin's map.
        VkmPipelineStateBase* getPipelineState(const std::string& name) const;
        VkmPipelineStateBase* getPipelineState(const std::string& name, VkmPipelineStateOrigin origin) const;

        void destroyAll();

    private:
        VkmDriverBase* _driver;
        std::unordered_map<std::string, std::unique_ptr<VkmPipelineStateBase>> _enginePipelineStates;
        std::unordered_map<std::string, std::unique_ptr<VkmPipelineStateBase>> _userPipelineStates;
    };
} // namespace vkm
