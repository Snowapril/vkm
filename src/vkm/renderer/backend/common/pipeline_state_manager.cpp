// Copyright (c) 2026 Snowapril

#include <vkm/renderer/backend/common/pipeline_state_manager.h>

#include <vkm/base/common.h>
#include <vkm/base/subprocess.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/pipeline_state_parser.h>
#include <vkm/renderer/backend/common/shader_cache_util.h>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <unordered_set>

namespace vkm
{
    VkmPipelineStateManager::VkmPipelineStateManager(VkmDriverBase* driver)
        : _driver(driver)
    {
    }

    VkmPipelineStateManager::~VkmPipelineStateManager()
    {
        destroyAll();
    }

    bool VkmPipelineStateManager::loadPipelineState(const VkmPipelineStateDescriptor& desc,
        const std::string& shaderCacheDir, VkmPipelineStateOrigin origin, std::string* outError,
        const std::string& jsonPath, const std::string& shaderRoot)
    {
        std::optional<std::vector<VkmPipelineStateDescriptor>> variants =
            expandPipelineStateOptions(desc, vkmActiveShaderCacheBackend(), outError);
        if (!variants.has_value())
        {
            return false;
        }

        std::unordered_map<std::string, std::unique_ptr<VkmPipelineStateBase>>& target =
            (origin == VkmPipelineStateOrigin::Engine) ? _enginePipelineStates : _userPipelineStates;
        const std::unordered_map<std::string, std::unique_ptr<VkmPipelineStateBase>>& other =
            (origin == VkmPipelineStateOrigin::Engine) ? _userPipelineStates : _enginePipelineStates;

        // Validate every expanded variant's name before creating or inserting anything, so a
        // later variant's collision can't leave earlier variants already registered in
        // `target` (a partial load). Checks both against the other origin's map and against
        // duplicate names within this same batch of variants.
        std::unordered_set<std::string> seenNames;
        for (const VkmPipelineStateDescriptor& variant : *variants)
        {
            std::string message;
            if (other.find(variant.name) != other.end())
            {
                message = "Pipeline state '" + variant.name + "' already exists under the other origin (Engine/User collision)";
            }
            else if (!seenNames.insert(variant.name).second)
            {
                message = "Pipeline state '" + variant.name + "' is defined more than once in this descriptor's expanded variants";
            }
            else
            {
                continue;
            }

            if (outError != nullptr)
            {
                *outError = message;
            }
            VKM_DEBUG_ERROR(message.c_str());
            return false;
        }

        for (const VkmPipelineStateDescriptor& variant : *variants)
        {
            VkmPipelineStateBase* pipelineState = _driver->newPipelineState(variant, shaderCacheDir, outError);
            if (pipelineState == nullptr)
            {
                const std::string message = "Failed to create pipeline state '" + variant.name + "'" +
                    (outError != nullptr && !outError->empty() ? ": " + *outError : "");
                if (outError != nullptr)
                {
                    *outError = message;
                }
                VKM_DEBUG_ERROR(message.c_str());
                return false;
            }
            target[variant.name] = std::unique_ptr<VkmPipelineStateBase>(pipelineState);
        }

        VkmPipelineStateSource source;
        source.jsonPath = jsonPath;
        source.shaderCacheDir = shaderCacheDir;
        source.shaderRoot = shaderRoot;
        source.origin = origin;
        for (const VkmPipelineStateDescriptor& variant : *variants)
        {
            source.variantNames.push_back(variant.name);
        }
        refreshWatchedFiles(source, *variants);
        _sources.push_back(std::move(source));

        return true;
    }

    bool VkmPipelineStateManager::loadPipelineStatesFromDirectory(const std::string& directory,
        const std::string& shaderCacheDir, VkmPipelineStateOrigin origin, std::string* outError,
        const std::string& shaderRoot)
    {
        std::error_code ec;
        if (!std::filesystem::exists(directory, ec) || !std::filesystem::is_directory(directory, ec))
        {
            VKM_DEBUG_INFO(("Pipeline state directory '" + directory + "' does not exist, skipping").c_str());
            return true;
        }

        for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(directory))
        {
            if (!entry.is_regular_file() || entry.path().extension() != ".json")
            {
                continue;
            }

            const std::string filepath = entry.path().string();
            std::string parseError;
            std::optional<VkmPipelineStateDescriptor> desc = parsePipelineStateFromFile(filepath, &parseError);
            if (!desc.has_value())
            {
                const std::string message = "Failed to load pipeline state file '" + filepath + "': " + parseError;
                if (outError != nullptr)
                {
                    *outError = message;
                }
                VKM_DEBUG_ERROR(message.c_str());
                return false;
            }

            std::string loadError;
            if (!loadPipelineState(*desc, shaderCacheDir, origin, &loadError, filepath, shaderRoot))
            {
                const std::string message = "Failed to load pipeline state file '" + filepath + "': " + loadError;
                if (outError != nullptr)
                {
                    *outError = message;
                }
                VKM_DEBUG_ERROR(message.c_str());
                return false;
            }
        }

        return true;
    }

    VkmPipelineStateBase* VkmPipelineStateManager::getPipelineState(const std::string& name) const
    {
        VkmPipelineStateBase* engineResult = getPipelineState(name, VkmPipelineStateOrigin::Engine);
        if (engineResult != nullptr)
        {
            return engineResult;
        }
        return getPipelineState(name, VkmPipelineStateOrigin::User);
    }

    VkmPipelineStateBase* VkmPipelineStateManager::getPipelineState(const std::string& name, VkmPipelineStateOrigin origin) const
    {
        const std::unordered_map<std::string, std::unique_ptr<VkmPipelineStateBase>>& source =
            (origin == VkmPipelineStateOrigin::Engine) ? _enginePipelineStates : _userPipelineStates;

        const auto it = source.find(name);
        if (it == source.end())
        {
            return nullptr;
        }
        return it->second.get();
    }

    void VkmPipelineStateManager::destroyAll()
    {
        for (auto& [name, pipelineState] : _enginePipelineStates)
        {
            pipelineState->destroy();
        }
        for (auto& [name, pipelineState] : _userPipelineStates)
        {
            pipelineState->destroy();
        }
        _enginePipelineStates.clear();
        _userPipelineStates.clear();
        _sources.clear();
    }

    size_t VkmPipelineStateManager::findSourceIndexOfPipelineState(const std::string& name) const
    {
        for (size_t i = 0; i < _sources.size(); ++i)
        {
            const std::vector<std::string>& names = _sources[i].variantNames;
            if (std::find(names.begin(), names.end(), name) != names.end())
            {
                return i;
            }
        }
        return std::string::npos;
    }

    bool VkmPipelineStateManager::isShaderRecompilationAvailable()
    {
#if defined(VKM_COMPILER_EXECUTABLE)
        std::error_code ec;
        return std::filesystem::exists(VKM_COMPILER_EXECUTABLE, ec);
#else
        return false;
#endif
    }

    bool VkmPipelineStateManager::recompileShaders(const VkmPipelineStateSource& source, std::string* outError) const
    {
#if defined(VKM_COMPILER_EXECUTABLE)
        // Exactly the arguments ShaderCompile.cmake passes, so the .vfcache files this
        // produces are the ones the build would have produced -- including --shader-root, which
        // the build passes for any directory whose json and HLSL live apart. Omitting it here
        // leaves vkm-compiler defaulting to the json's own directory, where an engine shader is
        // not.
        std::vector<std::string> args = {
            "--pso", source.jsonPath,
            "--output-dir", source.shaderCacheDir,
            "--backend", vkmShaderCacheBackendName(vkmActiveShaderCacheBackend()),
            "--include-dir", VKM_SHADER_INCLUDE_DIR,
        };
        if (!source.shaderRoot.empty())
        {
            args.push_back("--shader-root");
            args.push_back(source.shaderRoot);
        }
#if defined(VKM_COMPILER_EMIT_MSL)
        args.push_back("--emit-msl");
#endif

        const SubprocessResult result = runSubprocess(VKM_COMPILER_EXECUTABLE, args);
        if (result.exitCode != 0)
        {
            if (outError != nullptr)
            {
                *outError = "vkm-compiler failed (exit " + std::to_string(result.exitCode) + "):\n" + result.output;
            }
            return false;
        }
        return true;
#else
        (void)source;
        if (outError != nullptr)
        {
            *outError = "Shader recompilation is unavailable: this build has no vkm-compiler path baked in";
        }
        return false;
#endif
    }

    void VkmPipelineStateManager::refreshWatchedFiles(VkmPipelineStateSource& source,
        const std::vector<VkmPipelineStateDescriptor>& variants) const
    {
        source.watchedFiles.clear();
        source.stale = false;
        if (source.jsonPath.empty())
        {
            return;
        }

        std::vector<std::string> paths;
        paths.push_back(source.jsonPath);

        // Shader filepaths resolve against the source's own shader root, falling back to the
        // json's own directory -- vkm-compiler's --shader-root default. Resolving against the
        // wrong one yields a path last_write_time() fails on, and the shader silently never
        // enters watchedFiles, so nothing ever notices an edit to it.
        const std::filesystem::path shaderRoot =
            source.shaderRoot.empty() ? std::filesystem::path(source.jsonPath).parent_path()
                                      : std::filesystem::path(source.shaderRoot);
        for (const VkmPipelineStateDescriptor& variant : variants)
        {
            for (const std::optional<VkmShaderStageDescriptor>* stage :
                 {&variant.vertexShader, &variant.fragmentShader, &variant.computeShader})
            {
                if (stage->has_value())
                {
                    paths.push_back((shaderRoot / (*stage)->filepath).string());
                }
            }
        }

#if defined(VKM_SHADER_INCLUDE_DIR)
        // The shared *.hlsli headers, for the same reason ShaderCompile.cmake globs them into
        // every command's DEPENDS: dxc emits no depfile, so nothing else would notice an edit.
        std::error_code globError;
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(VKM_SHADER_INCLUDE_DIR, globError))
        {
            if (entry.is_regular_file() && entry.path().extension() == ".hlsli")
            {
                paths.push_back(entry.path().string());
            }
        }
#endif

        std::sort(paths.begin(), paths.end());
        paths.erase(std::unique(paths.begin(), paths.end()), paths.end());

        for (const std::string& path : paths)
        {
            std::error_code ec;
            const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(path, ec);
            if (!ec)
            {
                source.watchedFiles.emplace_back(path, writeTime);
            }
        }
    }

    bool VkmPipelineStateManager::refreshStaleness()
    {
        bool changed = false;
        for (VkmPipelineStateSource& source : _sources)
        {
            bool stale = false;
            for (const auto& [path, knownWriteTime] : source.watchedFiles)
            {
                std::error_code ec;
                const std::filesystem::file_time_type writeTime = std::filesystem::last_write_time(path, ec);
                // An error here means the file is momentarily unreadable (a save in progress,
                // typically) -- leave the previous verdict and re-check on the next poll.
                if (!ec && writeTime != knownWriteTime)
                {
                    stale = true;
                    break;
                }
            }
            if (stale != source.stale)
            {
                source.stale = stale;
                changed = true;
            }
        }
        return changed;
    }

    void VkmPipelineStateManager::pollSourceChanges(double deltaTime)
    {
        constexpr double kPollIntervalSeconds = 0.5;
        _secondsSinceStalenessPoll += deltaTime;
        if (_secondsSinceStalenessPoll < kPollIntervalSeconds)
        {
            return;
        }
        _secondsSinceStalenessPoll = 0.0;

        if (!refreshStaleness() || !_autoReloadOnChange)
        {
            return;
        }

        const bool recompile = isShaderRecompilationAvailable();
        for (size_t i = 0; i < _sources.size(); ++i)
        {
            if (_sources[i].stale && !_sources[i].jsonPath.empty())
            {
                reloadSource(i, recompile, nullptr);
            }
        }
    }

    bool VkmPipelineStateManager::reloadSource(size_t sourceIndex, bool recompile, std::string* outError)
    {
        // One funnel so every exit path -- including an auto-reload nobody passed an outError
        // to -- leaves the message where the debug UI can show it.
        std::string error;
        const bool succeeded = reloadSourceInner(sourceIndex, recompile, &error);
        _lastReloadError = error;
        if (outError != nullptr)
        {
            *outError = error;
        }
        return succeeded;
    }

    bool VkmPipelineStateManager::reloadSourceInner(size_t sourceIndex, bool recompile, std::string* outError)
    {
        if (sourceIndex >= _sources.size())
        {
            if (outError != nullptr)
            {
                *outError = "Invalid pipeline state source index";
            }
            return false;
        }

        VkmPipelineStateSource& source = _sources[sourceIndex];
        if (source.jsonPath.empty())
        {
            if (outError != nullptr)
            {
                *outError = "Pipeline state was registered without a json file and cannot be reloaded";
            }
            return false;
        }

        // Everything that can fail runs before anything is destroyed, so a bad edit leaves
        // the currently loaded pipelines untouched and still rendering.
        if (recompile && !recompileShaders(source, outError))
        {
            return false;
        }

        std::string parseError;
        std::optional<VkmPipelineStateDescriptor> desc = parsePipelineStateFromFile(source.jsonPath, &parseError);
        if (!desc.has_value())
        {
            if (outError != nullptr)
            {
                *outError = "Failed to parse '" + source.jsonPath + "': " + parseError;
            }
            return false;
        }

        std::optional<std::vector<VkmPipelineStateDescriptor>> variants =
            expandPipelineStateOptions(*desc, vkmActiveShaderCacheBackend(), &parseError);
        if (!variants.has_value())
        {
            if (outError != nullptr)
            {
                *outError = "Failed to expand options of '" + source.jsonPath + "': " + parseError;
            }
            return false;
        }

        std::unordered_map<std::string, std::unique_ptr<VkmPipelineStateBase>>& target =
            (source.origin == VkmPipelineStateOrigin::Engine) ? _enginePipelineStates : _userPipelineStates;

        // Backend pipeline objects are destroyed synchronously by destroyInner(); they never
        // go through the deferred reclaimer, so all in-flight work must be done first.
        _driver->waitIdle();

        std::string firstError;
        std::vector<std::string> newVariantNames;
        for (VkmPipelineStateDescriptor& variant : *variants)
        {
            std::string variantError;
            if (!_driver->resolveSwapChainFormats(variant, &variantError))
            {
                if (firstError.empty())
                {
                    firstError = "'" + variant.name + "': " + variantError;
                }
                continue;
            }

            const auto it = target.find(variant.name);
            if (it != target.end())
            {
                if (!it->second->reload(variant, source.shaderCacheDir, &variantError) && firstError.empty())
                {
                    firstError = "'" + variant.name + "': " + variantError;
                }
            }
            else
            {
                VkmPipelineStateBase* pipelineState = _driver->newPipelineState(variant, source.shaderCacheDir, &variantError);
                if (pipelineState == nullptr)
                {
                    if (firstError.empty())
                    {
                        firstError = "'" + variant.name + "': " + variantError;
                    }
                    continue;
                }
                target[variant.name] = std::unique_ptr<VkmPipelineStateBase>(pipelineState);
            }
            newVariantNames.push_back(variant.name);
        }

        // Variants the edit removed keep their previously created pipeline: callers hold raw
        // non-owning pointers to them with no invalidation hook, so destroying one here would
        // dangle. Report it instead and leave the object registered.
        std::string droppedVariants;
        for (const std::string& previousName : source.variantNames)
        {
            if (std::find(newVariantNames.begin(), newVariantNames.end(), previousName) == newVariantNames.end())
            {
                droppedVariants += (droppedVariants.empty() ? "" : ", ") + previousName;
                newVariantNames.push_back(previousName);
            }
        }

        source.variantNames = std::move(newVariantNames);
        refreshWatchedFiles(source, *variants);

        // A dropped variant is a warning, not a failure: the reload itself succeeded, the
        // caller just needs to know one of its pipelines is now older than the json.
        if (!droppedVariants.empty())
        {
            const std::string message = "Variant(s) no longer present in the json are kept loaded: " + droppedVariants;
            VKM_DEBUG_INFO(message.c_str());
            if (outError != nullptr && firstError.empty())
            {
                *outError = message;
            }
        }

        if (!firstError.empty())
        {
            if (outError != nullptr)
            {
                *outError = firstError;
            }
            return false;
        }
        return true;
    }

    bool VkmPipelineStateManager::reloadAllSources(bool recompile, std::string* outError)
    {
        bool allSucceeded = true;
        std::string firstError;
        for (size_t i = 0; i < _sources.size(); ++i)
        {
            if (_sources[i].jsonPath.empty())
            {
                continue;
            }
            // One broken file must not stop the others from reloading.
            std::string sourceError;
            if (!reloadSource(i, recompile, &sourceError))
            {
                allSucceeded = false;
                if (firstError.empty())
                {
                    firstError = sourceError;
                }
            }
        }

        _lastReloadError = firstError;
        if (outError != nullptr)
        {
            *outError = firstError;
        }
        return allSucceeded;
    }
} // namespace vkm
