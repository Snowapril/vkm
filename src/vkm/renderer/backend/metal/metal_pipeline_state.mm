// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/metal/metal_pipeline_state.h>
#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/metal/metal_util.h>
#include <vkm/renderer/backend/common/shader_cache_loader.h>
#include <vkm/renderer/backend/common/shader_cache_util.h>
#include <vkm/base/global_variable.h>

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
        // Selects how a Metal pipeline state is built: true (default) uses the Metal 4 path
        // (MTL4Compiler + MTL4RenderPipelineDescriptor); false uses the classic Metal 3 path
        // ([device newRenderPipelineStateWithDescriptor:]). Both produce id<MTLRenderPipelineState>
        // / id<MTLComputePipelineState>, which the MTL4 command encoder binds either way, so only
        // creation forks -- not the command-recording path. Override: --gv_metal4_pso=0.
        VKM_GLOBAL_VARIABLE(bool, gv_metal4_pso, true);

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

        // Loads one stage's .vfcache file into a transient id<MTLLibrary> (+ *outEntryPoint).
        // Returns nil (+ *outError) on any failure.
        //
        // MetalLib content (the default vkm-compiler output) is loaded as a precompiled binary
        // via -[MTLDevice newLibraryWithData:], which -- unlike compiling Msl source -- serializes
        // into Xcode GPU captures so a .gputrace replays without a source recompile. Msl source is
        // a fallback for caches built without a Metal toolchain: the Metal 4 path compiles it via
        // the MTL4 compiler, the Metal 3 path via [device newLibraryWithSource:].
        id<MTLLibrary> loadStageLibrary(id<MTLDevice> device, id<MTL4Compiler> compiler, bool useMetal4,
            const VkmShaderStageDescriptor& stageDesc, const std::string& shaderCacheDir,
            const std::string& optionName, VkmShaderCacheStage stage,
            std::string* outEntryPoint, std::string* outError)
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

            id<MTLLibrary> library = nil;
            if (loaded->contentFormat == VkmShaderCacheContentFormat::MetalLib)
            {
                dispatch_data_t libraryData = dispatch_data_create(loaded->content.data(),
                    loaded->content.size(), dispatch_get_main_queue(),
                    DISPATCH_DATA_DESTRUCTOR_DEFAULT);
                NSError* nsError = nil;
                library = [device newLibraryWithData:libraryData error:&nsError];
                if (library == nil)
                {
                    std::string reason = nsError != nil ? std::string(nsError.localizedDescription.UTF8String) : std::string("unknown error");
                    *outError = "Failed to load metallib for '" + fullPath + "': " + reason;
                    return nil;
                }
            }
            else if (loaded->contentFormat == VkmShaderCacheContentFormat::Msl)
            {
                NSString* mslSource = [[NSString alloc] initWithBytes:loaded->content.data()
                                                                 length:loaded->content.size()
                                                               encoding:NSUTF8StringEncoding];
                if (mslSource == nil)
                {
                    *outError = "Failed to decode MSL source as UTF-8 for '" + fullPath + "'";
                    return nil;
                }

                NSError* nsError = nil;
                if (useMetal4)
                {
                    MTL4LibraryDescriptor* libraryDescriptor = [[MTL4LibraryDescriptor alloc] init];
                    libraryDescriptor.source = mslSource;
                    library = [compiler newLibraryWithDescriptor:libraryDescriptor error:&nsError];
                }
                else
                {
                    library = [device newLibraryWithSource:mslSource options:nil error:&nsError];
                }
                if (library == nil)
                {
                    std::string reason = nsError != nil ? std::string(nsError.localizedDescription.UTF8String) : std::string("unknown error");
                    *outError = "Failed to compile MSL library for '" + fullPath + "': " + reason;
                    return nil;
                }
            }
            else
            {
                *outError = "Shader cache '" + fullPath + "' is neither metallib nor MSL content";
                return nil;
            }

            *outEntryPoint = loaded->entryPoint;
            return library;
        }

        // Wraps a loaded library + entry point as a Metal 4 function descriptor.
        MTL4LibraryFunctionDescriptor* makeMTL4FunctionDescriptor(id<MTLLibrary> library, const std::string& entryPoint)
        {
            MTL4LibraryFunctionDescriptor* functionDescriptor = [[MTL4LibraryFunctionDescriptor alloc] init];
            functionDescriptor.library = library;
            functionDescriptor.name = [NSString stringWithUTF8String:entryPoint.c_str()];
            return functionDescriptor;
        }

        // Builds the shared MTLVertexDescriptor from the pipeline's vertex input layout, or nil
        // when the pipeline declares no vertex input. Identical for both PSO paths (MTLVertexDescriptor
        // is classic Metal and both descriptor types accept it via their .vertexDescriptor property).
        MTLVertexDescriptor* buildVertexDescriptor(const VkmPipelineStateDescriptor& desc)
        {
            const VkmVertexInputLayoutDescriptor& vertexLayout = desc.vertexInputLayout;
            if (!vertexLayout.perVertex.has_value() && !vertexLayout.perInstance.has_value())
            {
                return nil;
            }

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

            return vertexDescriptor;
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
        const bool useMetal4 = gv_metal4_pso.get();

        // The MTL4 compiler is only needed by the Metal 4 path (and its MSL fallback); the
        // Metal 3 path builds pipeline states directly on the device.
        id<MTL4Compiler> compiler = nil;
        if (useMetal4)
        {
            MTL4CompilerDescriptor* compilerDescriptor = [[MTL4CompilerDescriptor alloc] init];
            NSError* compilerError = nil;
            compiler = [device newCompilerWithDescriptor:compilerDescriptor error:&compilerError];
            if (compiler == nil)
            {
                std::string reason = compilerError != nil ? std::string(compilerError.localizedDescription.UTF8String) : std::string("unknown error");
                setError("Failed to create MTL4Compiler: " + reason);
                return false;
            }
        }

        // ---- Compute pipeline ----
        if (desc.computeShader.has_value())
        {
            std::string computeEntry;
            std::string loadError;
            id<MTLLibrary> computeLibrary = loadStageLibrary(device, compiler, useMetal4,
                desc.computeShader.value(), shaderCacheDir, desc.optionName, VkmShaderCacheStage::Compute, &computeEntry, &loadError);
            if (computeLibrary == nil)
            {
                setError(loadError);
                return false;
            }

            NSError* pipelineError = nil;
            if (useMetal4)
            {
                MTL4ComputePipelineDescriptor* computePipelineDescriptor = [[MTL4ComputePipelineDescriptor alloc] init];
                computePipelineDescriptor.computeFunctionDescriptor = makeMTL4FunctionDescriptor(computeLibrary, computeEntry);
                _computePipelineState = [compiler newComputePipelineStateWithDescriptor:computePipelineDescriptor
                                                                     compilerTaskOptions:nil
                                                                                   error:&pipelineError];
            }
            else
            {
                id<MTLFunction> computeFunction = [computeLibrary newFunctionWithName:[NSString stringWithUTF8String:computeEntry.c_str()]];
                if (computeFunction == nil)
                {
                    setError("Compute function '" + computeEntry + "' not found in library");
                    return false;
                }
                _computePipelineState = [device newComputePipelineStateWithFunction:computeFunction error:&pipelineError];
            }
            if (_computePipelineState == nil)
            {
                std::string reason = pipelineError != nil ? std::string(pipelineError.localizedDescription.UTF8String) : std::string("unknown error");
                setError("Failed to create compute pipeline state: " + reason);
                return false;
            }

            return true;
        }

        // ---- Graphics pipeline ----
        if (!desc.vertexShader.has_value())
        {
            setError("Graphics pipeline requires a vertex shader");
            return false;
        }

        std::string loadError;
        std::string vertexEntry;
        id<MTLLibrary> vertexLibrary = loadStageLibrary(device, compiler, useMetal4,
            desc.vertexShader.value(), shaderCacheDir, desc.optionName, VkmShaderCacheStage::Vertex, &vertexEntry, &loadError);
        if (vertexLibrary == nil)
        {
            setError(loadError);
            return false;
        }

        id<MTLLibrary> fragmentLibrary = nil;
        std::string fragmentEntry;
        if (desc.fragmentShader.has_value())
        {
            fragmentLibrary = loadStageLibrary(device, compiler, useMetal4,
                desc.fragmentShader.value(), shaderCacheDir, desc.optionName, VkmShaderCacheStage::Fragment, &fragmentEntry, &loadError);
            if (fragmentLibrary == nil)
            {
                setError(loadError);
                return false;
            }
        }

        MTLVertexDescriptor* vertexDescriptor = buildVertexDescriptor(desc);

        NSError* pipelineError = nil;
        if (useMetal4)
        {
            MTL4RenderPipelineDescriptor* renderPipelineDescriptor = [[MTL4RenderPipelineDescriptor alloc] init];
            renderPipelineDescriptor.vertexFunctionDescriptor = makeMTL4FunctionDescriptor(vertexLibrary, vertexEntry);
            if (fragmentLibrary != nil)
            {
                renderPipelineDescriptor.fragmentFunctionDescriptor = makeMTL4FunctionDescriptor(fragmentLibrary, fragmentEntry);
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
            // MTLRenderPipelineDescriptor in the Metal 3 branch below) -- confirmed against the
            // macOS 26 SDK headers. Depth/stencil attachment formats come from the render pass
            // (MTL4RenderPassDescriptor) at encode time, so a mismatch with
            // desc.depthStencilState.depthStencilFormat can only surface via Metal's validation
            // layer at draw time, not here.

            if (vertexDescriptor != nil)
            {
                renderPipelineDescriptor.vertexDescriptor = vertexDescriptor;
            }

            _renderPipelineState = [compiler newRenderPipelineStateWithDescriptor:renderPipelineDescriptor
                                                                compilerTaskOptions:nil
                                                                              error:&pipelineError];
        }
        else
        {
            MTLRenderPipelineDescriptor* renderPipelineDescriptor = [[MTLRenderPipelineDescriptor alloc] init];
            renderPipelineDescriptor.vertexFunction = [vertexLibrary newFunctionWithName:[NSString stringWithUTF8String:vertexEntry.c_str()]];
            if (renderPipelineDescriptor.vertexFunction == nil)
            {
                setError("Vertex function '" + vertexEntry + "' not found in library");
                return false;
            }
            if (fragmentLibrary != nil)
            {
                renderPipelineDescriptor.fragmentFunction = [fragmentLibrary newFunctionWithName:[NSString stringWithUTF8String:fragmentEntry.c_str()]];
                if (renderPipelineDescriptor.fragmentFunction == nil)
                {
                    setError("Fragment function '" + fragmentEntry + "' not found in library");
                    return false;
                }
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
                renderPipelineDescriptor.colorAttachments[i].blendingEnabled = attachment.blendEnable ? YES : NO;
                renderPipelineDescriptor.colorAttachments[i].sourceRGBBlendFactor = toMTLBlendFactor(attachment.srcColorBlendFactor);
                renderPipelineDescriptor.colorAttachments[i].destinationRGBBlendFactor = toMTLBlendFactor(attachment.dstColorBlendFactor);
                renderPipelineDescriptor.colorAttachments[i].rgbBlendOperation = toMTLBlendOperation(attachment.colorBlendOp);
                renderPipelineDescriptor.colorAttachments[i].sourceAlphaBlendFactor = toMTLBlendFactor(attachment.srcAlphaBlendFactor);
                renderPipelineDescriptor.colorAttachments[i].destinationAlphaBlendFactor = toMTLBlendFactor(attachment.dstAlphaBlendFactor);
                renderPipelineDescriptor.colorAttachments[i].alphaBlendOperation = toMTLBlendOperation(attachment.alphaBlendOp);
            }

            // Unlike the MTL4 path, the classic descriptor bakes depth/stencil formats into the
            // PSO, so set them from the pipeline's declared format when present (Undefined => none;
            // stencil format only for depth-stencil combined formats).
            const VkmFormat depthStencilFormat = desc.depthStencilState.depthStencilFormat;
            if (depthStencilFormat != VkmFormat::Undefined)
            {
                renderPipelineDescriptor.depthAttachmentPixelFormat = getMTLPixelFormat(depthStencilFormat);
                if (depthStencilFormat == VkmFormat::D24_UNORM_S8_UINT || depthStencilFormat == VkmFormat::D32_SFLOAT_S8_UINT)
                {
                    renderPipelineDescriptor.stencilAttachmentPixelFormat = getMTLPixelFormat(depthStencilFormat);
                }
            }

            if (vertexDescriptor != nil)
            {
                renderPipelineDescriptor.vertexDescriptor = vertexDescriptor;
            }

            _renderPipelineState = [device newRenderPipelineStateWithDescriptor:renderPipelineDescriptor error:&pipelineError];
        }

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
