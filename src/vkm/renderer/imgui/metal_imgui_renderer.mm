// Copyright (c) 2025 Snowapril

#include <vkm/renderer/imgui/metal_imgui_renderer.h>
#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/metal/metal_command_buffer.h>
#include <vkm/renderer/backend/common/renderer_common.h>

#include <imgui.h>

#import <Metal/Metal.h>
#import <Metal/MTL4Compiler.h>
#import <Metal/MTL4LibraryDescriptor.h>
#import <Metal/MTL4LibraryFunctionDescriptor.h>
#import <Metal/MTL4RenderPipeline.h>
#import <Metal/MTL4PipelineState.h>
#import <Metal/MTL4ArgumentTable.h>
#import <Metal/MTL4RenderCommandEncoder.h>
#import <QuartzCore/CAMetalLayer.h>
#import <AppKit/AppKit.h>

#include <cstring>

namespace vkm
{
    namespace
    {
        // Vertex/fragment pair mirroring upstream imgui_impl_metal.mm's shader, retargeted to be
        // compiled through MTL4Compiler and bind resources by plain buffer(N)/texture(N)/sampler(N)
        // index (an MTL4ArgumentTable binds to the same indices; the shader syntax itself is unchanged).
        const char* IMGUI_METAL4_SHADER_SOURCE = R"(
            #include <metal_stdlib>
            using namespace metal;

            struct Uniforms { float4x4 projectionMatrix; };

            struct VertexIn { float2 position; float2 texCoord; uchar4 color; };

            struct VertexOut
            {
                float4 position [[position]];
                float2 texCoord;
                float4 color;
            };

            vertex VertexOut vkm_imgui_vertex(uint vertexID [[vertex_id]],
                                               device const VertexIn* vertices [[buffer(0)]],
                                               constant Uniforms& uniforms [[buffer(1)]])
            {
                VertexIn in = vertices[vertexID];
                VertexOut out;
                out.position = uniforms.projectionMatrix * float4(in.position, 0.0, 1.0);
                out.texCoord = in.texCoord;
                out.color = float4(in.color) / float4(255.0);
                return out;
            }

            fragment float4 vkm_imgui_fragment(VertexOut in [[stage_in]],
                                                texture2d<float> tex [[texture(0)]],
                                                sampler texSampler [[sampler(0)]])
            {
                return in.color * tex.sample(texSampler, in.texCoord);
            }
        )";

        struct Uniforms
        {
            float projectionMatrix[16];
        };

        ImTextureID toImTextureID(MTLResourceID resourceID)
        {
            ImTextureID texID;
            static_assert(sizeof(texID) == sizeof(resourceID), "MTLResourceID/ImTextureID size mismatch");
            std::memcpy(&texID, &resourceID, sizeof(texID));
            return texID;
        }

        MTLResourceID fromImTextureID(ImTextureID texID)
        {
            MTLResourceID resourceID;
            std::memcpy(&resourceID, &texID, sizeof(resourceID));
            return resourceID;
        }

        void updateTexture(id<MTLDevice> device, ImTextureData* tex)
        {
            if (tex->Status == ImTextureStatus_WantCreate)
            {
                MTLTextureDescriptor* texDesc = [[MTLTextureDescriptor alloc] init];
                texDesc.pixelFormat = (tex->Format == ImTextureFormat_Alpha8) ? MTLPixelFormatA8Unorm : MTLPixelFormatRGBA8Unorm;
                texDesc.width = (NSUInteger)tex->Width;
                texDesc.height = (NSUInteger)tex->Height;
                texDesc.usage = MTLTextureUsageShaderRead;
                texDesc.storageMode = MTLStorageModeShared;

                id<MTLTexture> texture = [device newTextureWithDescriptor:texDesc];
                [texDesc release];

                const MTLRegion region = MTLRegionMake2D(0, 0, (NSUInteger)tex->Width, (NSUInteger)tex->Height);
                [texture replaceRegion:region mipmapLevel:0 withBytes:tex->GetPixels() bytesPerRow:(NSUInteger)tex->GetPitch()];

                // texture is already +1 owned (newTextureWithDescriptor: follows Cocoa's "new"
                // ownership convention); store the raw pointer and release it explicitly on destroy.
                tex->BackendUserData = (void*)texture;
                tex->SetTexID(toImTextureID(texture.gpuResourceID));
                tex->SetStatus(ImTextureStatus_OK);
            }
            else if (tex->Status == ImTextureStatus_WantUpdates)
            {
                id<MTLTexture> texture = (id<MTLTexture>)tex->BackendUserData;
                for (const ImTextureRect& rect : tex->Updates)
                {
                    const MTLRegion region = MTLRegionMake2D(rect.x, rect.y, rect.w, rect.h);
                    [texture replaceRegion:region mipmapLevel:0 withBytes:tex->GetPixelsAt(rect.x, rect.y) bytesPerRow:(NSUInteger)tex->GetPitch()];
                }
                tex->SetStatus(ImTextureStatus_OK);
            }
            else if (tex->Status == ImTextureStatus_WantDestroy && tex->UnusedFrames > 0)
            {
                id<MTLTexture> texture = (id<MTLTexture>)tex->BackendUserData;
                [texture release];
                tex->BackendUserData = nullptr;
                tex->SetTexID(ImTextureID_Invalid);
                tex->SetStatus(ImTextureStatus_Destroyed);
            }
        }

        id<MTLBuffer> ensureBufferCapacity(id<MTLDevice> device, id<MTLBuffer> buffer, NSUInteger requiredSize)
        {
            if (buffer != nil && buffer.length >= requiredSize)
            {
                return buffer;
            }
            [buffer release];
            return [device newBufferWithLength:requiredSize options:MTLResourceStorageModeShared];
        }
    } // namespace

    class VkmImGuiRendererMetal::Impl
    {
    public:
        id<MTLDevice> device = nil;
        id<MTLRenderPipelineState> pipelineState = nil;
        id<MTL4ArgumentTable> argumentTable = nil;
        id<MTLSamplerState> sampler = nil;
        CAMetalLayer* metalLayer = nil;

        struct FrameResources
        {
            id<MTLBuffer> vertexBuffer = nil;
            id<MTLBuffer> indexBuffer = nil;
            id<MTLBuffer> uniformBuffer = nil;
        };
        FrameResources frames[FRAME_BUFFER_COUNT];
        uint32_t frameIndex = 0;
    };

    VkmImGuiRendererMetal::VkmImGuiRendererMetal(VkmDriverBase* driver)
        : VkmImGuiRendererBase(driver), _impl(new Impl())
    {
    }

    VkmImGuiRendererMetal::~VkmImGuiRendererMetal()
    {
        delete _impl;
    }

    bool VkmImGuiRendererMetal::initializeInner(void* windowHandle, VkmFormat backBufferFormat)
    {
        (void)backBufferFormat; // MTLPixelFormatRGBA16Float always, matches application.mm's CAMetalLayer setup

        VkmDriverMetal* driverMetal = static_cast<VkmDriverMetal*>(_driver);
        _impl->device = driverMetal->getMTLDevice();
        _impl->metalLayer = (__bridge CAMetalLayer*)windowHandle;

        ImGuiIO& io = ImGui::GetIO();
        io.BackendRendererName = "vkm_metal4";
        io.BackendFlags |= ImGuiBackendFlags_RendererHasVtxOffset | ImGuiBackendFlags_RendererHasTextures;

        NSError* error = nil;

        MTL4CompilerDescriptor* compilerDesc = [[MTL4CompilerDescriptor alloc] init];
        id<MTL4Compiler> compiler = [_impl->device newCompilerWithDescriptor:compilerDesc error:&error];
        [compilerDesc release];
        if (compiler == nil)
        {
            VKM_DEBUG_ERROR(fmt::format("Failed to create MTL4Compiler: {}", error.localizedDescription.UTF8String).c_str());
            return false;
        }

        MTL4LibraryDescriptor* libraryDesc = [[MTL4LibraryDescriptor alloc] init];
        libraryDesc.source = [NSString stringWithUTF8String:IMGUI_METAL4_SHADER_SOURCE];
        id<MTLLibrary> library = [compiler newLibraryWithDescriptor:libraryDesc error:&error];
        [libraryDesc release];
        if (library == nil)
        {
            VKM_DEBUG_ERROR(fmt::format("Failed to compile ImGui Metal4 shader library: {}", error.localizedDescription.UTF8String).c_str());
            return false;
        }

        MTL4LibraryFunctionDescriptor* vertexFuncDesc = [[MTL4LibraryFunctionDescriptor alloc] init];
        vertexFuncDesc.library = library;
        vertexFuncDesc.name = @"vkm_imgui_vertex";

        MTL4LibraryFunctionDescriptor* fragmentFuncDesc = [[MTL4LibraryFunctionDescriptor alloc] init];
        fragmentFuncDesc.library = library;
        fragmentFuncDesc.name = @"vkm_imgui_fragment";

        MTL4RenderPipelineDescriptor* pipelineDesc = [[MTL4RenderPipelineDescriptor alloc] init];
        pipelineDesc.vertexFunctionDescriptor = vertexFuncDesc;
        pipelineDesc.fragmentFunctionDescriptor = fragmentFuncDesc;
        pipelineDesc.rasterSampleCount = 1;
        pipelineDesc.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
        pipelineDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float;
        pipelineDesc.colorAttachments[0].blendingState = MTL4BlendStateEnabled;
        pipelineDesc.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
        pipelineDesc.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        pipelineDesc.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        pipelineDesc.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;

        _impl->pipelineState = [compiler newRenderPipelineStateWithDescriptor:pipelineDesc compilerTaskOptions:nil error:&error];

        [pipelineDesc release];
        [fragmentFuncDesc release];
        [vertexFuncDesc release];
        [library release];
        [compiler release];

        if (_impl->pipelineState == nil)
        {
            VKM_DEBUG_ERROR(fmt::format("Failed to create ImGui Metal4 pipeline state: {}", error.localizedDescription.UTF8String).c_str());
            return false;
        }

        MTL4ArgumentTableDescriptor* argTableDesc = [[MTL4ArgumentTableDescriptor alloc] init];
        argTableDesc.maxBufferBindCount = 2;
        argTableDesc.maxTextureBindCount = 1;
        argTableDesc.maxSamplerStateBindCount = 1;
        _impl->argumentTable = [_impl->device newArgumentTableWithDescriptor:argTableDesc error:&error];
        [argTableDesc release];
        if (_impl->argumentTable == nil)
        {
            VKM_DEBUG_ERROR(fmt::format("Failed to create ImGui Metal4 argument table: {}", error.localizedDescription.UTF8String).c_str());
            return false;
        }

        MTLSamplerDescriptor* samplerDesc = [[MTLSamplerDescriptor alloc] init];
        samplerDesc.minFilter = MTLSamplerMinMagFilterLinear;
        samplerDesc.magFilter = MTLSamplerMinMagFilterLinear;
        samplerDesc.sAddressMode = MTLSamplerAddressModeClampToEdge;
        samplerDesc.tAddressMode = MTLSamplerAddressModeClampToEdge;
        _impl->sampler = [_impl->device newSamplerStateWithDescriptor:samplerDesc];
        [samplerDesc release];
        [_impl->argumentTable setSamplerState:_impl->sampler.gpuResourceID atIndex:0];

        return true;
    }

    void VkmImGuiRendererMetal::newFrameInner()
    {
        ImGuiIO& io = ImGui::GetIO();
        const CGSize drawableSize = _impl->metalLayer.drawableSize;
        io.DisplaySize = ImVec2((float)drawableSize.width, (float)drawableSize.height);
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        io.DeltaTime = io.DeltaTime > 0.0f ? io.DeltaTime : (1.0f / 60.0f);

        // Minimal input bridge: mouse position + buttons polled from the key window each frame.
        // Keyboard input and scroll-wheel are not wired up yet (TODO(snowapril): needs an
        // event-driven bridge -- e.g. NSEvent local monitors -- rather than per-frame polling).
        NSWindow* keyWindow = [NSApp keyWindow];
        if (keyWindow != nil)
        {
            const NSPoint windowPoint = [keyWindow mouseLocationOutsideOfEventStream];
            const NSRect contentFrame = [keyWindow contentLayoutRect];
            io.AddMousePosEvent((float)windowPoint.x, (float)(contentFrame.size.height - windowPoint.y));

            const NSUInteger pressedButtons = [NSEvent pressedMouseButtons];
            io.AddMouseButtonEvent(ImGuiMouseButton_Left, (pressedButtons & (1u << 0)) != 0);
            io.AddMouseButtonEvent(ImGuiMouseButton_Right, (pressedButtons & (1u << 1)) != 0);
            io.AddMouseButtonEvent(ImGuiMouseButton_Middle, (pressedButtons & (1u << 2)) != 0);
        }

        ImGui::NewFrame();
    }

    void VkmImGuiRendererMetal::renderDrawDataInner(VkmCommandBufferBase* commandBuffer)
    {
        ImDrawData* drawData = ImGui::GetDrawData();
        if (drawData == nullptr || drawData->CmdListsCount == 0 || drawData->DisplaySize.x <= 0.0f || drawData->DisplaySize.y <= 0.0f)
        {
            return;
        }

        if (drawData->Textures != nullptr)
        {
            for (ImTextureData* tex : *drawData->Textures)
            {
                if (tex->Status != ImTextureStatus_OK)
                {
                    updateTexture(_impl->device, tex);
                }
            }
        }

        Impl::FrameResources& frame = _impl->frames[_impl->frameIndex];
        _impl->frameIndex = (_impl->frameIndex + 1) % FRAME_BUFFER_COUNT;

        const NSUInteger vertexBufferSize = (NSUInteger)drawData->TotalVtxCount * sizeof(ImDrawVert);
        const NSUInteger indexBufferSize = (NSUInteger)drawData->TotalIdxCount * sizeof(ImDrawIdx);
        if (vertexBufferSize == 0 || indexBufferSize == 0)
        {
            return;
        }

        frame.vertexBuffer = ensureBufferCapacity(_impl->device, frame.vertexBuffer, vertexBufferSize);
        frame.indexBuffer = ensureBufferCapacity(_impl->device, frame.indexBuffer, indexBufferSize);
        frame.uniformBuffer = ensureBufferCapacity(_impl->device, frame.uniformBuffer, sizeof(Uniforms));

        ImDrawVert* vtxDst = (ImDrawVert*)frame.vertexBuffer.contents;
        ImDrawIdx* idxDst = (ImDrawIdx*)frame.indexBuffer.contents;
        for (int i = 0; i < drawData->CmdListsCount; ++i)
        {
            const ImDrawList* cmdList = drawData->CmdLists[i];
            std::memcpy(vtxDst, cmdList->VtxBuffer.Data, (size_t)cmdList->VtxBuffer.Size * sizeof(ImDrawVert));
            std::memcpy(idxDst, cmdList->IdxBuffer.Data, (size_t)cmdList->IdxBuffer.Size * sizeof(ImDrawIdx));
            vtxDst += cmdList->VtxBuffer.Size;
            idxDst += cmdList->IdxBuffer.Size;
        }

        const float L = drawData->DisplayPos.x;
        const float R = drawData->DisplayPos.x + drawData->DisplaySize.x;
        const float T = drawData->DisplayPos.y;
        const float B = drawData->DisplayPos.y + drawData->DisplaySize.y;
        Uniforms uniforms{{
            2.0f / (R - L), 0.0f, 0.0f, 0.0f,
            0.0f, 2.0f / (T - B), 0.0f, 0.0f,
            0.0f, 0.0f, -1.0f, 0.0f,
            (R + L) / (L - R), (T + B) / (B - T), 0.0f, 1.0f,
        }};
        std::memcpy(frame.uniformBuffer.contents, &uniforms, sizeof(uniforms));

        VkmCommandBufferMetal* commandBufferMetal = static_cast<VkmCommandBufferMetal*>(commandBuffer);
        id<MTL4RenderCommandEncoder> encoder = commandBufferMetal->getActiveRenderCommandEncoder();

        [encoder setRenderPipelineState:_impl->pipelineState];
        [encoder setArgumentTable:_impl->argumentTable atStages:MTLRenderStageVertex | MTLRenderStageFragment];
        [_impl->argumentTable setAddress:frame.uniformBuffer.gpuAddress atIndex:1];
        [encoder setViewport:(MTLViewport){0.0, 0.0, (double)drawData->DisplaySize.x, (double)drawData->DisplaySize.y, 0.0, 1.0}];

        const ImVec2 clipOff = drawData->DisplayPos;
        uint32_t globalVtxOffset = 0;
        uint32_t globalIdxOffset = 0;
        for (int i = 0; i < drawData->CmdListsCount; ++i)
        {
            const ImDrawList* cmdList = drawData->CmdLists[i];
            for (int cmdIdx = 0; cmdIdx < cmdList->CmdBuffer.Size; ++cmdIdx)
            {
                const ImDrawCmd& cmd = cmdList->CmdBuffer[cmdIdx];
                if (cmd.UserCallback != nullptr)
                {
                    continue; // Custom draw callbacks are not supported by this minimal backend.
                }

                const float clipMinX = cmd.ClipRect.x - clipOff.x;
                const float clipMinY = cmd.ClipRect.y - clipOff.y;
                const float clipMaxX = cmd.ClipRect.z - clipOff.x;
                const float clipMaxY = cmd.ClipRect.w - clipOff.y;
                if (clipMaxX <= clipMinX || clipMaxY <= clipMinY)
                {
                    continue;
                }

                const NSUInteger scissorX = (NSUInteger)std::max(0.0f, clipMinX);
                const NSUInteger scissorY = (NSUInteger)std::max(0.0f, clipMinY);
                const NSUInteger scissorMaxX = (NSUInteger)std::min(clipMaxX, drawData->DisplaySize.x);
                const NSUInteger scissorMaxY = (NSUInteger)std::min(clipMaxY, drawData->DisplaySize.y);
                if (scissorMaxX <= scissorX || scissorMaxY <= scissorY)
                {
                    continue;
                }
                [encoder setScissorRect:(MTLScissorRect){scissorX, scissorY, scissorMaxX - scissorX, scissorMaxY - scissorY}];

                const uint64_t vtxByteOffset = (uint64_t)(globalVtxOffset + cmd.VtxOffset) * sizeof(ImDrawVert);
                const uint64_t idxByteOffset = (uint64_t)(globalIdxOffset + cmd.IdxOffset) * sizeof(ImDrawIdx);
                [_impl->argumentTable setAddress:(frame.vertexBuffer.gpuAddress + vtxByteOffset) atIndex:0];
                [_impl->argumentTable setTexture:fromImTextureID(cmd.GetTexID()) atIndex:0];

                [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                     indexCount:cmd.ElemCount
                                      indexType:MTLIndexTypeUInt16
                                    indexBuffer:(frame.indexBuffer.gpuAddress + idxByteOffset)
                              indexBufferLength:(frame.indexBuffer.length - idxByteOffset)];
            }
            globalVtxOffset += (uint32_t)cmdList->VtxBuffer.Size;
            globalIdxOffset += (uint32_t)cmdList->IdxBuffer.Size;
        }
    }

    void VkmImGuiRendererMetal::shutdownInner()
    {
        for (Impl::FrameResources& frame : _impl->frames)
        {
            [frame.vertexBuffer release];
            [frame.indexBuffer release];
            [frame.uniformBuffer release];
        }
        [_impl->sampler release];
        [_impl->argumentTable release];
        [_impl->pipelineState release];
    }
} // namespace vkm
