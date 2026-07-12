// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/shader_cache_util.h>

namespace vkm
{
    namespace
    {
        // Stage/backend name literals must match vkm-compiler's historical filename
        // components exactly so the optionName == "" case stays byte-identical.
        const char* stageShortName(VkmShaderCacheStage stage)
        {
            switch (stage)
            {
                case VkmShaderCacheStage::Vertex:   return "vert";
                case VkmShaderCacheStage::Fragment: return "frag";
                case VkmShaderCacheStage::Compute:  return "comp";
            }
            return "vert";
        }

        const char* backendName(VkmShaderCacheBackend backend)
        {
            switch (backend)
            {
                case VkmShaderCacheBackend::Vulkan: return "vulkan";
                case VkmShaderCacheBackend::Metal:  return "metal";
                case VkmShaderCacheBackend::WebGPU: return "webgpu";
            }
            return "vulkan";
        }
    } // namespace

    std::string buildShaderCacheFilename(const std::string& shaderStem, const std::string& optionName,
        VkmShaderCacheStage stage, VkmShaderCacheBackend backend)
    {
        std::string name = shaderStem;
        if (!optionName.empty())
        {
            name += "[" + optionName + "]";
        }
        name += ".";
        name += stageShortName(stage);
        name += ".";
        name += backendName(backend);
        name += ".vfcache";
        return name;
    }
} // namespace vkm
