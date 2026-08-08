// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_barrier.h>
#include <vkm/renderer/backend/vulkan/vulkan_util.h>

namespace vkm
{
    namespace
    {
        // The shader stages a scope can run shader work in. Graphics covers vertex and fragment
        // because the engine has no other raster stages bound (see VkmPipelineStateVulkan).
        VkPipelineStageFlags2 shaderStagesOf(VkmPipelineScope scope)
        {
            switch (scope)
            {
                case VkmPipelineScope::Graphics:
                    return VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
                case VkmPipelineScope::Compute:
                    return VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
                case VkmPipelineScope::Transfer:
                    // A transfer subgraph runs no shaders; if one somehow declares a shader access,
                    // ordering it against every stage is the answer that cannot be wrong.
                    return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
                default:
                    return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
            }
        }
    } // namespace

    VkPipelineStageFlags2 vkmToVkStageMask(VkmResourceAccess access, VkmPipelineScope scope)
    {
        switch (access)
        {
            // Produced outside this graph -- a host upload, a previous frame, a freshly acquired
            // swapchain image. Nothing here can narrow that.
            case VkmResourceAccess::None:
                return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

            case VkmResourceAccess::IndirectArgument:
                return VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT;

            case VkmResourceAccess::ConstantBufferRead:
            case VkmResourceAccess::ShaderSampledRead:
            case VkmResourceAccess::ShaderStorageRead:
            case VkmResourceAccess::ShaderStorageWrite:
            case VkmResourceAccess::ShaderStorageReadWrite:
            case VkmResourceAccess::AccelerationStructureShaderRead:
                return shaderStagesOf(scope);

            case VkmResourceAccess::ColorAttachmentWrite:
                return VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

            case VkmResourceAccess::DepthStencilAttachmentWrite:
                return VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                       VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

            // Read-only depth can be both tested against and sampled in the same pass.
            case VkmResourceAccess::DepthStencilAttachmentRead:
                return VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT |
                       VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT |
                       VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;

            case VkmResourceAccess::TransferRead:
            case VkmResourceAccess::TransferWrite:
                return VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;

            case VkmResourceAccess::AccelerationStructureBuildRead:
            case VkmResourceAccess::AccelerationStructureBuildWrite:
                return VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;

            // Presentation is outside the pipeline; ALL_COMMANDS is the only honest answer.
            case VkmResourceAccess::Present:
                return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

            default:
                return VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        }
    }

    VkAccessFlags2 vkmToVkAccessMask(VkmResourceAccess access)
    {
        switch (access)
        {
            // As a source this has to cover whatever the outside world wrote; as a destination the
            // caller does not use it.
            case VkmResourceAccess::None:
                return VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;

            case VkmResourceAccess::IndirectArgument:
                return VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT;
            case VkmResourceAccess::ConstantBufferRead:
                return VK_ACCESS_2_UNIFORM_READ_BIT;
            case VkmResourceAccess::ShaderSampledRead:
                return VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
            case VkmResourceAccess::ShaderStorageRead:
                return VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            case VkmResourceAccess::ShaderStorageWrite:
                return VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            case VkmResourceAccess::ShaderStorageReadWrite:
                return VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;

            // READ as well as WRITE: a VkmLoadAction::Load pass reads what is already there, and
            // blending reads it regardless of the load action.
            case VkmResourceAccess::ColorAttachmentWrite:
                return VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT;
            case VkmResourceAccess::DepthStencilAttachmentWrite:
                return VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                       VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
            case VkmResourceAccess::DepthStencilAttachmentRead:
                return VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;

            case VkmResourceAccess::TransferRead:
                return VK_ACCESS_2_TRANSFER_READ_BIT;
            case VkmResourceAccess::TransferWrite:
                return VK_ACCESS_2_TRANSFER_WRITE_BIT;

            // A build reads its vertex/index/instance buffers as shader reads and the structures it
            // refits from as acceleration structure reads.
            case VkmResourceAccess::AccelerationStructureBuildRead:
                return VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
            case VkmResourceAccess::AccelerationStructureBuildWrite:
                return VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
            case VkmResourceAccess::AccelerationStructureShaderRead:
                return VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;

            // The presentation engine is not a pipeline stage and reads through no access flag;
            // the layout transition is the whole point of a barrier naming it.
            case VkmResourceAccess::Present:
                return VK_ACCESS_2_NONE;

            default:
                return VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT;
        }
    }

    VkImageLayout vkmToVkImageLayout(VkmResourceAccess access, VkmFormat format)
    {
        switch (access)
        {
            case VkmResourceAccess::ShaderSampledRead:
                // The layout the bindless texture descriptors declare
                // (VkmBindlessResourceManagerVulkan writes SHADER_READ_ONLY_OPTIMAL), so anything
                // sampled through set 0 has to actually be in it.
                return (hasDepth(format) || hasStencil(format))
                           ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                           : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            // A storage image has no read-only layout to sit in.
            case VkmResourceAccess::ShaderStorageRead:
            case VkmResourceAccess::ShaderStorageWrite:
            case VkmResourceAccess::ShaderStorageReadWrite:
                return VK_IMAGE_LAYOUT_GENERAL;

            case VkmResourceAccess::ColorAttachmentWrite:
                return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            case VkmResourceAccess::DepthStencilAttachmentWrite:
                return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
            case VkmResourceAccess::DepthStencilAttachmentRead:
                return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

            case VkmResourceAccess::TransferRead:
                return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            case VkmResourceAccess::TransferWrite:
                return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

            case VkmResourceAccess::Present:
                return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

            // Buffer-only accesses, and None. UNDEFINED here means "this access names no layout",
            // which the caller reads as "leave the texture where it is".
            default:
                return VK_IMAGE_LAYOUT_UNDEFINED;
        }
    }

    VkImageAspectFlags vkmToVkAspectMask(VkmFormat format)
    {
        if (hasDepth(format) || hasStencil(format))
        {
            return (hasDepth(format) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0u) |
                   (hasStencil(format) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
        }
        return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}
