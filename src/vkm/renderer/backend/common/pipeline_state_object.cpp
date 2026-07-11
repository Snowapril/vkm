// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/pipeline_state_object.h>

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
} // namespace vkm
