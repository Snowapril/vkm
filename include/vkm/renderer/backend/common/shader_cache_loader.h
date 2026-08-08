// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/shader_cache.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vkm
{
    /*
    * @brief Backend-agnostic result of loading one .vfcache file.
    * @details Turning `content` into a VkShaderModule, MTLLibrary+MTLFunction or WGPUShaderModule
    * is each backend's pipeline-creation job; this loader only validates the header and hands back
    * raw bytes or text.
    */
    struct VkmLoadedShaderCache
    {
        VkmShaderCacheStage stage;
        VkmShaderCacheContentFormat contentFormat;
        std::string entryPoint;
        // The compute stage's declared [numthreads(x, y, z)]; {0, 0, 0} for other stages.
        // Only Metal consumes it (see VkmShaderCacheHeader::threadGroupSize).
        uint32_t threadGroupSize[3] = {};
        std::vector<uint8_t> content;  // SPIR-V words as bytes, or UTF-8 MSL/WGSL text
    };

    /*
    * @brief Reads and validates one .vfcache file's header and returns its content.
    * @param filepath File to read.
    * @param expectedBackend Backend the file's header must name.
    * @param outError Receives the failure reason. May be null.
    * @return The loaded cache, or nullopt on file open failure, truncated header, bad magic,
    * version mismatch, backend mismatch, or content shorter than `contentSize` states.
    */
    std::optional<VkmLoadedShaderCache> loadShaderCacheFile(const std::string& filepath,
        VkmShaderCacheBackend expectedBackend, std::string* outError = nullptr);
} // namespace vkm
