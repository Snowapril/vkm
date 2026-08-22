// Copyright (c) 2026 Snowapril

#pragma once

#include <vkm/renderer/backend/common/shader_cache.h>

#include <string>

namespace vkm
{
    /*
    * @brief Builds the on-disk filename, without directory, for one compiled shader stage's
    * .vfcache file.
    * @details Shared by vkm-compiler as producer and the runtime loader as consumer, so the two
    * cannot drift on naming.
    *   buildShaderCacheFilename("triangle", "", Vertex, Vulkan)
    *     == "triangle.vert.vulkan.vfcache"
    *   buildShaderCacheFilename("triangle", "wireframe", Vertex, Vulkan)
    *     == "triangle[wireframe].vert.vulkan.vfcache"
    * @param shaderStem std::filesystem::path(stageDesc.filepath).stem(), e.g. "triangle".
    * @param optionName The owning descriptor's optionName, empty for the base case.
    * @param stage Shader stage the cache holds.
    * @param backend Backend the cache was compiled for.
    * @return The filename.
    */
    std::string buildShaderCacheFilename(const std::string& shaderStem, const std::string& optionName,
        VkmShaderCacheStage stage, VkmShaderCacheBackend backend);

    // The backend this vkmcore build targets. One backend is active per build; the priority order
    // below matches ShaderCompile.cmake's identical selection.
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

    // The spelling vkm-compiler's --backend option expects. Kept beside the filename builder so
    // producer and consumer cannot drift on it.
    inline const char* vkmShaderCacheBackendName(VkmShaderCacheBackend backend)
    {
        switch (backend)
        {
            case VkmShaderCacheBackend::Vulkan: return "vulkan";
            case VkmShaderCacheBackend::Metal:  return "metal";
            case VkmShaderCacheBackend::WebGPU: return "webgpu";
        }
        return "";
    }
} // namespace vkm
