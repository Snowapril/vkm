// Copyright (c) 2025 Snowapril

#pragma once

#include <cstdint>

namespace vkm
{
    // On-disk format for a single compiled shader stage produced by vkm-compiler.
    // Every .vfcache file is a VkmShaderCacheHeader written at offset 0, followed
    // by exactly `contentSize` bytes of raw content (SPIR-V binary words, a metallib
    // binary, or UTF-8 MSL/WGSL text depending on `contentFormat`).
    //
    // This header is the contract for the future runtime reader (out of scope
    // here: VkmDriverBase has no shader-module/pipeline-creation method yet, see
    // TODO.md). Bump kVkmShaderCacheVersion whenever this layout changes.

    // 'V''F''C''H' little-endian magic identifying a vkm shader cache file.
    constexpr uint32_t kVkmShaderCacheMagic = 0x48434656u;
    constexpr uint32_t kVkmShaderCacheVersion = 2u;

    enum class VkmShaderCacheBackend : uint8_t
    {
        Vulkan = 0,
        Metal = 1,
        WebGPU = 2,
    };

    enum class VkmShaderCacheStage : uint8_t
    {
        Vertex = 0,
        Fragment = 1,
        Compute = 2,
    };

    enum class VkmShaderCacheContentFormat : uint8_t
    {
        SpirV = 0,
        Msl = 1,
        Wgsl = 2,
        // Precompiled Metal library (metallib binary, output of `metal`/`metallib`).
        // Loaded at runtime via -[MTLDevice newLibraryWithData:] -- unlike Msl source,
        // a binary library serializes into Xcode GPU captures without a replay-time
        // recompile (see metal_pipeline_state.mm).
        MetalLib = 3,
    };

#pragma pack(push, 1)
    struct VkmShaderCacheHeader
    {
        uint32_t magic = kVkmShaderCacheMagic;
        uint32_t version = kVkmShaderCacheVersion;
        VkmShaderCacheBackend backend;
        VkmShaderCacheStage stage;
        VkmShaderCacheContentFormat contentFormat;
        char entryPoint[64] = {}; // NUL-padded UTF-8
        uint64_t contentSize;
    };
#pragma pack(pop)
} // namespace vkm
