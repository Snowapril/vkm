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
#import <Carbon/Carbon.h>

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

        // macOS virtual-keycode -> ImGuiKey table, mirroring Dear ImGui's official
        // imgui_impl_osx.mm backend (ImGui_ImplOSX_KeyCodeToImGuiKey), since this renderer
        // bridges input via its own NSEvent local monitor instead of that backend.
        ImGuiKey keyCodeToImGuiKey(int keyCode)
        {
            switch (keyCode)
            {
                case kVK_ANSI_A: return ImGuiKey_A;
                case kVK_ANSI_S: return ImGuiKey_S;
                case kVK_ANSI_D: return ImGuiKey_D;
                case kVK_ANSI_F: return ImGuiKey_F;
                case kVK_ANSI_H: return ImGuiKey_H;
                case kVK_ANSI_G: return ImGuiKey_G;
                case kVK_ANSI_Z: return ImGuiKey_Z;
                case kVK_ANSI_X: return ImGuiKey_X;
                case kVK_ANSI_C: return ImGuiKey_C;
                case kVK_ANSI_V: return ImGuiKey_V;
                case kVK_ANSI_B: return ImGuiKey_B;
                case kVK_ANSI_Q: return ImGuiKey_Q;
                case kVK_ANSI_W: return ImGuiKey_W;
                case kVK_ANSI_E: return ImGuiKey_E;
                case kVK_ANSI_R: return ImGuiKey_R;
                case kVK_ANSI_Y: return ImGuiKey_Y;
                case kVK_ANSI_T: return ImGuiKey_T;
                case kVK_ANSI_1: return ImGuiKey_1;
                case kVK_ANSI_2: return ImGuiKey_2;
                case kVK_ANSI_3: return ImGuiKey_3;
                case kVK_ANSI_4: return ImGuiKey_4;
                case kVK_ANSI_6: return ImGuiKey_6;
                case kVK_ANSI_5: return ImGuiKey_5;
                case kVK_ANSI_Equal: return ImGuiKey_Equal;
                case kVK_ANSI_9: return ImGuiKey_9;
                case kVK_ANSI_7: return ImGuiKey_7;
                case kVK_ANSI_Minus: return ImGuiKey_Minus;
                case kVK_ANSI_8: return ImGuiKey_8;
                case kVK_ANSI_0: return ImGuiKey_0;
                case kVK_ANSI_RightBracket: return ImGuiKey_RightBracket;
                case kVK_ANSI_O: return ImGuiKey_O;
                case kVK_ANSI_U: return ImGuiKey_U;
                case kVK_ANSI_LeftBracket: return ImGuiKey_LeftBracket;
                case kVK_ANSI_I: return ImGuiKey_I;
                case kVK_ANSI_P: return ImGuiKey_P;
                case kVK_ANSI_L: return ImGuiKey_L;
                case kVK_ANSI_J: return ImGuiKey_J;
                case kVK_ANSI_Quote: return ImGuiKey_Apostrophe;
                case kVK_ANSI_K: return ImGuiKey_K;
                case kVK_ANSI_Semicolon: return ImGuiKey_Semicolon;
                case kVK_ANSI_Backslash: return ImGuiKey_Backslash;
                case kVK_ANSI_Comma: return ImGuiKey_Comma;
                case kVK_ANSI_Slash: return ImGuiKey_Slash;
                case kVK_ANSI_N: return ImGuiKey_N;
                case kVK_ANSI_M: return ImGuiKey_M;
                case kVK_ANSI_Period: return ImGuiKey_Period;
                case kVK_ANSI_Grave: return ImGuiKey_GraveAccent;
                case kVK_ANSI_KeypadDecimal: return ImGuiKey_KeypadDecimal;
                case kVK_ANSI_KeypadMultiply: return ImGuiKey_KeypadMultiply;
                case kVK_ANSI_KeypadPlus: return ImGuiKey_KeypadAdd;
                case kVK_ANSI_KeypadClear: return ImGuiKey_NumLock;
                case kVK_ANSI_KeypadDivide: return ImGuiKey_KeypadDivide;
                case kVK_ANSI_KeypadEnter: return ImGuiKey_KeypadEnter;
                case kVK_ANSI_KeypadMinus: return ImGuiKey_KeypadSubtract;
                case kVK_ANSI_KeypadEquals: return ImGuiKey_KeypadEqual;
                case kVK_ANSI_Keypad0: return ImGuiKey_Keypad0;
                case kVK_ANSI_Keypad1: return ImGuiKey_Keypad1;
                case kVK_ANSI_Keypad2: return ImGuiKey_Keypad2;
                case kVK_ANSI_Keypad3: return ImGuiKey_Keypad3;
                case kVK_ANSI_Keypad4: return ImGuiKey_Keypad4;
                case kVK_ANSI_Keypad5: return ImGuiKey_Keypad5;
                case kVK_ANSI_Keypad6: return ImGuiKey_Keypad6;
                case kVK_ANSI_Keypad7: return ImGuiKey_Keypad7;
                case kVK_ANSI_Keypad8: return ImGuiKey_Keypad8;
                case kVK_ANSI_Keypad9: return ImGuiKey_Keypad9;
                case kVK_Return: return ImGuiKey_Enter;
                case kVK_Tab: return ImGuiKey_Tab;
                case kVK_Space: return ImGuiKey_Space;
                case kVK_Delete: return ImGuiKey_Backspace;
                case kVK_Escape: return ImGuiKey_Escape;
                case kVK_CapsLock: return ImGuiKey_CapsLock;
                case kVK_Control: return ImGuiKey_LeftCtrl;
                case kVK_Shift: return ImGuiKey_LeftShift;
                case kVK_Option: return ImGuiKey_LeftAlt;
                case kVK_Command: return ImGuiKey_LeftSuper;
                case kVK_RightControl: return ImGuiKey_RightCtrl;
                case kVK_RightShift: return ImGuiKey_RightShift;
                case kVK_RightOption: return ImGuiKey_RightAlt;
                case kVK_RightCommand: return ImGuiKey_RightSuper;
                case kVK_F1: return ImGuiKey_F1;
                case kVK_F2: return ImGuiKey_F2;
                case kVK_F3: return ImGuiKey_F3;
                case kVK_F4: return ImGuiKey_F4;
                case kVK_F5: return ImGuiKey_F5;
                case kVK_F6: return ImGuiKey_F6;
                case kVK_F7: return ImGuiKey_F7;
                case kVK_F8: return ImGuiKey_F8;
                case kVK_F9: return ImGuiKey_F9;
                case kVK_F10: return ImGuiKey_F10;
                case kVK_F11: return ImGuiKey_F11;
                case kVK_F12: return ImGuiKey_F12;
                case kVK_F13: return ImGuiKey_F13;
                case kVK_F14: return ImGuiKey_F14;
                case kVK_F15: return ImGuiKey_F15;
                case kVK_F16: return ImGuiKey_F16;
                case kVK_F17: return ImGuiKey_F17;
                case kVK_F18: return ImGuiKey_F18;
                case kVK_F19: return ImGuiKey_F19;
                case kVK_F20: return ImGuiKey_F20;
                case kVK_ContextualMenu: return ImGuiKey_Menu;
                case kVK_Help: return ImGuiKey_Insert;
                case kVK_Home: return ImGuiKey_Home;
                case kVK_PageUp: return ImGuiKey_PageUp;
                case kVK_ForwardDelete: return ImGuiKey_Delete;
                case kVK_End: return ImGuiKey_End;
                case kVK_PageDown: return ImGuiKey_PageDown;
                case kVK_LeftArrow: return ImGuiKey_LeftArrow;
                case kVK_RightArrow: return ImGuiKey_RightArrow;
                case kVK_DownArrow: return ImGuiKey_DownArrow;
                case kVK_UpArrow: return ImGuiKey_UpArrow;
                default: return ImGuiKey_None;
            }
        }

        // Filters out ASCII control characters, Delete, and the 0xF700-0xF8FF private-use range
        // macOS uses for arrow/function/page keys before forwarding text to
        // io.AddInputCharactersUTF8(), mirroring the character filtering historically applied by
        // Dear ImGui's official imgui_impl_osx backend.
        void addFilteredInputCharacters(ImGuiIO& io, NSString* characters)
        {
            NSMutableString* filtered = [NSMutableString stringWithCapacity:characters.length];
            for (NSUInteger i = 0; i < characters.length; ++i)
            {
                const unichar c = [characters characterAtIndex:i];
                if (c < 0x20 || c == 0x7F || (c >= 0xF700 && c <= 0xF8FF))
                {
                    continue;
                }
                [filtered appendFormat:@"%C", c];
            }
            if (filtered.length > 0)
            {
                io.AddInputCharactersUTF8(filtered.UTF8String);
            }
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
        id keyEventMonitor = nil; // NSEvent local monitor for keyboard + scroll-wheel input.

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

        const NSEventMask keyEventMask = NSEventMaskKeyDown | NSEventMaskKeyUp | NSEventMaskFlagsChanged | NSEventMaskScrollWheel;
        _impl->keyEventMonitor = [NSEvent addLocalMonitorForEventsMatchingMask:keyEventMask handler:^NSEvent* _Nullable(NSEvent* event) {
            ImGuiIO& imguiIO = ImGui::GetIO();
            switch (event.type)
            {
                case NSEventTypeKeyDown:
                {
                    if (![event isARepeat])
                    {
                        imguiIO.AddKeyEvent(keyCodeToImGuiKey((int)event.keyCode), true);
                    }
                    addFilteredInputCharacters(imguiIO, event.characters);
                    return imguiIO.WantCaptureKeyboard ? nil : event;
                }
                case NSEventTypeKeyUp:
                {
                    imguiIO.AddKeyEvent(keyCodeToImGuiKey((int)event.keyCode), false);
                    return imguiIO.WantCaptureKeyboard ? nil : event;
                }
                case NSEventTypeFlagsChanged:
                {
                    const NSEventModifierFlags modifierFlags = event.modifierFlags;
                    imguiIO.AddKeyEvent(ImGuiMod_Shift, (modifierFlags & NSEventModifierFlagShift) != 0);
                    imguiIO.AddKeyEvent(ImGuiMod_Ctrl, (modifierFlags & NSEventModifierFlagControl) != 0);
                    imguiIO.AddKeyEvent(ImGuiMod_Alt, (modifierFlags & NSEventModifierFlagOption) != 0);
                    imguiIO.AddKeyEvent(ImGuiMod_Super, (modifierFlags & NSEventModifierFlagCommand) != 0);

                    // macOS does not generate down/up events for individual modifier keys, so the
                    // per-key (as opposed to per-mod) state is derived from hardware-dependent masks,
                    // mirroring the official imgui_impl_osx backend.
                    const ImGuiKey modifierKey = keyCodeToImGuiKey((int)event.keyCode);
                    NSEventModifierFlags hardwareMask = 0;
                    switch (modifierKey)
                    {
                        case ImGuiKey_LeftCtrl:   hardwareMask = 0x0001; break;
                        case ImGuiKey_RightCtrl:  hardwareMask = 0x2000; break;
                        case ImGuiKey_LeftShift:  hardwareMask = 0x0002; break;
                        case ImGuiKey_RightShift: hardwareMask = 0x0004; break;
                        case ImGuiKey_LeftSuper:  hardwareMask = 0x0008; break;
                        case ImGuiKey_RightSuper: hardwareMask = 0x0010; break;
                        case ImGuiKey_LeftAlt:    hardwareMask = 0x0020; break;
                        case ImGuiKey_RightAlt:   hardwareMask = 0x0040; break;
                        default:                  return imguiIO.WantCaptureKeyboard ? nil : event;
                    }
                    imguiIO.AddKeyEvent(modifierKey, (modifierFlags & hardwareMask) != 0);
                    return imguiIO.WantCaptureKeyboard ? nil : event;
                }
                case NSEventTypeScrollWheel:
                {
                    double wheelDeltaX = event.scrollingDeltaX;
                    double wheelDeltaY = event.scrollingDeltaY;
                    if (!event.hasPreciseScrollingDeltas)
                    {
                        wheelDeltaX *= 0.1;
                        wheelDeltaY *= 0.1;
                    }
                    if (wheelDeltaX != 0.0 || wheelDeltaY != 0.0)
                    {
                        imguiIO.AddMouseWheelEvent((float)wheelDeltaX, (float)wheelDeltaY);
                    }
                    return imguiIO.WantCaptureMouse ? nil : event;
                }
                default:
                    return event;
            }
        }];

        return true;
    }

    void VkmImGuiRendererMetal::newFrameInner()
    {
        ImGuiIO& io = ImGui::GetIO();
        const CGSize drawableSize = _impl->metalLayer.drawableSize;
        io.DisplaySize = ImVec2((float)drawableSize.width, (float)drawableSize.height);
        io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
        io.DeltaTime = io.DeltaTime > 0.0f ? io.DeltaTime : (1.0f / 60.0f);

        // Mouse position + buttons are polled from the key window each frame. Keyboard input
        // and scroll-wheel are event-driven instead, bridged via the NSEvent local monitor
        // installed in initializeInner() (see keyEventMonitor).
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
        if (_impl->keyEventMonitor != nil)
        {
            [NSEvent removeMonitor:_impl->keyEventMonitor];
            _impl->keyEventMonitor = nil;
        }

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
