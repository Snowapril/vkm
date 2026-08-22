// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/renderer/backend/common/pipeline_state.h>

#include <optional>
#include <string>
#include <vector>

namespace vkm
{
    /*
    * @brief Parses a VkmPipelineStateDescriptor from a JSON file on disk.
    * @details Fields the JSON omits are populated with the documented defaults from
    * pipeline_state.h's default member initializers.
    * @param filepath Absolute, or relative to the process's CWD. Callers typically prefix with
    * RESOURCES_DIR or a sample's own resource path.
    * @param outError Receives a human-readable message, also logged via VKM_DEBUG_ERROR. May be null.
    * @return The descriptor, or nullopt on any failure: file not found or unreadable, malformed
    * JSON, an unrecognized enum string, a JSON value of the wrong type, or a missing required
    * field.
    */
    std::optional<VkmPipelineStateDescriptor> parsePipelineStateFromFile(
        const std::string& filepath,
        std::string* outError = nullptr);

    /*
    * @brief Parses a JSON document already held in memory, such as a string literal embedded in
    * sample source. Identical semantics to parsePipelineStateFromFile.
    * @param jsonText Document to parse.
    * @param outError Receives a human-readable message. May be null.
    * @return The descriptor, or nullopt on any parse failure.
    */
    std::optional<VkmPipelineStateDescriptor> parsePipelineStateFromString(
        const std::string& jsonText,
        std::string* outError = nullptr);

    /*
    * @brief Expands a descriptor into one fully-resolved descriptor per entry in its options.
    * @details Each variant is renamed to "<base.name>[<option_name>]", with fixed-function fields
    * and shader-stage definitions overridden per the precedence documented in pipeline_state.h.
    * @param base Descriptor to expand. When its options are empty, the result is a single
    * unmodified copy.
    * @param backend The caller's active backend: vkmcore its compile-time one, vkm-compiler its
    * --backend argument. Options whose non-empty "backends" allowlist excludes it are dropped.
    * @param outError Receives the failure reason. May be null.
    * @return The variants, or nullopt when `base.options` is non-empty but `base.name` is empty, or
    * a resolved variant ends up with both a compute and a graphics stage set.
    */
    std::optional<std::vector<VkmPipelineStateDescriptor>> expandPipelineStateOptions(
        const VkmPipelineStateDescriptor& base, VkmShaderCacheBackend backend, std::string* outError = nullptr);
} // namespace vkm
