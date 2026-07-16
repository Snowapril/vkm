// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/pipeline_state_manager.h>

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/pipeline_state_parser.h>
#include <vkm/renderer/backend/common/shader_cache_util.h>

#include <filesystem>
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
        const std::string& shaderCacheDir, VkmPipelineStateOrigin origin, std::string* outError)
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

        return true;
    }

    bool VkmPipelineStateManager::loadPipelineStatesFromDirectory(const std::string& directory,
        const std::string& shaderCacheDir, VkmPipelineStateOrigin origin, std::string* outError)
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
            if (!loadPipelineState(*desc, shaderCacheDir, origin, &loadError))
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
    }
} // namespace vkm
