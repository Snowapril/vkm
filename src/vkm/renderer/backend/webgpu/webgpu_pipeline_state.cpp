// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/webgpu/webgpu_pipeline_state.h>
#include <vkm/renderer/backend/webgpu/webgpu_driver.h>
#include <vkm/renderer/backend/webgpu/webgpu_util.h>
#include <vkm/renderer/backend/common/shader_cache_loader.h>
#include <vkm/renderer/backend/common/shader_cache_util.h>

#include <filesystem>
#include <optional>
#include <vector>

namespace vkm
{
    namespace
    {
        // One transiently-created WGPUShaderModule plus the entry point recorded in its
        // .vfcache header (the source of truth for what was actually compiled, rather than
        // trusting VkmShaderStageDescriptor::entryPoint again).
        struct LoadedStageModule
        {
            WGPUShaderModule module = nullptr;
            std::string entryPoint;
        };

        std::optional<LoadedStageModule> loadStageModule(WGPUDevice device, const VkmPipelineStateDescriptor& desc,
            const VkmShaderStageDescriptor& stageDesc, VkmShaderCacheStage stage, const std::string& shaderCacheDir, std::string* outError)
        {
            const std::string shaderStem = std::filesystem::path(stageDesc.filepath).stem().string();
            const std::string cacheFilename = buildShaderCacheFilename(shaderStem, desc.optionName, stage, VkmShaderCacheBackend::WebGPU);
            const std::string cachePath = (std::filesystem::path(shaderCacheDir) / cacheFilename).string();

            std::optional<VkmLoadedShaderCache> loaded = loadShaderCacheFile(cachePath, VkmShaderCacheBackend::WebGPU, outError);
            if (!loaded.has_value())
            {
                return std::nullopt;
            }

            if (loaded->contentFormat != VkmShaderCacheContentFormat::Wgsl)
            {
                if (outError != nullptr)
                    *outError = fmt::format("Shader cache file '{}' is not WGSL content", cachePath);
                return std::nullopt;
            }

            const std::string wgslSource(reinterpret_cast<const char*>(loaded->content.data()), loaded->content.size());

            WGPUShaderSourceWGSL wgslSourceDesc{};
            wgslSourceDesc.chain.sType = WGPUSType_ShaderSourceWGSL;
            wgslSourceDesc.code = toWGPUStringView(wgslSource.c_str());

            WGPUShaderModuleDescriptor moduleDesc{};
            moduleDesc.nextInChain = &wgslSourceDesc.chain;
            moduleDesc.label = toWGPUStringView(shaderStem.c_str());

            WGPUShaderModule module = wgpuDeviceCreateShaderModule(device, &moduleDesc);
            if (module == nullptr)
            {
                if (outError != nullptr)
                    *outError = fmt::format("Failed to create WGPUShaderModule from '{}'", cachePath);
                return std::nullopt;
            }

            return LoadedStageModule{module, loaded->entryPoint};
        }

        // WGPUCullMode has no FrontAndBack equivalent (only None/Front/Back).
        bool toWGPUCullMode(VkmCullMode cullMode, WGPUCullMode* out, std::string* outError)
        {
            switch (cullMode)
            {
                case VkmCullMode::None: *out = WGPUCullMode_None; return true;
                case VkmCullMode::Front: *out = WGPUCullMode_Front; return true;
                case VkmCullMode::Back: *out = WGPUCullMode_Back; return true;
                case VkmCullMode::FrontAndBack:
                    if (outError != nullptr)
                        *outError = "VkmCullMode::FrontAndBack is not supported on the WebGPU backend (WGPUCullMode has no FrontAndBack equivalent)";
                    return false;
                default:
                    if (outError != nullptr)
                        *outError = "Invalid cull mode";
                    return false;
            }
        }

        WGPUFrontFace toWGPUFrontFace(VkmFrontFace frontFace)
        {
            switch (frontFace)
            {
                case VkmFrontFace::CounterClockwise: return WGPUFrontFace_CCW;
                case VkmFrontFace::Clockwise: return WGPUFrontFace_CW;
                default: VKM_ASSERT(false, "Invalid front face"); return WGPUFrontFace_CCW;
            }
        }

        WGPUPrimitiveTopology toWGPUPrimitiveTopology(VkmPrimitiveTopology topology)
        {
            switch (topology)
            {
                case VkmPrimitiveTopology::PointList: return WGPUPrimitiveTopology_PointList;
                case VkmPrimitiveTopology::LineList: return WGPUPrimitiveTopology_LineList;
                case VkmPrimitiveTopology::LineStrip: return WGPUPrimitiveTopology_LineStrip;
                case VkmPrimitiveTopology::TriangleList: return WGPUPrimitiveTopology_TriangleList;
                case VkmPrimitiveTopology::TriangleStrip: return WGPUPrimitiveTopology_TriangleStrip;
                default: VKM_ASSERT(false, "Invalid primitive topology"); return WGPUPrimitiveTopology_TriangleList;
            }
        }

        WGPUCompareFunction toWGPUCompareFunction(VkmCompareOp op)
        {
            switch (op)
            {
                case VkmCompareOp::Never: return WGPUCompareFunction_Never;
                case VkmCompareOp::Less: return WGPUCompareFunction_Less;
                case VkmCompareOp::Equal: return WGPUCompareFunction_Equal;
                case VkmCompareOp::LessOrEqual: return WGPUCompareFunction_LessEqual;
                case VkmCompareOp::Greater: return WGPUCompareFunction_Greater;
                case VkmCompareOp::NotEqual: return WGPUCompareFunction_NotEqual;
                case VkmCompareOp::GreaterOrEqual: return WGPUCompareFunction_GreaterEqual;
                case VkmCompareOp::Always: return WGPUCompareFunction_Always;
                default: VKM_ASSERT(false, "Invalid compare op"); return WGPUCompareFunction_Always;
            }
        }

        WGPUStencilOperation toWGPUStencilOp(VkmStencilOp op)
        {
            switch (op)
            {
                case VkmStencilOp::Keep: return WGPUStencilOperation_Keep;
                case VkmStencilOp::Zero: return WGPUStencilOperation_Zero;
                case VkmStencilOp::Replace: return WGPUStencilOperation_Replace;
                case VkmStencilOp::IncrementAndClamp: return WGPUStencilOperation_IncrementClamp;
                case VkmStencilOp::DecrementAndClamp: return WGPUStencilOperation_DecrementClamp;
                case VkmStencilOp::Invert: return WGPUStencilOperation_Invert;
                case VkmStencilOp::IncrementAndWrap: return WGPUStencilOperation_IncrementWrap;
                case VkmStencilOp::DecrementAndWrap: return WGPUStencilOperation_DecrementWrap;
                default: VKM_ASSERT(false, "Invalid stencil op"); return WGPUStencilOperation_Keep;
            }
        }

        WGPUStencilFaceState toWGPUStencilFaceState(const VkmStencilOpState& state)
        {
            WGPUStencilFaceState face{};
            face.compare = toWGPUCompareFunction(state.compareOp);
            face.failOp = toWGPUStencilOp(state.failOp);
            face.depthFailOp = toWGPUStencilOp(state.depthFailOp);
            face.passOp = toWGPUStencilOp(state.passOp);
            return face;
        }

        // WebGPU has no distinct "alpha constant" blend factor -- WGPUBlendFactor_Constant /
        // WGPUBlendFactor_OneMinusConstant apply to both color and alpha channels alike.
        WGPUBlendFactor toWGPUBlendFactor(VkmBlendFactor factor)
        {
            switch (factor)
            {
                case VkmBlendFactor::Zero: return WGPUBlendFactor_Zero;
                case VkmBlendFactor::One: return WGPUBlendFactor_One;
                case VkmBlendFactor::SrcColor: return WGPUBlendFactor_Src;
                case VkmBlendFactor::OneMinusSrcColor: return WGPUBlendFactor_OneMinusSrc;
                case VkmBlendFactor::DstColor: return WGPUBlendFactor_Dst;
                case VkmBlendFactor::OneMinusDstColor: return WGPUBlendFactor_OneMinusDst;
                case VkmBlendFactor::SrcAlpha: return WGPUBlendFactor_SrcAlpha;
                case VkmBlendFactor::OneMinusSrcAlpha: return WGPUBlendFactor_OneMinusSrcAlpha;
                case VkmBlendFactor::DstAlpha: return WGPUBlendFactor_DstAlpha;
                case VkmBlendFactor::OneMinusDstAlpha: return WGPUBlendFactor_OneMinusDstAlpha;
                case VkmBlendFactor::ConstantColor: return WGPUBlendFactor_Constant;
                case VkmBlendFactor::OneMinusConstantColor: return WGPUBlendFactor_OneMinusConstant;
                case VkmBlendFactor::ConstantAlpha: return WGPUBlendFactor_Constant;
                case VkmBlendFactor::OneMinusConstantAlpha: return WGPUBlendFactor_OneMinusConstant;
                case VkmBlendFactor::SrcAlphaSaturate: return WGPUBlendFactor_SrcAlphaSaturated;
                default: VKM_ASSERT(false, "Invalid blend factor"); return WGPUBlendFactor_One;
            }
        }

        WGPUBlendOperation toWGPUBlendOp(VkmBlendOp op)
        {
            switch (op)
            {
                case VkmBlendOp::Add: return WGPUBlendOperation_Add;
                case VkmBlendOp::Subtract: return WGPUBlendOperation_Subtract;
                case VkmBlendOp::ReverseSubtract: return WGPUBlendOperation_ReverseSubtract;
                case VkmBlendOp::Min: return WGPUBlendOperation_Min;
                case VkmBlendOp::Max: return WGPUBlendOperation_Max;
                default: VKM_ASSERT(false, "Invalid blend op"); return WGPUBlendOperation_Add;
            }
        }

        WGPUVertexFormat toWGPUVertexFormat(VkmVertexAttributeBaseType baseType, uint32_t componentCount)
        {
            switch (baseType)
            {
                case VkmVertexAttributeBaseType::Float:
                    switch (componentCount)
                    {
                        case 1: return WGPUVertexFormat_Float32;
                        case 2: return WGPUVertexFormat_Float32x2;
                        case 3: return WGPUVertexFormat_Float32x3;
                        case 4: return WGPUVertexFormat_Float32x4;
                        default: break;
                    }
                    break;
                case VkmVertexAttributeBaseType::Int:
                    switch (componentCount)
                    {
                        case 1: return WGPUVertexFormat_Sint32;
                        case 2: return WGPUVertexFormat_Sint32x2;
                        case 3: return WGPUVertexFormat_Sint32x3;
                        case 4: return WGPUVertexFormat_Sint32x4;
                        default: break;
                    }
                    break;
                case VkmVertexAttributeBaseType::UInt:
                    switch (componentCount)
                    {
                        case 1: return WGPUVertexFormat_Uint32;
                        case 2: return WGPUVertexFormat_Uint32x2;
                        case 3: return WGPUVertexFormat_Uint32x3;
                        case 4: return WGPUVertexFormat_Uint32x4;
                        default: break;
                    }
                    break;
            }
            VKM_ASSERT(false, "Unsupported vertex attribute base type/component count combination");
            return WGPUVertexFormat_Float32;
        }
    } // namespace

    VkmPipelineStateWebGPU::VkmPipelineStateWebGPU(VkmDriverBase* driver)
        : VkmPipelineStateBase(driver)
    {
    }

    VkmPipelineStateWebGPU::~VkmPipelineStateWebGPU()
    {
    }

    bool VkmPipelineStateWebGPU::createInner(const VkmPipelineStateDescriptor& desc, const std::string& shaderCacheDir, std::string* outError)
    {
        if (desc.rasterizationState.fillMode != VkmFillMode::Solid)
        {
            if (outError != nullptr)
                *outError = "WebGPU backend only supports VkmFillMode::Solid (WGPUPrimitiveState has no polygon/fill-mode concept; Wireframe/Point are unsupported)";
            return false;
        }

        WGPUCullMode cullMode = WGPUCullMode_None;
        if (!toWGPUCullMode(desc.rasterizationState.cullMode, &cullMode, outError))
        {
            return false;
        }

        VkmDriverWebGPU* driverWebGPU = static_cast<VkmDriverWebGPU*>(_driver);
        WGPUDevice device = driverWebGPU->getDevice();

        // Minimal empty pipeline layout -- descriptor sets/bind groups are a deliberately
        // deferred follow-up (see pipeline_state_object.h).
        WGPUPipelineLayoutDescriptor pipelineLayoutDesc{};
        pipelineLayoutDesc.label = toWGPUStringView("VkmPipelineStateWebGPU EmptyLayout");
        pipelineLayoutDesc.bindGroupLayoutCount = 0;
        pipelineLayoutDesc.bindGroupLayouts = nullptr;

        WGPUPipelineLayout pipelineLayout = wgpuDeviceCreatePipelineLayout(device, &pipelineLayoutDesc);
        if (pipelineLayout == nullptr)
        {
            if (outError != nullptr)
                *outError = "Failed to create empty WebGPU pipeline layout";
            return false;
        }

        if (desc.computeShader.has_value())
        {
            std::optional<LoadedStageModule> computeModule =
                loadStageModule(device, desc, *desc.computeShader, VkmShaderCacheStage::Compute, shaderCacheDir, outError);
            if (!computeModule.has_value())
            {
                wgpuPipelineLayoutRelease(pipelineLayout);
                return false;
            }

            WGPUComputePipelineDescriptor computePipelineDesc{};
            computePipelineDesc.label = toWGPUStringView(desc.name.c_str());
            computePipelineDesc.layout = pipelineLayout;
            computePipelineDesc.compute.module = computeModule->module;
            computePipelineDesc.compute.entryPoint = toWGPUStringView(computeModule->entryPoint.c_str());

            _computePipeline = wgpuDeviceCreateComputePipeline(device, &computePipelineDesc);

            wgpuShaderModuleRelease(computeModule->module);
            wgpuPipelineLayoutRelease(pipelineLayout);

            if (_computePipeline == nullptr)
            {
                if (outError != nullptr)
                    *outError = "Failed to create WebGPU compute pipeline";
                return false;
            }

            return true;
        }

        // Graphics pipeline path.
        if (!desc.vertexShader.has_value())
        {
            wgpuPipelineLayoutRelease(pipelineLayout);
            if (outError != nullptr)
                *outError = "Graphics pipeline descriptor is missing a vertex shader";
            return false;
        }

        std::optional<LoadedStageModule> vertexModule =
            loadStageModule(device, desc, *desc.vertexShader, VkmShaderCacheStage::Vertex, shaderCacheDir, outError);
        if (!vertexModule.has_value())
        {
            wgpuPipelineLayoutRelease(pipelineLayout);
            return false;
        }

        std::optional<LoadedStageModule> fragmentModule;
        if (desc.fragmentShader.has_value())
        {
            fragmentModule = loadStageModule(device, desc, *desc.fragmentShader, VkmShaderCacheStage::Fragment, shaderCacheDir, outError);
            if (!fragmentModule.has_value())
            {
                wgpuShaderModuleRelease(vertexModule->module);
                wgpuPipelineLayoutRelease(pipelineLayout);
                return false;
            }
        }

        // Vertex buffer layout(s): perVertex/perInstance are independently optional.
        std::vector<std::vector<WGPUVertexAttribute>> attributeStorage;
        attributeStorage.reserve(2);
        std::vector<WGPUVertexBufferLayout> vertexBufferLayouts;
        vertexBufferLayouts.reserve(2);

        auto addLayoutPart = [&attributeStorage, &vertexBufferLayouts](const VkmVertexInputLayoutPart& part, WGPUVertexStepMode stepMode)
        {
            std::vector<WGPUVertexAttribute> attributes;
            attributes.reserve(part.attributes.size());
            for (const VkmVertexAttributeDescriptor& attribute : part.attributes)
            {
                WGPUVertexAttribute vertexAttribute{};
                vertexAttribute.format = toWGPUVertexFormat(attribute.baseType, attribute.componentCount);
                vertexAttribute.offset = attribute.offset;
                vertexAttribute.shaderLocation = attribute.location;
                attributes.push_back(vertexAttribute);
            }
            attributeStorage.push_back(std::move(attributes));

            WGPUVertexBufferLayout bufferLayout{};
            bufferLayout.stepMode = stepMode;
            bufferLayout.arrayStride = part.stride;
            bufferLayout.attributeCount = attributeStorage.back().size();
            bufferLayout.attributes = attributeStorage.back().data();
            vertexBufferLayouts.push_back(bufferLayout);
        };

        if (desc.vertexInputLayout.perVertex.has_value())
            addLayoutPart(*desc.vertexInputLayout.perVertex, WGPUVertexStepMode_Vertex);
        if (desc.vertexInputLayout.perInstance.has_value())
            addLayoutPart(*desc.vertexInputLayout.perInstance, WGPUVertexStepMode_Instance);

        // Color targets / blend state -- blendStates is pre-reserved so pointers into it
        // (captured below by colorTargets) stay valid for the rest of this function.
        std::vector<WGPUBlendState> blendStates;
        blendStates.reserve(desc.colorAttachments.size());
        std::vector<WGPUColorTargetState> colorTargets;
        colorTargets.reserve(desc.colorAttachments.size());
        for (const VkmColorBlendAttachmentState& attachment : desc.colorAttachments)
        {
            WGPUColorTargetState target{};
            target.format = toWGPUTextureFormat(attachment.format);
            target.writeMask = WGPUColorWriteMask_All;
            if (attachment.blendEnable)
            {
                WGPUBlendState blendState{};
                blendState.color.operation = toWGPUBlendOp(attachment.colorBlendOp);
                blendState.color.srcFactor = toWGPUBlendFactor(attachment.srcColorBlendFactor);
                blendState.color.dstFactor = toWGPUBlendFactor(attachment.dstColorBlendFactor);
                blendState.alpha.operation = toWGPUBlendOp(attachment.alphaBlendOp);
                blendState.alpha.srcFactor = toWGPUBlendFactor(attachment.srcAlphaBlendFactor);
                blendState.alpha.dstFactor = toWGPUBlendFactor(attachment.dstAlphaBlendFactor);
                blendStates.push_back(blendState);
                target.blend = &blendStates.back();
            }
            colorTargets.push_back(target);
        }

        WGPUFragmentState fragmentState{};
        if (fragmentModule.has_value())
        {
            fragmentState.module = fragmentModule->module;
            fragmentState.entryPoint = toWGPUStringView(fragmentModule->entryPoint.c_str());
            fragmentState.targetCount = colorTargets.size();
            fragmentState.targets = colorTargets.data();
        }

        // Depth-stencil.
        WGPUDepthStencilState depthStencilState{};
        const bool hasDepthStencil = desc.depthStencilState.depthStencilFormat != VkmFormat::Undefined;
        if (hasDepthStencil)
        {
            depthStencilState.format = toWGPUTextureFormat(desc.depthStencilState.depthStencilFormat);
            depthStencilState.depthWriteEnabled = desc.depthStencilState.depthWriteEnable ? WGPUOptionalBool_True : WGPUOptionalBool_False;
            depthStencilState.depthCompare = desc.depthStencilState.depthTestEnable
                ? toWGPUCompareFunction(desc.depthStencilState.depthCompareOp)
                : WGPUCompareFunction_Always;

            if (desc.depthStencilState.stencilTestEnable)
            {
                depthStencilState.stencilFront = toWGPUStencilFaceState(desc.depthStencilState.front);
                depthStencilState.stencilBack = toWGPUStencilFaceState(desc.depthStencilState.back);
                // WebGPU has a single read/write mask pair shared by both faces (unlike
                // Vulkan/Metal's independent front/back masks) -- front's masks win.
                depthStencilState.stencilReadMask = desc.depthStencilState.front.compareMask;
                depthStencilState.stencilWriteMask = desc.depthStencilState.front.writeMask;
            }
            else
            {
                WGPUStencilFaceState noopFace{};
                noopFace.compare = WGPUCompareFunction_Always;
                noopFace.failOp = WGPUStencilOperation_Keep;
                noopFace.depthFailOp = WGPUStencilOperation_Keep;
                noopFace.passOp = WGPUStencilOperation_Keep;
                depthStencilState.stencilFront = noopFace;
                depthStencilState.stencilBack = noopFace;
                depthStencilState.stencilReadMask = 0xFFFFFFFFu;
                depthStencilState.stencilWriteMask = 0xFFFFFFFFu;
            }
        }

        WGPUVertexState vertexState{};
        vertexState.module = vertexModule->module;
        vertexState.entryPoint = toWGPUStringView(vertexModule->entryPoint.c_str());
        vertexState.bufferCount = vertexBufferLayouts.size();
        vertexState.buffers = vertexBufferLayouts.data();

        WGPUPrimitiveState primitiveState{};
        primitiveState.topology = toWGPUPrimitiveTopology(desc.primitiveTopology);
        primitiveState.stripIndexFormat = WGPUIndexFormat_Undefined;
        primitiveState.frontFace = toWGPUFrontFace(desc.rasterizationState.frontFace);
        primitiveState.cullMode = cullMode;

        WGPUMultisampleState multisampleState{};
        multisampleState.count = 1;
        multisampleState.mask = 0xFFFFFFFFu;
        multisampleState.alphaToCoverageEnabled = false;

        WGPURenderPipelineDescriptor renderPipelineDesc{};
        renderPipelineDesc.label = toWGPUStringView(desc.name.c_str());
        renderPipelineDesc.layout = pipelineLayout;
        renderPipelineDesc.vertex = vertexState;
        renderPipelineDesc.primitive = primitiveState;
        renderPipelineDesc.depthStencil = hasDepthStencil ? &depthStencilState : nullptr;
        renderPipelineDesc.multisample = multisampleState;
        renderPipelineDesc.fragment = fragmentModule.has_value() ? &fragmentState : nullptr;

        _renderPipeline = wgpuDeviceCreateRenderPipeline(device, &renderPipelineDesc);

        wgpuShaderModuleRelease(vertexModule->module);
        if (fragmentModule.has_value())
        {
            wgpuShaderModuleRelease(fragmentModule->module);
        }
        wgpuPipelineLayoutRelease(pipelineLayout);

        if (_renderPipeline == nullptr)
        {
            if (outError != nullptr)
                *outError = "Failed to create WebGPU render pipeline";
            return false;
        }

        return true;
    }

    void VkmPipelineStateWebGPU::destroyInner()
    {
        if (_renderPipeline != nullptr)
        {
            wgpuRenderPipelineRelease(_renderPipeline);
            _renderPipeline = nullptr;
        }
        if (_computePipeline != nullptr)
        {
            wgpuComputePipelineRelease(_computePipeline);
            _computePipeline = nullptr;
        }
    }
} // namespace vkm
