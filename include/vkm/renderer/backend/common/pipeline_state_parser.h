// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/pipeline_state.h>

#include <optional>
#include <string>

namespace vkm
{
    // Parses a VkmPipelineStateDescriptor from a JSON file on disk.
    // `filepath` may be absolute or relative to the process's CWD (callers typically
    // prefix with RESOURCES_DIR or a sample's own resource path).
    //
    // Returns std::nullopt on any failure: file not found/unreadable, malformed JSON,
    // an unrecognized enum string for any field, a JSON value of the wrong type,
    // or a missing required field (currently only shaders.vertex.filepath).
    // On failure, a human-readable message is written to *outError (if non-null)
    // and also logged via VKM_DEBUG_ERROR. Fields the JSON omits are populated with
    // the documented defaults from pipeline_state.h's default member initializers.
    std::optional<VkmPipelineStateDescriptor> parsePipelineStateFromFile(
        const std::string& filepath,
        std::string* outError = nullptr);

    // Identical semantics to parsePipelineStateFromFile, but parses a JSON document
    // already held in memory (e.g. a string literal embedded in sample source code)
    // instead of reading it from a file.
    std::optional<VkmPipelineStateDescriptor> parsePipelineStateFromString(
        const std::string& jsonText,
        std::string* outError = nullptr);
} // namespace vkm
