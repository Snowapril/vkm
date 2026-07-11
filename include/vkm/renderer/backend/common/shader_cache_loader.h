// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/shader_cache.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace vkm
{
    // Backend-agnostic result of loading one .vfcache file. Turning `content` into an
    // actual VkShaderModule / MTLLibrary+MTLFunction / WGPUShaderModule is each
    // backend's pipeline-creation job -- this loader only validates the header and
    // hands back raw bytes/text.
    struct VkmLoadedShaderCache
    {
        VkmShaderCacheStage stage;
        VkmShaderCacheContentFormat contentFormat;
        std::string entryPoint;
        std::vector<uint8_t> content; // SPIR-V words (as bytes) or UTF-8 MSL/WGSL text
    };

    // Reads and validates one .vfcache file's header (magic, version, `expectedBackend`
    // match) and returns its content. Returns std::nullopt (+ *outError, if non-null)
    // on: file open failure, truncated header, bad magic, version mismatch, backend
    // mismatch, or truncated content (fewer bytes available than `contentSize` states).
    std::optional<VkmLoadedShaderCache> loadShaderCacheFile(const std::string& filepath,
        VkmShaderCacheBackend expectedBackend, std::string* outError = nullptr);
} // namespace vkm
