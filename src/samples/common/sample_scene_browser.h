// Copyright (c) 2026 Snowapril

#pragma once

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

/*
* Shared between the samples that let a scene be chosen at run time. Header-only and sample-only:
* the vkm library must not reference src/samples, but one sample sharing with another is what this
* directory is for. Only the two file-system helpers live here -- each sample draws its own list,
* because what Add and Remove do to a loaded scene is the part that differs.
*/
namespace vkmsample
{
    // One loadable file found under resources/Scenes/.
    struct SceneEntry
    {
        std::string _displayName; // path relative to the scenes directory
        std::string _path;
    };

    /*
    * @brief Splits a comma-separated cvar into individual paths, dropping empty entries.
    * @details A global variable is a single string, so this is how one names several scenes. A
    * value with no comma yields exactly one path, which is the single-scene case unchanged.
    * @param value The cvar's value.
    * @return One path per non-empty comma-separated field, in order.
    */
    inline std::vector<std::string> splitScenePaths(const std::string& value)
    {
        std::vector<std::string> paths;
        size_t begin = 0;
        while (begin <= value.size())
        {
            const size_t comma = value.find(',', begin);
            const size_t end = (comma == std::string::npos) ? value.size() : comma;
            if (end > begin)
            {
                paths.push_back(value.substr(begin, end - begin));
            }
            if (comma == std::string::npos)
            {
                break;
            }
            begin = comma + 1;
        }
        return paths;
    }

    /*
    * @brief Walks a scenes directory for loadable glTF files.
    * @details Recursive, because scenes are one directory per asset -- that is how
    * scripts/download_scenes.py lays them out. A missing directory yields no entries rather than
    * an error: a checkout that has not run the download script has none.
    * @param scenesDirectory Directory to walk, normally RESOURCES_DIR "Scenes".
    * @return Every .gltf and .glb below it, sorted by the path relative to it.
    */
    inline std::vector<SceneEntry> scanSceneDirectory(const std::filesystem::path& scenesDirectory)
    {
        std::vector<SceneEntry> entries;
        std::error_code ec;
        if (!std::filesystem::is_directory(scenesDirectory, ec))
        {
            return entries;
        }

        for (const std::filesystem::directory_entry& entry :
             std::filesystem::recursive_directory_iterator(scenesDirectory, ec))
        {
            if (!entry.is_regular_file())
            {
                continue;
            }
            const std::filesystem::path extension = entry.path().extension();
            if (extension != ".gltf" && extension != ".glb")
            {
                continue;
            }
            entries.push_back(SceneEntry{
                std::filesystem::relative(entry.path(), scenesDirectory, ec).generic_string(),
                entry.path().string(),
            });
        }

        std::sort(entries.begin(), entries.end(), [](const SceneEntry& lhs, const SceneEntry& rhs) {
            return lhs._displayName < rhs._displayName;
        });
        return entries;
    }
} // namespace vkmsample
