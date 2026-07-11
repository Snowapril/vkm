// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_pipeline_state.h>
#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/common/shader_cache_loader.h>
#include <vkm/renderer/backend/common/shader_cache_util.h>

#import <Metal/MTLDevice.h>
#import <Metal/MTLLibrary.h>
#import <Metal/MTLVertexDescriptor.h>
#import <Metal/MTLDepthStencil.h>
#import <Metal/MTLRenderPipeline.h>
#import <Metal/MTLPixelFormat.h>
#import <Metal/MTL4Compiler.h>
#import <Metal/MTL4LibraryDescriptor.h>
#import <Metal/MTL4FunctionDescriptor.h>
#import <Metal/MTL4LibraryFunctionDescriptor.h>
#import <Metal/MTL4RenderPipeline.h>
#import <Metal/MTL4ComputePipeline.h>
#import <Metal/MTL4PipelineState.h>

#include <filesystem>

namespace vkm
{
    namespace
    {
        // NOTE: duplicated from metal_texture.mm's anonymous-namespace helper of the
        // same name -- no shared metal-format-conversion header exists yet in this
        // codebase, and adding one is out of scope for this pipeline-state-only change.
        MTLPixelFormat getMTLPixelFormat(VkmFormat format)
        {
            switch (format)
            {
                case VkmFormat::R8G8B8A8_UNORM:      return MTLPixelFormatRGBA8Unorm;
                case VkmFormat::R8G8B8A8_SRGB:       return MTLPixelFormatRGBA8Unorm_sRGB;
                case VkmFormat::R8G8B8A8_UINT:       return MTLPixelFormatRGBA8Uint;
                case VkmFormat::R8G8B8A8_SNORM:      return MTLPixelFormatRGBA8Snorm;
                case VkmFormat::R8G8B8A8_SINT:       return MTLPixelFormatRGBA8Sint;
                case VkmFormat::R16G16B16A16_UNORM:  return MTLPixelFormatRGBA16Unorm;
                case VkmFormat::R16G16B16A16_SFLOAT: return MTLPixelFormatRGBA16Float;
                case VkmFormat::R32G32B32A32_SFLOAT: return MTLPixelFormatRGBA32Float;
                case VkmFormat::D32_SFLOAT:          return MTLPixelFormatDepth32Float;
                case VkmFormat::D24_UNORM_S8_UINT:   return MTLPixelFormatDepth24Unorm_Stencil8;
                case VkmFormat::D32_SFLOAT_S8_UINT:  return MTLPixelFormatDepth32Float_Stencil8;
                case VkmFormat::BGRA8_UNORM:         return MTLPixelFormatBGRA8Unorm;
                case VkmFormat::BGRA8_SRGB:          return MTLPixelFormatBGRA8Unorm_sRGB;
                case VkmFormat::Undefined:
                default:                             return MTLPixelFormatInvalid;
            }
        }

        MTLPrimitiveTopologyClass toMTLPrimitiveTopologyClass(VkmPrimitiveTopology topology)
        {
            switch (topology)
            {
                case VkmPrimitiveTopology::PointList:
                    return MTLPrimitiveTopologyClassPoint;
                case VkmPrimitiveTopology::LineList:
                case VkmPrimitiveTopology::LineStrip:
                    return MTLPrimitiveTopologyClassLine;
                case VkmPrimitiveTopology::TriangleList:
                case VkmPrimitiveTopology::TriangleStrip:
                    return MTLPrimitiveTopologyClassTriangle;
                default:
                    return MTLPrimitiveTopologyClassUnspecified;
            }
        }

        MTLVertexFormat toMTLVertexFormat(VkmVertexAttributeBaseType baseType, uint32_t componentCount)
        {
            switch (baseType)
            {
                case VkmVertexAttributeBaseType::Float:
                    switch (componentCount)
                    {
                        case 1: return MTLVertexFormatFloat;
                        case 2: return MTLVertexFormatFloat2;
                        case 3: return MTLVertexFormatFloat3;
                        case 4: return MTLVertexFormatFloat4;
                        default: break;
                    }
                    break;
                case VkmVertexAttributeBaseType::Int:
                    switch (componentCount)
                    {
                        case 1: return MTLVertexFormatInt;
                        case 2: return MTLVertexFormatInt2;
                        case 3: return MTLVertexFormatInt3;
                        case 4: return MTLVertexFormatInt4;
                        default: break;
                    }
                    break;
                case VkmVertexAttributeBaseType::UInt:
                    switch (componentCount)
                    {
                        case 1: return MTLVertexFormatUInt;
                        case 2: return MTLVertexFormatUInt2;
                        case 3: return MTLVertexFormatUInt3;
                        case 4: return MTLVertexFormatUInt4;
                        default: break;
                    }
                    break;
            }
            return MTLVertexFormatInvalid;
        }

        MTLBlendFactor toMTLBlendFactor(VkmBlendFactor factor)
        {
            switch (factor)
            {
                case VkmBlendFactor::Zero:                  return MTLBlendFactorZero;
                case VkmBlendFactor::One:                   return MTLBlendFactorOne;
                case VkmBlendFactor::SrcColor:               return MTLBlendFactorSourceColor;
                case VkmBlendFactor::OneMinusSrcColor:       return MTLBlendFactorOneMinusSourceColor;
                case VkmBlendFactor::DstColor:               return MTLBlendFactorDestinationColor;
                case VkmBlendFactor::OneMinusDstColor:       return MTLBlendFactorOneMinusDestinationColor;
                case VkmBlendFactor::SrcAlpha:               return MTLBlendFactorSourceAlpha;
                case VkmBlendFactor::OneMinusSrcAlpha:       return MTLBlendFactorOneMinusSourceAlpha;
                case VkmBlendFactor::DstAlpha:               return MTLBlendFactorDestinationAlpha;
                case VkmBlendFactor::OneMinusDstAlpha:       return MTLBlendFactorOneMinusDestinationAlpha;
                case VkmBlendFactor::ConstantColor:          return MTLBlendFactorBlendColor;
                case VkmBlendFactor::OneMinusConstantColor:  return MTLBlendFactorOneMinusBlendColor;
                case VkmBlendFactor::ConstantAlpha:          return MTLBlendFactorBlendAlpha;
                case VkmBlendFactor::OneMinusConstantAlpha:  return MTLBlendFactorOneMinusBlendAlpha;
                case VkmBlendFactor::SrcAlphaSaturate:       return MTLBlendFactorSourceAlphaSaturated;
                default:                                     return MTLBlendFactorOne;
            }
        }

        MTLBlendOperation toMTLBlendOperation(VkmBlendOp op)
        {
            switch (op)
            {
                case VkmBlendOp::Add:              return MTLBlendOperationAdd;
                case VkmBlendOp::Subtract:          return MTLBlendOperationSubtract;
                case VkmBlendOp::ReverseSubtract:   return MTLBlendOperationReverseSubtract;
                case VkmBlendOp::Min:               return MTLBlendOperationMin;
                case VkmBlendOp::Max:               return MTLBlendOperationMax;
                default:                            return MTLBlendOperationAdd;
            }
        }

        MTLCompareFunction toMTLCompareFunction(VkmCompareOp op)
        {
            switch (op)
            {
                case VkmCompareOp::Never:          return MTLCompareFunctionNever;
                case VkmCompareOp::Less:            return MTLCompareFunctionLess;
                case VkmCompareOp::Equal:           return MTLCompareFunctionEqual;
                case VkmCompareOp::LessOrEqual:      return MTLCompareFunctionLessEqual;
                case VkmCompareOp::Greater:          return MTLCompareFunctionGreater;
                case VkmCompareOp::NotEqual:         return MTLCompareFunctionNotEqual;
                case VkmCompareOp::GreaterOrEqual:   return MTLCompareFunctionGreaterEqual;
                case VkmCompareOp::Always:           return MTLCompareFunctionAlways;
                default:                             return MTLCompareFunctionAlways;
            }
        }

        MTLStencilOperation toMTLStencilOperation(VkmStencilOp op)
        {
            switch (op)
            {
                case VkmStencilOp::Keep:               return MTLStencilOperationKeep;
                case VkmStencilOp::Zero:                return MTLStencilOperationZero;
                case VkmStencilOp::Replace:             return MTLStencilOperationReplace;
                case VkmStencilOp::IncrementAndClamp:    return MTLStencilOperationIncrementClamp;
                case VkmStencilOp::DecrementAndClamp:    return MTLStencilOperationDecrementClamp;
                case VkmStencilOp::Invert:              return MTLStencilOperationInvert;
                case VkmStencilOp::IncrementAndWrap:     return MTLStencilOperationIncrementWrap;
                case VkmStencilOp::DecrementAndWrap:     return MTLStencilOperationDecrementWrap;
                default:                                return MTLStencilOperationKeep;
            }
        }

        MTLStencilDescriptor* toMTLStencilDescriptor(const VkmStencilOpState& state)
        {
            MTLStencilDescriptor* stencilDescriptor = [[MTLStencilDescriptor alloc] init];
            stencilDescriptor.stencilCompareFunction = toMTLCompareFunction(state.compareOp);
            stencilDescriptor.stencilFailureOperation = toMTLStencilOperation(state.failOp);
            stencilDescriptor.depthFailureOperation = toMTLStencilOperation(state.depthFailOp);
            stencilDescriptor.depthStencilPassOperation = toMTLStencilOperation(state.passOp);
            stencilDescriptor.readMask = state.compareMask;
            stencilDescriptor.writeMask = state.writeMask;
            return stencilDescriptor;
        }

        // Loads one stage's .vfcache file, compiles it into a transient id<MTLLibrary>
        // via the Metal4 compiler, and returns a MTL4LibraryFunctionDescriptor
        // referencing its entry point. Returns nil (+ *outError) on any failure.
        MTL4LibraryFunctionDescriptor* loadStageLibraryFunction(id<MTL4Compiler> compiler,
            const VkmShaderStageDescriptor& stageDesc, const std::string& shaderCacheDir,
            const std::string& optionName, VkmShaderCacheStage stage, std::string* outError)
        {
            const std::string shaderStem = std::filesystem::path(stageDesc.filepath).stem().string();
            const std::string filename = buildShaderCacheFilename(shaderStem, optionName, stage, VkmShaderCacheBackend::Metal);
            const std::string fullPath = (std::filesystem::path(shaderCacheDir) / filename).string();

            std::string loadError;
            std::optional<VkmLoadedShaderCache> loaded = loadShaderCacheFile(fullPath, VkmShaderCacheBackend::Metal, &loadError);
            if (!loaded.has_value())
            {
                *outError = "Failed to load shader cache '" + fullPath + "': " + loadError;
                return nil;
            }

            if (loaded->contentFormat != VkmShaderCacheContentFormat::Msl)
            {
                *outError = "Shader cache '" + fullPath + "' is not MSL content";
                return nil;
            }

            NSString* mslSource = [[NSString alloc] initWithBytes:loaded->content.data()
                                                             length:loaded->content.size()
                                                           encoding:NSUTF8StringEncoding];
            if (mslSource == nil)
            {
                *outError = "Failed to decode MSL source as UTF-8 for '" + fullPath + "'";
                return nil;
            }

            MTL4LibraryDescriptor* libraryDescriptor = [[MTL4LibraryDescriptor alloc] init];
            libraryDescriptor.source = mslSource;

            NSError* nsError = nil;
            id<MTLLibrary> library = [compiler newLibraryWithDescriptor:libraryDescriptor error:&nsError];
            if (library == nil)
            {
                std::string reason = nsError != nil ? std::string(nsError.localizedDescription.UTF8String) : std::string("unknown error");
                *outError = "Failed to compile MSL library for '" + fullPath + "': " + reason;
                return nil;
            }

            MTL4LibraryFunctionDescriptor* functionDescriptor = [[MTL4LibraryFunctionDescriptor alloc] init];
            functionDescriptor.library = library;
            functionDescriptor.name = [NSString stringWithUTF8String:loaded->entryPoint.c_str()];
            return functionDescriptor;
        }
    }

    VkmPipelineStateMetal::VkmPipelineStateMetal(VkmDriverBase* driver)
        : VkmPipelineStateBase(driver)
    {
    }

    VkmPipelineStateMetal::~VkmPipelineStateMetal()
    {
    }

    bool VkmPipelineStateMetal::createInner(const VkmPipelineStateDescriptor& desc, const std::string& shaderCacheDir, std::string* outError)
    {
        auto setError = [outError](const std::string& message)
        {
            if (outError != nullptr)
            {
                *outError = message;
            }
        };

        // NOTE(pipeline-state): Point fill mode / FrontAndBack cull mode have no
        // MTLTriangleFillMode / MTLCullMode equivalent (see pipeline_state.h).
        if (desc.rasterizationState.fillMode == VkmFillMode::Point)
        {
            setError("Metal does not support VkmFillMode::Point (MTLTriangleFillMode has no point mode)");
            return false;
        }
        if (desc.rasterizationState.cullMode == VkmCullMode::FrontAndBack)
        {
            setError("Metal does not support VkmCullMode::FrontAndBack (MTLCullMode has no combined front+back mode)");
            return false;
        }

        id<MTLDevice> device = static_cast<VkmDriverMetal*>(_driver)->getMTLDevice();

        MTL4CompilerDescriptor* compilerDescriptor = [[MTL4CompilerDescriptor alloc] init];
        NSError* nsError = nil;
        id<MTL4Compiler> compiler = [device newCompilerWithDescriptor:compilerDescriptor error:&nsError];
        if (compiler == nil)
        {
            std::string reason = nsError != nil ? std::string(nsError.localizedDescription.UTF8String) : std::string("unknown error");
            setError("Failed to create MTL4Compiler: " + reason);
            return false;
        }

        if (desc.computeShader.has_value())
        {
            std::string loadError;
            MTL4LibraryFunctionDescriptor* computeFunctionDescriptor = loadStageLibraryFunction(
                compiler, desc.computeShader.value(), shaderCacheDir, desc.optionName, VkmShaderCacheStage::Compute, &loadError);
            if (computeFunctionDescriptor == nil)
            {
                setError(loadError);
                return false;
            }

            MTL4ComputePipelineDescriptor* computePipelineDescriptor = [[MTL4ComputePipelineDescriptor alloc] init];
            computePipelineDescriptor.computeFunctionDescriptor = computeFunctionDescriptor;

            NSError* pipelineError = nil;
            _computePipelineState = [compiler newComputePipelineStateWithDescriptor:computePipelineDescriptor
                                                                 compilerTaskOptions:nil
                                                                               error:&pipelineError];
            if (_computePipelineState == nil)
            {
                std::string reason = pipelineError != nil ? std::string(pipelineError.localizedDescription.UTF8String) : std::string("unknown error");
                setError("Failed to create compute pipeline state: " + reason);
                return false;
            }

            return true;
        }

        if (!desc.vertexShader.has_value())
        {
            setError("Graphics pipeline requires a vertex shader");
            return false;
        }

        std::string loadError;
        MTL4LibraryFunctionDescriptor* vertexFunctionDescriptor = loadStageLibraryFunction(
            compiler, desc.vertexShader.value(), shaderCacheDir, desc.optionName, VkmShaderCacheStage::Vertex, &loadError);
        if (vertexFunctionDescriptor == nil)
        {
            setError(loadError);
            return false;
        }

        MTL4LibraryFunctionDescriptor* fragmentFunctionDescriptor = nil;
        if (desc.fragmentShader.has_value())
        {
            fragmentFunctionDescriptor = loadStageLibraryFunction(
                compiler, desc.fragmentShader.value(), shaderCacheDir, desc.optionName, VkmShaderCacheStage::Fragment, &loadError);
            if (fragmentFunctionDescriptor == nil)
            {
                setError(loadError);
                return false;
            }
        }

        MTL4RenderPipelineDescriptor* renderPipelineDescriptor = [[MTL4RenderPipelineDescriptor alloc] init];
        renderPipelineDescriptor.vertexFunctionDescriptor = vertexFunctionDescriptor;
        if (fragmentFunctionDescriptor != nil)
        {
            renderPipelineDescriptor.fragmentFunctionDescriptor = fragmentFunctionDescriptor;
        }
        else
        {
            renderPipelineDescriptor.rasterizationEnabled = NO;
        }

        renderPipelineDescriptor.inputPrimitiveTopology = toMTLPrimitiveTopologyClass(desc.primitiveTopology);

        for (size_t i = 0; i < desc.colorAttachments.size(); ++i)
        {
            const VkmColorBlendAttachmentState& attachment = desc.colorAttachments[i];
            renderPipelineDescriptor.colorAttachments[i].pixelFormat = getMTLPixelFormat(attachment.format);
            renderPipelineDescriptor.colorAttachments[i].blendingState = attachment.blendEnable ? MTL4BlendStateEnabled : MTL4BlendStateDisabled;
            renderPipelineDescriptor.colorAttachments[i].sourceRGBBlendFactor = toMTLBlendFactor(attachment.srcColorBlendFactor);
            renderPipelineDescriptor.colorAttachments[i].destinationRGBBlendFactor = toMTLBlendFactor(attachment.dstColorBlendFactor);
            renderPipelineDescriptor.colorAttachments[i].rgbBlendOperation = toMTLBlendOperation(attachment.colorBlendOp);
            renderPipelineDescriptor.colorAttachments[i].sourceAlphaBlendFactor = toMTLBlendFactor(attachment.srcAlphaBlendFactor);
            renderPipelineDescriptor.colorAttachments[i].destinationAlphaBlendFactor = toMTLBlendFactor(attachment.dstAlphaBlendFactor);
            renderPipelineDescriptor.colorAttachments[i].alphaBlendOperation = toMTLBlendOperation(attachment.alphaBlendOp);
        }

        // NOTE(pipeline-state): MTL4RenderPipelineDescriptor has no
        // depthAttachmentPixelFormat/stencilAttachmentPixelFormat (unlike the classic
        // MTLRenderPipelineDescriptor) -- confirmed against the macOS 26 SDK headers.
        // Depth/stencil attachment formats are therefore only known at encoder/render-
        // pass time (MTL4RenderPassDescriptor), not at pipeline-creation time.

        const VkmVertexInputLayoutDescriptor& vertexLayout = desc.vertexInputLayout;
        if (vertexLayout.perVertex.has_value() || vertexLayout.perInstance.has_value())
        {
            constexpr NSUInteger kPerVertexBufferIndex = 0;
            constexpr NSUInteger kPerInstanceBufferIndex = 1;

            MTLVertexDescriptor* vertexDescriptor = [MTLVertexDescriptor vertexDescriptor];
            uint32_t attributeIndexBase = 0;

            // NOTE(pipeline-state): VkmVertexInputLayoutPart::attributes[].location is
            // sequential *within its own part*, restarting at 0 for perInstance (see
            // pipeline_state_parser.cpp's parseVertexInputLayoutPart). Metal's vertex
            // descriptor attribute array is shared across both buffers, so perInstance
            // locations are shifted by perVertex's attribute count here to avoid index
            // collisions between the two parts.
            if (vertexLayout.perVertex.has_value())
            {
                const VkmVertexInputLayoutPart& part = vertexLayout.perVertex.value();
                for (const VkmVertexAttributeDescriptor& attribute : part.attributes)
                {
                    const NSUInteger attributeIndex = attributeIndexBase + attribute.location;
                    vertexDescriptor.attributes[attributeIndex].format = toMTLVertexFormat(attribute.baseType, attribute.componentCount);
                    vertexDescriptor.attributes[attributeIndex].offset = attribute.offset;
                    vertexDescriptor.attributes[attributeIndex].bufferIndex = kPerVertexBufferIndex;
                }
                vertexDescriptor.layouts[kPerVertexBufferIndex].stride = part.stride;
                vertexDescriptor.layouts[kPerVertexBufferIndex].stepFunction = MTLVertexStepFunctionPerVertex;
                attributeIndexBase += static_cast<uint32_t>(part.attributes.size());
            }

            if (vertexLayout.perInstance.has_value())
            {
                const VkmVertexInputLayoutPart& part = vertexLayout.perInstance.value();
                for (const VkmVertexAttributeDescriptor& attribute : part.attributes)
                {
                    const NSUInteger attributeIndex = attributeIndexBase + attribute.location;
                    vertexDescriptor.attributes[attributeIndex].format = toMTLVertexFormat(attribute.baseType, attribute.componentCount);
                    vertexDescriptor.attributes[attributeIndex].offset = attribute.offset;
                    vertexDescriptor.attributes[attributeIndex].bufferIndex = kPerInstanceBufferIndex;
                }
                vertexDescriptor.layouts[kPerInstanceBufferIndex].stride = part.stride;
                vertexDescriptor.layouts[kPerInstanceBufferIndex].stepFunction = MTLVertexStepFunctionPerInstance;
            }

            renderPipelineDescriptor.vertexDescriptor = vertexDescriptor;
        }

        NSError* pipelineError = nil;
        _renderPipelineState = [compiler newRenderPipelineStateWithDescriptor:renderPipelineDescriptor
                                                            compilerTaskOptions:nil
                                                                          error:&pipelineError];
        if (_renderPipelineState == nil)
        {
            std::string reason = pipelineError != nil ? std::string(pipelineError.localizedDescription.UTF8String) : std::string("unknown error");
            setError("Failed to create render pipeline state: " + reason);
            return false;
        }

        // Depth/stencil test state is dynamic encoder state in Metal (both classic and
        // Metal4), not part of the pipeline descriptor -- build it here so a future
        // draw-call recording phase can bind it without recomputing it per-frame (see
        // getDepthStencilState()).
        MTLDepthStencilDescriptor* depthStencilDescriptor = [[MTLDepthStencilDescriptor alloc] init];
        depthStencilDescriptor.depthCompareFunction = desc.depthStencilState.depthTestEnable
            ? toMTLCompareFunction(desc.depthStencilState.depthCompareOp)
            : MTLCompareFunctionAlways;
        depthStencilDescriptor.depthWriteEnabled = desc.depthStencilState.depthWriteEnable;
        if (desc.depthStencilState.stencilTestEnable)
        {
            depthStencilDescriptor.frontFaceStencil = toMTLStencilDescriptor(desc.depthStencilState.front);
            depthStencilDescriptor.backFaceStencil = toMTLStencilDescriptor(desc.depthStencilState.back);
        }

        _depthStencilState = [device newDepthStencilStateWithDescriptor:depthStencilDescriptor];
        if (_depthStencilState == nil)
        {
            setError("Failed to create MTLDepthStencilState");
            return false;
        }

        return true;
    }

    void VkmPipelineStateMetal::destroyInner()
    {
        _renderPipelineState = nil;
        _computePipelineState = nil;
        _depthStencilState = nil;
    }
} // namespace vkm
