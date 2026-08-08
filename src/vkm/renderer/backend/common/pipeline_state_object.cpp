// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/pipeline_state_object.h>

#include <vkm/base/common.h>

namespace vkm
{
    VkmPipelineStateBase::VkmPipelineStateBase(VkmDriverBase* driver)
        : _driver(driver)
    {
    }

    VkmPipelineStateBase::~VkmPipelineStateBase()
    {
    }

    bool VkmPipelineStateBase::initialize(const VkmPipelineStateDescriptor& desc, const std::string& shaderCacheDir, std::string* outError)
    {
        _descriptor = desc;
        return createInner(desc, shaderCacheDir, outError);
    }

    void VkmPipelineStateBase::destroy()
    {
        destroyInner();
    }

    bool VkmPipelineStateBase::reload(const VkmPipelineStateDescriptor& desc, const std::string& shaderCacheDir, std::string* outError)
    {
        destroyInner();
        if (createInner(desc, shaderCacheDir, outError))
        {
            _descriptor = desc;
            return true;
        }

        // Roll back to the last known-good descriptor so a bad edit leaves a working pipeline
        // behind rather than a destroyed one every cached pointer still refers to. destroyInner()
        // again first: a failed createInner() can return with some of its objects already created.
        // A failure here is a descriptor problem rather than a shader one -- the caller only gets
        // this far after vkm-compiler exited 0, and it writes a .vfcache only for a stage that
        // compiled -- so the rollback pairs the freshly compiled shaders with the previous render
        // state, which is still a working pipeline.
        destroyInner();
        std::string rollbackError;
        if (!createInner(_descriptor, shaderCacheDir, &rollbackError))
        {
            VKM_DEBUG_ERROR(("Pipeline state '" + _descriptor.name +
                "' could not be restored after a failed reload: " + rollbackError).c_str());
        }
        return false;
    }
} // namespace vkm
