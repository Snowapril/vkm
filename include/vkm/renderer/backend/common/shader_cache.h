// Copyright (c) 2025 Snowapril

#pragma once

#include <cstdint>

namespace vkm
{
    // On-disk format for a single compiled shader stage produced by vkm-compiler. Every .vfcache
    // file is a VkmShaderCacheHeader written at offset 0, followed by exactly `contentSize` bytes
    // of raw content: SPIR-V binary words, a metallib binary, or UTF-8 MSL/WGSL text, per
    // `contentFormat`. Bump kVkmShaderCacheVersion whenever this layout changes.

    // 'V''F''C''H' little-endian magic identifying a vkm shader cache file.
    constexpr uint32_t kVkmShaderCacheMagic = 0x48434656u;
    constexpr uint32_t kVkmShaderCacheVersion = 3u;

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
        // Precompiled Metal library (metallib binary), loaded via
        // -[MTLDevice newLibraryWithData:]. Unlike Msl source, a binary library serializes into
        // Xcode GPU captures without a replay-time recompile.
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

        /*
        * @brief The compute stage's declared [numthreads(x, y, z)], read out of the compiled
        * SPIR-V. {0, 0, 0} for non-compute stages.
        * @details Metal needs this at dispatch time, MTLComputePipelineState not being able to
        * report what threadgroup size its function declared, and getting it wrong dispatches the
        * wrong number of threads with no diagnostic. Carrying it in the cache keeps the shader the
        * single source of truth. Vulkan and WebGPU take the size from the shader module and ignore
        * this.
        */
        uint32_t threadGroupSize[3] = {};

        uint64_t contentSize;
    };
#pragma pack(pop)
} // namespace vkm
