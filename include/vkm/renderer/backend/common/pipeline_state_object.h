// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/pipeline_state.h>

#include <string>

namespace vkm
{
    class VkmDriverBase;

    // A single backend pipeline object built from one fully-resolved
    // VkmPipelineStateDescriptor (already expanded via expandPipelineStateOptions() if it
    // came from a JSON node with "options" -- this class itself knows nothing about option
    // expansion). On Vulkan, every pipeline shares the engine-global bindless set 0 (see
    // VkmBindlessResourceManagerVulkan); descriptor sets 1-3 and any Metal/WebGPU
    // resource-binding equivalent remain a deferred follow-up.
    class VkmPipelineStateBase
    {
    public:
        explicit VkmPipelineStateBase(VkmDriverBase* driver);
        virtual ~VkmPipelineStateBase();

        // `shaderCacheDir`: directory containing the .vfcache files vkm-compiler wrote for
        // this exact descriptor (one per present stage; filenames per
        // buildShaderCacheFilename() in shader_cache_util.h). Loads each stage's cache file,
        // builds a transient backend shader module/library, and creates the real backend
        // pipeline object.
        bool initialize(const VkmPipelineStateDescriptor& desc, const std::string& shaderCacheDir, std::string* outError = nullptr);
        void destroy();

        inline const VkmPipelineStateDescriptor& getDescriptor() const { return _descriptor; }
        inline bool isCompute() const { return _descriptor.computeShader.has_value(); }
        inline const std::string& getName() const { return _descriptor.name; } // includes "[option]" suffix

    protected:
        // For each present stage in `desc`: compute its cache filename
        // (buildShaderCacheFilename + currentShaderCacheBackend()), loadShaderCacheFile() it,
        // build a transient backend shader module/library/function, and create either a
        // graphics or compute pipeline object depending on isCompute(). Must reject
        // unsupported fixed-function combinations noted in pipeline_state.h (e.g.
        // VkmFillMode::Point on non-Vulkan, VkmCullMode::FrontAndBack on Metal/WebGPU).
        virtual bool createInner(const VkmPipelineStateDescriptor& desc, const std::string& shaderCacheDir, std::string* outError) = 0;
        virtual void destroyInner() = 0;

    protected:
        VkmDriverBase* _driver;
        VkmPipelineStateDescriptor _descriptor;
    };
} // namespace vkm
