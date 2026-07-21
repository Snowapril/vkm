// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/vulkan/vulkan_pipeline_state.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/backend/vulkan/vulkan_util.h>
#include <vkm/renderer/backend/vulkan/vulkan_bindless_resource_manager.h>
#include <vkm/renderer/backend/common/shader_cache_loader.h>
#include <vkm/renderer/backend/common/shader_cache_util.h>

#include <array>
#include <filesystem>

namespace vkm
{
    namespace
    {
        void setError(std::string* outError, const std::string& message)
        {
            if (outError != nullptr && outError->empty())
            {
                *outError = message;
            }
        }

        VkPrimitiveTopology toVkPrimitiveTopology(VkmPrimitiveTopology topology)
        {
            switch (topology)
            {
                case VkmPrimitiveTopology::PointList:     return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
                case VkmPrimitiveTopology::LineList:      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
                case VkmPrimitiveTopology::LineStrip:     return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
                case VkmPrimitiveTopology::TriangleList:  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
                case VkmPrimitiveTopology::TriangleStrip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
            }
            return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        }

        VkPolygonMode toVkPolygonMode(VkmFillMode fillMode)
        {
            switch (fillMode)
            {
                case VkmFillMode::Solid:     return VK_POLYGON_MODE_FILL;
                case VkmFillMode::Wireframe: return VK_POLYGON_MODE_LINE;
                case VkmFillMode::Point:     return VK_POLYGON_MODE_POINT;
            }
            return VK_POLYGON_MODE_FILL;
        }

        VkCullModeFlags toVkCullModeFlags(VkmCullMode cullMode)
        {
            switch (cullMode)
            {
                case VkmCullMode::None:         return VK_CULL_MODE_NONE;
                case VkmCullMode::Front:        return VK_CULL_MODE_FRONT_BIT;
                case VkmCullMode::Back:         return VK_CULL_MODE_BACK_BIT;
                case VkmCullMode::FrontAndBack: return VK_CULL_MODE_FRONT_AND_BACK;
            }
            return VK_CULL_MODE_BACK_BIT;
        }

        // The mapping is deliberately INVERTED -- do not "fix" it. vkm-compiler builds Vulkan
        // SPIR-V with -fvk-invert-y, negating SV_Position.y to put the engine's +Y-up clip
        // space on Vulkan's +Y-down NDC. That Y-flip also mirrors triangle winding in screen
        // space. Inverting the enum here cancels that out, so a PSO declaring
        // "counter_clockwise" culls the same faces on Vulkan as it does on Metal/WebGPU.
        VkFrontFace toVkFrontFace(VkmFrontFace frontFace)
        {
            switch (frontFace)
            {
                case VkmFrontFace::CounterClockwise: return VK_FRONT_FACE_CLOCKWISE;
                case VkmFrontFace::Clockwise:        return VK_FRONT_FACE_COUNTER_CLOCKWISE;
            }
            return VK_FRONT_FACE_CLOCKWISE;
        }

        VkStencilOp toVkStencilOp(VkmStencilOp stencilOp)
        {
            switch (stencilOp)
            {
                case VkmStencilOp::Keep:              return VK_STENCIL_OP_KEEP;
                case VkmStencilOp::Zero:              return VK_STENCIL_OP_ZERO;
                case VkmStencilOp::Replace:           return VK_STENCIL_OP_REPLACE;
                case VkmStencilOp::IncrementAndClamp: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
                case VkmStencilOp::DecrementAndClamp: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
                case VkmStencilOp::Invert:            return VK_STENCIL_OP_INVERT;
                case VkmStencilOp::IncrementAndWrap:  return VK_STENCIL_OP_INCREMENT_AND_WRAP;
                case VkmStencilOp::DecrementAndWrap:  return VK_STENCIL_OP_DECREMENT_AND_WRAP;
            }
            return VK_STENCIL_OP_KEEP;
        }

        VkBlendFactor toVkBlendFactor(VkmBlendFactor blendFactor)
        {
            switch (blendFactor)
            {
                case VkmBlendFactor::Zero:                 return VK_BLEND_FACTOR_ZERO;
                case VkmBlendFactor::One:                  return VK_BLEND_FACTOR_ONE;
                case VkmBlendFactor::SrcColor:              return VK_BLEND_FACTOR_SRC_COLOR;
                case VkmBlendFactor::OneMinusSrcColor:      return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
                case VkmBlendFactor::DstColor:              return VK_BLEND_FACTOR_DST_COLOR;
                case VkmBlendFactor::OneMinusDstColor:      return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
                case VkmBlendFactor::SrcAlpha:              return VK_BLEND_FACTOR_SRC_ALPHA;
                case VkmBlendFactor::OneMinusSrcAlpha:      return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                case VkmBlendFactor::DstAlpha:              return VK_BLEND_FACTOR_DST_ALPHA;
                case VkmBlendFactor::OneMinusDstAlpha:      return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
                case VkmBlendFactor::ConstantColor:         return VK_BLEND_FACTOR_CONSTANT_COLOR;
                case VkmBlendFactor::OneMinusConstantColor: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
                case VkmBlendFactor::ConstantAlpha:         return VK_BLEND_FACTOR_CONSTANT_ALPHA;
                case VkmBlendFactor::OneMinusConstantAlpha: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
                case VkmBlendFactor::SrcAlphaSaturate:      return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
            }
            return VK_BLEND_FACTOR_ONE;
        }

        VkBlendOp toVkBlendOp(VkmBlendOp blendOp)
        {
            switch (blendOp)
            {
                case VkmBlendOp::Add:             return VK_BLEND_OP_ADD;
                case VkmBlendOp::Subtract:        return VK_BLEND_OP_SUBTRACT;
                case VkmBlendOp::ReverseSubtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
                case VkmBlendOp::Min:             return VK_BLEND_OP_MIN;
                case VkmBlendOp::Max:             return VK_BLEND_OP_MAX;
            }
            return VK_BLEND_OP_ADD;
        }

        // Undefined is a valid, common value (e.g. "no depth attachment") -- unlike
        // toVkFormat() in vulkan_util.h, this must not assert on it.
        VkFormat toVkFormatOrUndefined(VkmFormat format)
        {
            return (format == VkmFormat::Undefined) ? VK_FORMAT_UNDEFINED : toVkFormat(format);
        }

        VkFormat toVkVertexAttributeFormat(VkmVertexAttributeBaseType baseType, uint32_t componentCount)
        {
            switch (baseType)
            {
                case VkmVertexAttributeBaseType::Float:
                    switch (componentCount)
                    {
                        case 1: return VK_FORMAT_R32_SFLOAT;
                        case 2: return VK_FORMAT_R32G32_SFLOAT;
                        case 3: return VK_FORMAT_R32G32B32_SFLOAT;
                        case 4: return VK_FORMAT_R32G32B32A32_SFLOAT;
                        default: break;
                    }
                    break;
                case VkmVertexAttributeBaseType::Int:
                    switch (componentCount)
                    {
                        case 1: return VK_FORMAT_R32_SINT;
                        case 2: return VK_FORMAT_R32G32_SINT;
                        case 3: return VK_FORMAT_R32G32B32_SINT;
                        case 4: return VK_FORMAT_R32G32B32A32_SINT;
                        default: break;
                    }
                    break;
                case VkmVertexAttributeBaseType::UInt:
                    switch (componentCount)
                    {
                        case 1: return VK_FORMAT_R32_UINT;
                        case 2: return VK_FORMAT_R32G32_UINT;
                        case 3: return VK_FORMAT_R32G32B32_UINT;
                        case 4: return VK_FORMAT_R32G32B32A32_UINT;
                        default: break;
                    }
                    break;
            }
            VKM_ASSERT(false, "Unsupported vertex attribute base type/component count combination");
            return VK_FORMAT_UNDEFINED;
        }

        // Loads one stage's .vfcache file and builds a transient VkShaderModule from its
        // SPIR-V content. On success, `*outModule`/`*outEntryPoint` are populated and the
        // caller owns destroying the module. On failure, `*outError` is set (unless the
        // loader already set it) and `false` is returned.
        bool loadStageModule(VkDevice device, const VkmShaderStageDescriptor& stageDesc, const std::string& optionName,
            const std::string& shaderCacheDir, VkmShaderCacheStage stage, VkShaderModule* outModule, std::string* outEntryPoint, std::string* outError)
        {
            const std::string stem = std::filesystem::path(stageDesc.filepath).stem().string();
            const std::string filename = buildShaderCacheFilename(stem, optionName, stage, VkmShaderCacheBackend::Vulkan);
            const std::string filepath = (std::filesystem::path(shaderCacheDir) / filename).string();

            std::optional<VkmLoadedShaderCache> loaded = loadShaderCacheFile(filepath, VkmShaderCacheBackend::Vulkan, outError);
            if (!loaded.has_value())
            {
                setError(outError, "Failed to load shader cache file: " + filepath);
                return false;
            }

            const VkShaderModuleCreateInfo moduleCreateInfo{
                .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                .codeSize = loaded->content.size(),
                .pCode    = reinterpret_cast<const uint32_t*>(loaded->content.data()),
            };
            const VkResult vkResult = vkCreateShaderModule(device, &moduleCreateInfo, nullptr, outModule);
            if (!VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to create shader module"))
            {
                setError(outError, "Failed to create VkShaderModule for " + filepath);
                return false;
            }

            *outEntryPoint = loaded->entryPoint;
            return true;
        }
    } // namespace

    VkmPipelineStateVulkan::VkmPipelineStateVulkan(VkmDriverBase* driver)
        : VkmPipelineStateBase(driver)
    {
    }

    VkmPipelineStateVulkan::~VkmPipelineStateVulkan()
    {
    }

    bool VkmPipelineStateVulkan::createInner(const VkmPipelineStateDescriptor& desc, const std::string& shaderCacheDir, std::string* outError)
    {
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        VkDevice device = driverVulkan->getDevice();

        // Every pipeline shares the engine-global bindless set 0 (VkmBindlessResourceManagerVulkan)
        // plus a push-constant range carrying the current draw's bindless slot indices
        // (vertexBufferIndex, indexBufferIndex). Sets 1-3 remain unreserved/deferred.
        VkDescriptorSetLayout bindlessSetLayout = driverVulkan->getBindlessResourceManager()->getSetLayout();
        const VkPushConstantRange pushConstantRange{
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
            .offset     = 0,
            .size       = sizeof(uint32_t) * 2,
        };
        const VkPipelineLayoutCreateInfo layoutCreateInfo{
            .sType                  = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount         = 1,
            .pSetLayouts            = &bindlessSetLayout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges    = &pushConstantRange,
        };
        VkResult vkResult = vkCreatePipelineLayout(device, &layoutCreateInfo, nullptr, &_pipelineLayout);
        if (!VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to create pipeline layout"))
        {
            setError(outError, "Failed to create VkPipelineLayout for " + desc.name);
            return false;
        }

        if (desc.computeShader.has_value())
        {
            VkShaderModule computeModule = VK_NULL_HANDLE;
            std::string entryPoint;
            if (!loadStageModule(device, *desc.computeShader, desc.optionName, shaderCacheDir, VkmShaderCacheStage::Compute, &computeModule, &entryPoint, outError))
            {
                vkDestroyPipelineLayout(device, _pipelineLayout, nullptr);
                _pipelineLayout = VK_NULL_HANDLE;
                return false;
            }

            const VkPipelineShaderStageCreateInfo stageCreateInfo{
                .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage  = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = computeModule,
                .pName  = entryPoint.c_str(),
            };
            const VkComputePipelineCreateInfo computeCreateInfo{
                .sType  = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                .stage  = stageCreateInfo,
                .layout = _pipelineLayout,
            };
            vkResult = vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &computeCreateInfo, nullptr, &_pipeline);

            vkDestroyShaderModule(device, computeModule, nullptr);

            if (!VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to create compute pipeline"))
            {
                setError(outError, "Failed to create VkPipeline (compute) for " + desc.name);
                vkDestroyPipelineLayout(device, _pipelineLayout, nullptr);
                _pipelineLayout = VK_NULL_HANDLE;
                return false;
            }
            return true;
        }

        // --- Graphics pipeline (dynamic rendering; no VkRenderPass/VkFramebuffer) ---
        std::vector<VkPipelineShaderStageCreateInfo> stageCreateInfos;
        std::vector<VkShaderModule> transientModules;
        std::string vertexEntryPoint, fragmentEntryPoint;

        if (desc.vertexShader.has_value())
        {
            VkShaderModule vertexModule = VK_NULL_HANDLE;
            if (!loadStageModule(device, *desc.vertexShader, desc.optionName, shaderCacheDir, VkmShaderCacheStage::Vertex, &vertexModule, &vertexEntryPoint, outError))
            {
                vkDestroyPipelineLayout(device, _pipelineLayout, nullptr);
                _pipelineLayout = VK_NULL_HANDLE;
                return false;
            }
            transientModules.push_back(vertexModule);
        }

        if (desc.fragmentShader.has_value())
        {
            VkShaderModule fragmentModule = VK_NULL_HANDLE;
            if (!loadStageModule(device, *desc.fragmentShader, desc.optionName, shaderCacheDir, VkmShaderCacheStage::Fragment, &fragmentModule, &fragmentEntryPoint, outError))
            {
                for (VkShaderModule module : transientModules)
                {
                    vkDestroyShaderModule(device, module, nullptr);
                }
                vkDestroyPipelineLayout(device, _pipelineLayout, nullptr);
                _pipelineLayout = VK_NULL_HANDLE;
                return false;
            }
            transientModules.push_back(fragmentModule);
        }

        // Build stage-create-infos only after both entry-point strings are finalized, since
        // pName aliases their c_str() pointers -- reallocating either string afterward would
        // dangle it.
        if (desc.vertexShader.has_value())
        {
            stageCreateInfos.push_back(VkPipelineShaderStageCreateInfo{
                .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage  = VK_SHADER_STAGE_VERTEX_BIT,
                .module = transientModules[0],
                .pName  = vertexEntryPoint.c_str(),
            });
        }
        if (desc.fragmentShader.has_value())
        {
            const size_t moduleIndex = desc.vertexShader.has_value() ? 1 : 0;
            stageCreateInfos.push_back(VkPipelineShaderStageCreateInfo{
                .sType  = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage  = VK_SHADER_STAGE_FRAGMENT_BIT,
                .module = transientModules[moduleIndex],
                .pName  = fragmentEntryPoint.c_str(),
            });
        }

        // Vertex input: perVertex/perInstance parts each assign locations starting at 0
        // (see VkmVertexInputLayoutPart's contract), so per-instance locations must be
        // offset by however many attributes perVertex already claimed to avoid collisions.
        std::vector<VkVertexInputBindingDescription> bindingDescriptions;
        std::vector<VkVertexInputAttributeDescription> attributeDescriptions;
        uint32_t locationOffset = 0;
        if (desc.vertexInputLayout.perVertex.has_value())
        {
            const VkmVertexInputLayoutPart& part = *desc.vertexInputLayout.perVertex;
            const uint32_t binding = static_cast<uint32_t>(bindingDescriptions.size());
            bindingDescriptions.push_back(VkVertexInputBindingDescription{
                .binding   = binding,
                .stride    = part.stride,
                .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
            });
            for (const VkmVertexAttributeDescriptor& attribute : part.attributes)
            {
                attributeDescriptions.push_back(VkVertexInputAttributeDescription{
                    .location = locationOffset + attribute.location,
                    .binding  = binding,
                    .format   = toVkVertexAttributeFormat(attribute.baseType, attribute.componentCount),
                    .offset   = attribute.offset,
                });
            }
            locationOffset += static_cast<uint32_t>(part.attributes.size());
        }
        if (desc.vertexInputLayout.perInstance.has_value())
        {
            const VkmVertexInputLayoutPart& part = *desc.vertexInputLayout.perInstance;
            const uint32_t binding = static_cast<uint32_t>(bindingDescriptions.size());
            bindingDescriptions.push_back(VkVertexInputBindingDescription{
                .binding   = binding,
                .stride    = part.stride,
                .inputRate = VK_VERTEX_INPUT_RATE_INSTANCE,
            });
            for (const VkmVertexAttributeDescriptor& attribute : part.attributes)
            {
                attributeDescriptions.push_back(VkVertexInputAttributeDescription{
                    .location = locationOffset + attribute.location,
                    .binding  = binding,
                    .format   = toVkVertexAttributeFormat(attribute.baseType, attribute.componentCount),
                    .offset   = attribute.offset,
                });
            }
        }

        const VkPipelineVertexInputStateCreateInfo vertexInputState{
            .sType                           = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount   = static_cast<uint32_t>(bindingDescriptions.size()),
            .pVertexBindingDescriptions      = bindingDescriptions.data(),
            .vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size()),
            .pVertexAttributeDescriptions    = attributeDescriptions.data(),
        };

        const VkPipelineInputAssemblyStateCreateInfo inputAssemblyState{
            .sType    = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = toVkPrimitiveTopology(desc.primitiveTopology),
        };

        // Viewport/scissor extents are unknown at pipeline-creation time (no fixed
        // framebuffer size in this codebase's design) -- set via dynamic state instead.
        const VkPipelineViewportStateCreateInfo viewportState{
            .sType         = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .scissorCount  = 1,
        };

        const VkPipelineRasterizationStateCreateInfo rasterizationState{
            .sType       = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = toVkPolygonMode(desc.rasterizationState.fillMode),
            .cullMode    = toVkCullModeFlags(desc.rasterizationState.cullMode),
            .frontFace   = toVkFrontFace(desc.rasterizationState.frontFace),
            .lineWidth   = 1.0f,
        };

        const VkPipelineMultisampleStateCreateInfo multisampleState{
            .sType                = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        };

        // Static stencil reference for both faces -- this codebase has no per-face
        // reference convention (VkmDepthStencilStateDescriptor has a single shared
        // stencilReference field).
        const VkStencilOpState frontStencil{
            .failOp      = toVkStencilOp(desc.depthStencilState.front.failOp),
            .passOp      = toVkStencilOp(desc.depthStencilState.front.passOp),
            .depthFailOp = toVkStencilOp(desc.depthStencilState.front.depthFailOp),
            .compareOp   = toVkCompareOp(desc.depthStencilState.front.compareOp),
            .compareMask = desc.depthStencilState.front.compareMask,
            .writeMask   = desc.depthStencilState.front.writeMask,
            .reference   = desc.depthStencilState.stencilReference,
        };
        const VkStencilOpState backStencil{
            .failOp      = toVkStencilOp(desc.depthStencilState.back.failOp),
            .passOp      = toVkStencilOp(desc.depthStencilState.back.passOp),
            .depthFailOp = toVkStencilOp(desc.depthStencilState.back.depthFailOp),
            .compareOp   = toVkCompareOp(desc.depthStencilState.back.compareOp),
            .compareMask = desc.depthStencilState.back.compareMask,
            .writeMask   = desc.depthStencilState.back.writeMask,
            .reference   = desc.depthStencilState.stencilReference,
        };
        const VkPipelineDepthStencilStateCreateInfo depthStencilState{
            .sType             = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable   = desc.depthStencilState.depthTestEnable ? VK_TRUE : VK_FALSE,
            .depthWriteEnable  = desc.depthStencilState.depthWriteEnable ? VK_TRUE : VK_FALSE,
            .depthCompareOp    = toVkCompareOp(desc.depthStencilState.depthCompareOp),
            .stencilTestEnable = desc.depthStencilState.stencilTestEnable ? VK_TRUE : VK_FALSE,
            .front             = frontStencil,
            .back              = backStencil,
        };

        std::vector<VkPipelineColorBlendAttachmentState> colorBlendAttachments;
        std::vector<VkFormat> colorAttachmentFormats;
        colorBlendAttachments.reserve(desc.colorAttachments.size());
        colorAttachmentFormats.reserve(desc.colorAttachments.size());
        for (const VkmColorBlendAttachmentState& attachment : desc.colorAttachments)
        {
            colorBlendAttachments.push_back(VkPipelineColorBlendAttachmentState{
                .blendEnable         = attachment.blendEnable ? VK_TRUE : VK_FALSE,
                .srcColorBlendFactor = toVkBlendFactor(attachment.srcColorBlendFactor),
                .dstColorBlendFactor = toVkBlendFactor(attachment.dstColorBlendFactor),
                .colorBlendOp        = toVkBlendOp(attachment.colorBlendOp),
                .srcAlphaBlendFactor = toVkBlendFactor(attachment.srcAlphaBlendFactor),
                .dstAlphaBlendFactor = toVkBlendFactor(attachment.dstAlphaBlendFactor),
                .alphaBlendOp        = toVkBlendOp(attachment.alphaBlendOp),
                .colorWriteMask      = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
            });
            colorAttachmentFormats.push_back(toVkFormatOrUndefined(attachment.format));
        }

        const VkPipelineColorBlendStateCreateInfo colorBlendState{
            .sType           = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = static_cast<uint32_t>(colorBlendAttachments.size()),
            .pAttachments    = colorBlendAttachments.data(),
        };

        const std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        const VkPipelineDynamicStateCreateInfo dynamicState{
            .sType             = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = static_cast<uint32_t>(dynamicStates.size()),
            .pDynamicStates    = dynamicStates.data(),
        };

        const bool hasDepthAttachment = (desc.depthStencilState.depthStencilFormat != VkmFormat::Undefined);
        const VkFormat depthStencilVkFormat = toVkFormatOrUndefined(desc.depthStencilState.depthStencilFormat);
        const VkFormat depthAttachmentFormat = hasDepthAttachment ? depthStencilVkFormat : VK_FORMAT_UNDEFINED;
        const VkFormat stencilAttachmentFormat = hasStencil(desc.depthStencilState.depthStencilFormat) ? depthStencilVkFormat : VK_FORMAT_UNDEFINED;

        const VkPipelineRenderingCreateInfo renderingCreateInfo{
            .sType                   = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount    = static_cast<uint32_t>(colorAttachmentFormats.size()),
            .pColorAttachmentFormats = colorAttachmentFormats.data(),
            .depthAttachmentFormat   = depthAttachmentFormat,
            .stencilAttachmentFormat = stencilAttachmentFormat,
        };

        const VkGraphicsPipelineCreateInfo graphicsCreateInfo{
            .sType               = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext               = &renderingCreateInfo,
            .stageCount          = static_cast<uint32_t>(stageCreateInfos.size()),
            .pStages             = stageCreateInfos.data(),
            .pVertexInputState   = &vertexInputState,
            .pInputAssemblyState = &inputAssemblyState,
            .pViewportState      = &viewportState,
            .pRasterizationState = &rasterizationState,
            .pMultisampleState   = &multisampleState,
            .pDepthStencilState  = &depthStencilState,
            .pColorBlendState    = &colorBlendState,
            .pDynamicState       = &dynamicState,
            .layout              = _pipelineLayout,
        };

        vkResult = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &graphicsCreateInfo, nullptr, &_pipeline);

        for (VkShaderModule module : transientModules)
        {
            vkDestroyShaderModule(device, module, nullptr);
        }

        if (!VKM_VK_CHECK_RESULT_MSG(vkResult, "Failed to create graphics pipeline"))
        {
            setError(outError, "Failed to create VkPipeline (graphics) for " + desc.name);
            vkDestroyPipelineLayout(device, _pipelineLayout, nullptr);
            _pipelineLayout = VK_NULL_HANDLE;
            return false;
        }

        return true;
    }

    void VkmPipelineStateVulkan::destroyInner()
    {
        VkmDriverVulkan* driverVulkan = static_cast<VkmDriverVulkan*>(_driver);
        VkDevice device = driverVulkan->getDevice();

        if (_pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(device, _pipeline, nullptr);
            _pipeline = VK_NULL_HANDLE;
        }
        if (_pipelineLayout != VK_NULL_HANDLE)
        {
            vkDestroyPipelineLayout(device, _pipelineLayout, nullptr);
            _pipelineLayout = VK_NULL_HANDLE;
        }
    }
} // namespace vkm
