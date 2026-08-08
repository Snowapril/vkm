// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/pipeline_state.h>

#include <string>

namespace vkm
{
    class VkmDriverBase;

    /*
    * @brief A single backend pipeline object built from one fully-resolved
    * VkmPipelineStateDescriptor.
    * @details The descriptor must already be expanded via expandPipelineStateOptions() if it came
    * from a JSON node with "options" -- this class knows nothing about option expansion.
    */
    class VkmPipelineStateBase
    {
    public:
        explicit VkmPipelineStateBase(VkmDriverBase* driver);
        virtual ~VkmPipelineStateBase();

        /*
        * @brief Loads each stage's shader cache and creates the backend pipeline object.
        * @param desc Fully-resolved descriptor.
        * @param shaderCacheDir Directory holding the .vfcache files vkm-compiler wrote for this
        * exact descriptor, one per present stage, named per buildShaderCacheFilename().
        * @param outError Receives the failure reason. May be null.
        * @return False if a cache file could not be loaded or the pipeline could not be created.
        */
        bool initialize(const VkmPipelineStateDescriptor& desc, const std::string& shaderCacheDir, std::string* outError = nullptr);
        void destroy();

        /*
        * @brief Rebuild this pipeline in place, keeping the object's address so every cached raw
        * VkmPipelineStateBase* stays valid.
        * @details Samples, render-graph render callbacks and VkmCommandBufferBase's bound-pipeline
        * history all hold non-owning pointers with no invalidation hook. The caller must ensure no
        * in-flight GPU work still references this pipeline, destroyInner() being synchronous;
        * VkmPipelineStateManager calls VkmDriverBase::waitIdle() once per reload batch.
        * @param desc New fully-resolved descriptor.
        * @param shaderCacheDir Directory holding its .vfcache files.
        * @param outError Receives the failure reason. May be null.
        * @return False on failure, in which case the previous descriptor is rebuilt so the pipeline
        * is left usable.
        */
        bool reload(const VkmPipelineStateDescriptor& desc, const std::string& shaderCacheDir, std::string* outError = nullptr);

        inline const VkmPipelineStateDescriptor& getDescriptor() const { return _descriptor; }
        inline bool isCompute() const { return _descriptor.computeShader.has_value(); }
        inline const std::string& getName() const { return _descriptor.name; } // includes "[option]" suffix

    protected:
        // For each present stage: compute its cache filename (buildShaderCacheFilename +
        // currentShaderCacheBackend()), loadShaderCacheFile() it, build a transient backend shader
        // module/library/function, and create a graphics or compute pipeline per isCompute(). Must
        // reject the unsupported fixed-function combinations noted in pipeline_state.h.
        virtual bool createInner(const VkmPipelineStateDescriptor& desc, const std::string& shaderCacheDir, std::string* outError) = 0;
        virtual void destroyInner() = 0;

    protected:
        VkmDriverBase* _driver;
        VkmPipelineStateDescriptor _descriptor;
    };
} // namespace vkm
