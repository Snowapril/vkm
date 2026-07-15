// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/shader_cache.h>

#include <string>

namespace vkm
{
    // Builds the on-disk filename (no directory) for one compiled shader stage's
    // .vfcache file, shared by vkm-compiler (producer) and the runtime loader
    // (consumer) so the two can never drift on naming.
    //
    // `shaderStem` = std::filesystem::path(stageDesc.filepath).stem() (e.g.
    // "triangle" for "triangle.hlsl"). `optionName` = the owning descriptor's
    // optionName ("" for the base/no-options case, in which the produced name is
    // byte-identical to what vkm-compiler wrote before option support existed):
    //   buildShaderCacheFilename("triangle", "", Vertex, Vulkan)
    //     == "triangle.vert.vulkan.vfcache"
    //   buildShaderCacheFilename("triangle", "wireframe", Vertex, Vulkan)
    //     == "triangle[wireframe].vert.vulkan.vfcache"
    std::string buildShaderCacheFilename(const std::string& shaderStem, const std::string& optionName,
        VkmShaderCacheStage stage, VkmShaderCacheBackend backend);

    // The backend this vkmcore build targets. One backend is active per build (see
    // ShaderCompile.cmake's identical selection); the priority order below matches it.
    inline VkmShaderCacheBackend vkmActiveShaderCacheBackend()
    {
#if defined(VKM_USE_VULKAN_API)
        return VkmShaderCacheBackend::Vulkan;
#elif defined(VKM_USE_METAL_API)
        return VkmShaderCacheBackend::Metal;
#else
        return VkmShaderCacheBackend::WebGPU;
#endif
    }
} // namespace vkm
