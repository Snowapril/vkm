// Copyright (c) 2025 Snowapril

#include <vkm/platform/apple/application.h>

#if defined(VKM_USE_METAL_API)
#include <vkm/renderer/backend/metal/metal_driver.h>
#include <vkm/renderer/backend/metal/metal_swapchain.h>

#import <Cocoa/Cocoa.h>

#if TARGET_OS_IOS
#import <UIKit/UIKit.h>
#endif

#include <Metal/Metal.h>
#include <QuartzCore/QuartzCore.h>
#include <QuartzCore/CAFrameRateRange.h>

#include <QuartzCore/CAMetalLayer.h>

// Provides the kVK_* virtual key constants used by macKeyCodeToVkmKeyCode below.
#include <Carbon/Carbon.h>
#elif defined(VKM_USE_VULKAN_API)
// vulkan_driver.h (volk) must precede glfw3.h: glfw3.h only declares
// glfwInitVulkanLoader() when the Vulkan API types are already visible.
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <GLFW/glfw3.h>
#endif

#include <vkm/platform/common/window.h>
#include <vkm/platform/common/glfw_input.h>

#if defined(VKM_USE_METAL_API)
@interface VkmWindowImpl : NSWindow
- (void)setEngine:(nonnull vkm::VkmEngine *)engine;
@end

@interface VkmApplicationImpl : NSObject <NSApplicationDelegate, NSWindowDelegate>
@end

@interface RendererCoordinatorController : NSObject <CAMetalDisplayLinkDelegate>
- (nonnull instancetype)initWithMetalLayer:(nonnull CAMetalLayer *)metalLayer uiCanvasSize:(NSUInteger)uiCanvasSize;

- (void)metalDisplayLink:(nonnull CAMetalDisplayLink *)link needsUpdate:(nonnull CAMetalDisplayLinkUpdate *)update;
@end

static void* renderWorker( void* _Nullable obj )
{
    pthread_setname_np("RenderThread");
    CAMetalDisplayLink* metalDisplayLink = (__bridge CAMetalDisplayLink *)obj;
    [metalDisplayLink addToRunLoop:[NSRunLoop currentRunLoop]
                           forMode:NSDefaultRunLoopMode];
    [[NSRunLoop currentRunLoop] run];
    return nil;
}

@implementation RendererCoordinatorController
{
    CAMetalLayer*                   _metalLayer;
    CAMetalDisplayLink*             _metalDisplayLink;
    vkm::VkmEngine*                 _engine;
    vkm::VkmSwapChainMetal*         _swapChain;
    // Secondary ImGui window. It has no display link of its own -- the main link's callback
    // pulls a drawable for it each frame so both swapchains stay on the same frame cadence.
    CAMetalLayer*                   _imguiMetalLayer;
    vkm::VkmSwapChainMetal*         _imguiSwapChain;
}

- (nonnull instancetype)initWithMetalLayer:(nonnull CAMetalLayer *)metalLayer uiCanvasSize:(NSUInteger)uiCanvasSize
{
    self = [super init];
    if(self)
    {
        _metalLayer = metalLayer;

        _metalDisplayLink = [[CAMetalDisplayLink alloc] initWithMetalLayer:_metalLayer];
        _metalDisplayLink.delegate = self;
        _metalDisplayLink.preferredFrameRateRange = CAFrameRateRangeMake(30, 120, 120);
        
        // Create a high-priority thread sets up the CAMetalDisplayLink callback and renders the game
        
        int res = 0;
        pthread_attr_t attr;
        res = pthread_attr_init( &attr );
        NSAssert( res == 0, @"Unable to initialize thread attributes." );
        
        // Opt out of priority decay:
        res = pthread_attr_setschedpolicy( &attr, SCHED_RR );
        NSAssert( res == 0, @"Unable to set thread attribute scheduler policy." );
        
        // Increate priority of render thread:
        struct sched_param param = { .sched_priority = 45 };
        res = pthread_attr_setschedparam( &attr, &param );
        NSAssert( res == 0, @"Unable to set thread attribute priority." );
        
        // Enable the system to automatically clean up upon thread exit:
        res = pthread_attr_setdetachstate( &attr, PTHREAD_CREATE_DETACHED );
        NSAssert( res == 0, @"Unable set set thread attribute to run detached." );
        
        // Create thread:
        pthread_t tid;
        res = pthread_create( &tid, &attr, renderWorker, (__bridge void *)_metalDisplayLink );
        NSAssert( res == 0, @"Unable to create render thread" );
        
        // Clean up transient objects:
        pthread_attr_destroy( &attr );
    }

    return self;
}

- (void)dealloc
{
    self->_metalDisplayLink = nil;
    [super dealloc];
}

- (void)renderThreadLoop
{
    [_metalDisplayLink addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSDefaultRunLoopMode];
    [[NSRunLoop currentRunLoop] run];
}

- (void)metalDisplayLink:(nonnull CAMetalDisplayLink *)link needsUpdate:(nonnull CAMetalDisplayLinkUpdate *)update
{
#if TARGET_OS_IOS
    // iOS does not post EDR headroom notifications. This sample queries the headroom
    // and adjusts it before rendering each frame.
    // float maxEDRValue = UIScreen.mainScreen.potentialEDRHeadroom;
    // _engine->setMaxEDRValue(maxEDRValue);
#endif // TARGET_OS_IOS
    
    id<CAMetalDrawable> drawable = update.drawable;

    _swapChain->overrideCurrentDrawable(drawable);

    // Feed the secondary ImGui window a drawable for this frame. nextDrawable may briefly block
    // on the drawable pool; if it returns nil the engine skips that window's render this frame.
    if (_imguiSwapChain != nullptr && _imguiMetalLayer != nil)
    {
        id<CAMetalDrawable> imguiDrawable = [_imguiMetalLayer nextDrawable];
        _imguiSwapChain->overrideCurrentDrawable(imguiDrawable);
    }

    _engine->loopInner(CACurrentMediaTime());

    if (_engine->shouldExit())
    {
        // AppKit calls must run on the main thread, not this render-thread callback.
        dispatch_async(dispatch_get_main_queue(), ^{
            [[NSApplication sharedApplication] terminate:nil];
        });
    }
}

- (void)setEngine:(nonnull vkm::VkmEngine*)engine
{
    _engine = engine;
}

- (void)setSwapChain:(nonnull vkm::VkmSwapChainMetal*)swapChain
{
    _swapChain = swapChain;
}

- (void)setImGuiSwapChain:(nonnull vkm::VkmSwapChainMetal*)swapChain
{
    _imguiSwapChain = swapChain;
}

- (void)setImGuiMetalLayer:(nonnull CAMetalLayer*)metalLayer
{
    _imguiMetalLayer = metalLayer;
}
@end

namespace
{
    vkm::VkmKeyCode macKeyCodeToVkmKeyCode(int keyCode)
    {
        switch (keyCode)
        {
            case kVK_ANSI_A: return vkm::VkmKeyCode::A;
            case kVK_ANSI_B: return vkm::VkmKeyCode::B;
            case kVK_ANSI_C: return vkm::VkmKeyCode::C;
            case kVK_ANSI_D: return vkm::VkmKeyCode::D;
            case kVK_ANSI_E: return vkm::VkmKeyCode::E;
            case kVK_ANSI_F: return vkm::VkmKeyCode::F;
            case kVK_ANSI_G: return vkm::VkmKeyCode::G;
            case kVK_ANSI_H: return vkm::VkmKeyCode::H;
            case kVK_ANSI_I: return vkm::VkmKeyCode::I;
            case kVK_ANSI_J: return vkm::VkmKeyCode::J;
            case kVK_ANSI_K: return vkm::VkmKeyCode::K;
            case kVK_ANSI_L: return vkm::VkmKeyCode::L;
            case kVK_ANSI_M: return vkm::VkmKeyCode::M;
            case kVK_ANSI_N: return vkm::VkmKeyCode::N;
            case kVK_ANSI_O: return vkm::VkmKeyCode::O;
            case kVK_ANSI_P: return vkm::VkmKeyCode::P;
            case kVK_ANSI_Q: return vkm::VkmKeyCode::Q;
            case kVK_ANSI_R: return vkm::VkmKeyCode::R;
            case kVK_ANSI_S: return vkm::VkmKeyCode::S;
            case kVK_ANSI_T: return vkm::VkmKeyCode::T;
            case kVK_ANSI_U: return vkm::VkmKeyCode::U;
            case kVK_ANSI_V: return vkm::VkmKeyCode::V;
            case kVK_ANSI_W: return vkm::VkmKeyCode::W;
            case kVK_ANSI_X: return vkm::VkmKeyCode::X;
            case kVK_ANSI_Y: return vkm::VkmKeyCode::Y;
            case kVK_ANSI_Z: return vkm::VkmKeyCode::Z;

            case kVK_ANSI_0: return vkm::VkmKeyCode::Num0;
            case kVK_ANSI_1: return vkm::VkmKeyCode::Num1;
            case kVK_ANSI_2: return vkm::VkmKeyCode::Num2;
            case kVK_ANSI_3: return vkm::VkmKeyCode::Num3;
            case kVK_ANSI_4: return vkm::VkmKeyCode::Num4;
            case kVK_ANSI_5: return vkm::VkmKeyCode::Num5;
            case kVK_ANSI_6: return vkm::VkmKeyCode::Num6;
            case kVK_ANSI_7: return vkm::VkmKeyCode::Num7;
            case kVK_ANSI_8: return vkm::VkmKeyCode::Num8;
            case kVK_ANSI_9: return vkm::VkmKeyCode::Num9;

            case kVK_F1:  return vkm::VkmKeyCode::F1;
            case kVK_F2:  return vkm::VkmKeyCode::F2;
            case kVK_F3:  return vkm::VkmKeyCode::F3;
            case kVK_F4:  return vkm::VkmKeyCode::F4;
            case kVK_F5:  return vkm::VkmKeyCode::F5;
            case kVK_F6:  return vkm::VkmKeyCode::F6;
            case kVK_F7:  return vkm::VkmKeyCode::F7;
            case kVK_F8:  return vkm::VkmKeyCode::F8;
            case kVK_F9:  return vkm::VkmKeyCode::F9;
            case kVK_F10: return vkm::VkmKeyCode::F10;
            case kVK_F11: return vkm::VkmKeyCode::F11;
            case kVK_F12: return vkm::VkmKeyCode::F12;

            case kVK_LeftArrow:  return vkm::VkmKeyCode::Left;
            case kVK_RightArrow: return vkm::VkmKeyCode::Right;
            case kVK_UpArrow:    return vkm::VkmKeyCode::Up;
            case kVK_DownArrow:  return vkm::VkmKeyCode::Down;

            case kVK_Shift:        return vkm::VkmKeyCode::LeftShift;
            case kVK_RightShift:   return vkm::VkmKeyCode::RightShift;
            case kVK_Control:      return vkm::VkmKeyCode::LeftControl;
            case kVK_RightControl: return vkm::VkmKeyCode::RightControl;
            case kVK_Option:       return vkm::VkmKeyCode::LeftAlt;
            case kVK_RightOption:  return vkm::VkmKeyCode::RightAlt;
            case kVK_Command:      return vkm::VkmKeyCode::LeftSuper;
            case kVK_RightCommand: return vkm::VkmKeyCode::RightSuper;

            case kVK_Space:       return vkm::VkmKeyCode::Space;
            case kVK_Return:      return vkm::VkmKeyCode::Enter;
            case kVK_Tab:         return vkm::VkmKeyCode::Tab;
            case kVK_Delete:      return vkm::VkmKeyCode::Backspace;
            case kVK_ForwardDelete: return vkm::VkmKeyCode::Delete;
            case kVK_Escape:      return vkm::VkmKeyCode::Escape;

            case kVK_Help:        return vkm::VkmKeyCode::Insert;
            case kVK_Home:        return vkm::VkmKeyCode::Home;
            case kVK_End:         return vkm::VkmKeyCode::End;
            case kVK_PageUp:      return vkm::VkmKeyCode::PageUp;
            case kVK_PageDown:    return vkm::VkmKeyCode::PageDown;
            case kVK_CapsLock:    return vkm::VkmKeyCode::CapsLock;

            case kVK_ANSI_Minus:        return vkm::VkmKeyCode::Minus;
            case kVK_ANSI_Equal:        return vkm::VkmKeyCode::Equal;
            case kVK_ANSI_LeftBracket:  return vkm::VkmKeyCode::LeftBracket;
            case kVK_ANSI_RightBracket: return vkm::VkmKeyCode::RightBracket;
            case kVK_ANSI_Backslash:    return vkm::VkmKeyCode::Backslash;
            case kVK_ANSI_Semicolon:    return vkm::VkmKeyCode::Semicolon;
            case kVK_ANSI_Quote:        return vkm::VkmKeyCode::Apostrophe;
            case kVK_ANSI_Grave:        return vkm::VkmKeyCode::GraveAccent;
            case kVK_ANSI_Comma:        return vkm::VkmKeyCode::Comma;
            case kVK_ANSI_Period:       return vkm::VkmKeyCode::Period;
            case kVK_ANSI_Slash:        return vkm::VkmKeyCode::Slash;

            case kVK_ANSI_Keypad0: return vkm::VkmKeyCode::Keypad0;
            case kVK_ANSI_Keypad1: return vkm::VkmKeyCode::Keypad1;
            case kVK_ANSI_Keypad2: return vkm::VkmKeyCode::Keypad2;
            case kVK_ANSI_Keypad3: return vkm::VkmKeyCode::Keypad3;
            case kVK_ANSI_Keypad4: return vkm::VkmKeyCode::Keypad4;
            case kVK_ANSI_Keypad5: return vkm::VkmKeyCode::Keypad5;
            case kVK_ANSI_Keypad6: return vkm::VkmKeyCode::Keypad6;
            case kVK_ANSI_Keypad7: return vkm::VkmKeyCode::Keypad7;
            case kVK_ANSI_Keypad8: return vkm::VkmKeyCode::Keypad8;
            case kVK_ANSI_Keypad9: return vkm::VkmKeyCode::Keypad9;

            case kVK_ANSI_KeypadDecimal:  return vkm::VkmKeyCode::KeypadDecimal;
            case kVK_ANSI_KeypadDivide:   return vkm::VkmKeyCode::KeypadDivide;
            case kVK_ANSI_KeypadMultiply: return vkm::VkmKeyCode::KeypadMultiply;
            case kVK_ANSI_KeypadMinus:    return vkm::VkmKeyCode::KeypadSubtract;
            case kVK_ANSI_KeypadPlus:     return vkm::VkmKeyCode::KeypadAdd;
            case kVK_ANSI_KeypadEnter:    return vkm::VkmKeyCode::KeypadEnter;
            case kVK_ANSI_KeypadEquals:   return vkm::VkmKeyCode::KeypadEqual;

            default: return vkm::VkmKeyCode::Unknown;
        }
    }

    uint32_t macModifierFlagsToVkmModifiers(NSEventModifierFlags modifierFlags)
    {
        uint32_t modifiers = 0;
        if ((modifierFlags & NSEventModifierFlagShift) != 0)   { modifiers |= static_cast<uint32_t>(vkm::VkmInputModifier::Shift); }
        if ((modifierFlags & NSEventModifierFlagControl) != 0) { modifiers |= static_cast<uint32_t>(vkm::VkmInputModifier::Control); }
        if ((modifierFlags & NSEventModifierFlagOption) != 0)  { modifiers |= static_cast<uint32_t>(vkm::VkmInputModifier::Alt); }
        if ((modifierFlags & NSEventModifierFlagCommand) != 0) { modifiers |= static_cast<uint32_t>(vkm::VkmInputModifier::Super); }
        return modifiers;
    }

    vkm::VkmMouseButton macButtonNumberToVkmMouseButton(NSInteger buttonNumber)
    {
        switch (buttonNumber)
        {
            case 0:  return vkm::VkmMouseButton::Left;
            case 1:  return vkm::VkmMouseButton::Right;
            case 2:  return vkm::VkmMouseButton::Middle;
            case 3:  return vkm::VkmMouseButton::Button4;
            case 4:  return vkm::VkmMouseButton::Button5;
            default: return vkm::VkmMouseButton::Count;
        }
    }
}

@implementation VkmWindowImpl
{
    vkm::VkmEngine* _engine;
}

- (void)setEngine:(nonnull vkm::VkmEngine *)engine
{
    _engine = engine;
}

- (BOOL)acceptsFirstResponder
{
    return YES;
}

// Key, flags-changed and scroll events that ImGui wants never reach this window: the ImGui
// Metal renderer installs an NSEvent local monitor for exactly those masks and swallows them
// while io.WantCaptureKeyboard/WantCaptureMouse is set. Mouse button and move events are not
// in that mask (ImGui polls them per frame instead), so those are gated explicitly below.

// Overriding this function allows to prevent clicking noise when using keyboard and esc key to go windowed
- (void)keyDown:(NSEvent *)event
{
    if (_engine == nullptr)
    {
        return;
    }

    const vkm::VkmKeyCode key = macKeyCodeToVkmKeyCode((int)event.keyCode);
    if (key != vkm::VkmKeyCode::Unknown)
    {
        _engine->getInputHandler().onKeyEvent(key, vkm::VkmKeyAction::Press,
            macModifierFlagsToVkmModifiers(event.modifierFlags), [event isARepeat]);
    }
}

- (void)keyUp:(NSEvent *)event
{
    if (_engine == nullptr)
    {
        return;
    }

    const vkm::VkmKeyCode key = macKeyCodeToVkmKeyCode((int)event.keyCode);
    if (key != vkm::VkmKeyCode::Unknown)
    {
        _engine->getInputHandler().onKeyEvent(key, vkm::VkmKeyAction::Release,
            macModifierFlagsToVkmModifiers(event.modifierFlags));
    }
}

- (void)flagsChanged:(NSEvent *)event
{
    if (_engine == nullptr)
    {
        return;
    }

    // macOS does not generate down/up events for individual modifier keys, so the per-key
    // state is derived from hardware-dependent masks, mirroring the official imgui_impl_osx
    // backend (see keyEventMonitor in metal_imgui_renderer.mm).
    const vkm::VkmKeyCode key = macKeyCodeToVkmKeyCode((int)event.keyCode);
    NSEventModifierFlags hardwareMask = 0;
    switch (key)
    {
        case vkm::VkmKeyCode::LeftControl:  hardwareMask = 0x0001; break;
        case vkm::VkmKeyCode::RightControl: hardwareMask = 0x2000; break;
        case vkm::VkmKeyCode::LeftShift:    hardwareMask = 0x0002; break;
        case vkm::VkmKeyCode::RightShift:   hardwareMask = 0x0004; break;
        case vkm::VkmKeyCode::LeftSuper:    hardwareMask = 0x0008; break;
        case vkm::VkmKeyCode::RightSuper:   hardwareMask = 0x0010; break;
        case vkm::VkmKeyCode::LeftAlt:      hardwareMask = 0x0020; break;
        case vkm::VkmKeyCode::RightAlt:     hardwareMask = 0x0040; break;
        default: return;
    }

    const uint32_t modifiers = macModifierFlagsToVkmModifiers(event.modifierFlags);
    const bool isDown = (event.modifierFlags & hardwareMask) != 0;
    _engine->getInputHandler().onKeyEvent(key,
        isDown ? vkm::VkmKeyAction::Press : vkm::VkmKeyAction::Release, modifiers);
}

- (void)forwardMouseButtonEvent:(NSEvent *)event action:(vkm::VkmKeyAction)action
{
    if (_engine == nullptr || _engine->wantsCaptureMouse())
    {
        return;
    }

    const vkm::VkmMouseButton button = macButtonNumberToVkmMouseButton(event.buttonNumber);
    if (button != vkm::VkmMouseButton::Count)
    {
        _engine->getInputHandler().onMouseButtonEvent(button, action,
            macModifierFlagsToVkmModifiers(event.modifierFlags));
    }
}

- (void)forwardCursorMoveEvent:(NSEvent *)event
{
    if (_engine == nullptr || _engine->wantsCaptureMouse())
    {
        return;
    }

    // AppKit's origin is bottom-left; flip Y so getCursorY() means the same top-left-origin
    // thing here as it does on the GLFW backends.
    const NSPoint windowPoint = [event locationInWindow];
    const NSRect contentFrame = [self contentLayoutRect];
    _engine->getInputHandler().onCursorMove(windowPoint.x, contentFrame.size.height - windowPoint.y);
}

- (void)mouseDown:(NSEvent *)event       { [self forwardMouseButtonEvent:event action:vkm::VkmKeyAction::Press];   [super mouseDown:event]; }
- (void)mouseUp:(NSEvent *)event         { [self forwardMouseButtonEvent:event action:vkm::VkmKeyAction::Release]; [super mouseUp:event]; }
- (void)rightMouseDown:(NSEvent *)event  { [self forwardMouseButtonEvent:event action:vkm::VkmKeyAction::Press];   [super rightMouseDown:event]; }
- (void)rightMouseUp:(NSEvent *)event    { [self forwardMouseButtonEvent:event action:vkm::VkmKeyAction::Release]; [super rightMouseUp:event]; }
- (void)otherMouseDown:(NSEvent *)event  { [self forwardMouseButtonEvent:event action:vkm::VkmKeyAction::Press];   [super otherMouseDown:event]; }
- (void)otherMouseUp:(NSEvent *)event    { [self forwardMouseButtonEvent:event action:vkm::VkmKeyAction::Release]; [super otherMouseUp:event]; }

- (void)mouseMoved:(NSEvent *)event            { [self forwardCursorMoveEvent:event]; [super mouseMoved:event]; }
- (void)mouseDragged:(NSEvent *)event          { [self forwardCursorMoveEvent:event]; [super mouseDragged:event]; }
- (void)rightMouseDragged:(NSEvent *)event     { [self forwardCursorMoveEvent:event]; [super rightMouseDragged:event]; }
- (void)otherMouseDragged:(NSEvent *)event     { [self forwardCursorMoveEvent:event]; [super otherMouseDragged:event]; }

- (void)scrollWheel:(NSEvent *)event
{
    if (_engine == nullptr)
    {
        return;
    }

    // Line-based wheels report far larger deltas than trackpads; scale them the same way the
    // ImGui Metal renderer does so magnitudes match across devices and backends.
    double wheelDeltaX = event.scrollingDeltaX;
    double wheelDeltaY = event.scrollingDeltaY;
    if ([event hasPreciseScrollingDeltas] == NO)
    {
        wheelDeltaX *= 0.1;
        wheelDeltaY *= 0.1;
    }

    if (wheelDeltaX != 0.0 || wheelDeltaY != 0.0)
    {
        _engine->getInputHandler().onScroll(wheelDeltaX, wheelDeltaY);
    }
}
@end

@implementation VkmApplicationImpl
{
    id<MTLDevice>                   _mtlDevice;
    VkmWindowImpl*                  _window;
    NSView*                         _view;
    CAMetalLayer*                   _metalLayer;
    vkm::VkmEngine*                 _engine;
    RendererCoordinatorController*  _rendererCoordinator;
    const char*                    _appName;
    // Dedicated ImGui window (plain NSWindow) with its own Metal layer + swapchain.
    NSWindow*                       _imguiWindow;
    NSView*                         _imguiView;
    CAMetalLayer*                   _imguiMetalLayer;
}

- (BOOL) applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)sender
{
    return YES;
}

- (void)createWindow
{
    NSWindowStyleMask mask = NSWindowStyleMaskClosable | NSWindowStyleMaskTitled | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    NSScreen * screen = [NSScreen mainScreen];
    NSRect contentRect = NSMakeRect(0, 0, 1280, 720);
    // Center window to the middle of the screen
    contentRect.origin.x = (screen.frame.size.width / 2) - (contentRect.size.width / 2);
    contentRect.origin.y = (screen.frame.size.height / 2) - (contentRect.size.height / 2);
    _window = [[VkmWindowImpl alloc] initWithContentRect:contentRect
                                            styleMask:mask
                                            backing:NSBackingStoreBuffered
                                                defer:NO
                                            screen:screen];
    _window.releasedWhenClosed = NO;
    _window.minSize = NSMakeSize(640, 360);
    _window.delegate = self;
    // Without this, NSWindow never delivers mouseMoved: at all.
    [_window setAcceptsMouseMovedEvents:YES];
    [_window setEngine:_engine];
    [self updateWindowTitle:_window];
    
    vkm::VKM_DEBUG_INFO(fmt::format("Creating window with size: {}x{}", (int)_window.contentLayoutRect.size.width, (int)_window.contentLayoutRect.size.height).c_str());
}

- (void)showWindow
{
    [_window setIsVisible:YES];
    [_window makeMainWindow];
    [_window makeKeyAndOrderFront:_window];
}

// Create the view and Metal layer backing it that renders the game
- (void)createView
{
    NSAssert(_window, @"You need to create the window before the view");
    
    _metalLayer = [[CAMetalLayer alloc] init];
    _metalLayer.device = _mtlDevice;
    
    // Set layer size and make it opaque:
    _metalLayer.drawableSize = NSMakeSize(1920 , 1080);
    _metalLayer.opaque = YES;
    _metalLayer.framebufferOnly = YES;
    
    // Configure CoreAnimation letterboxing:
    _metalLayer.contentsGravity = kCAGravityResizeAspect;
    _metalLayer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);

    // NOTE: pixelFormat / wantsExtendedDynamicRangeContent / colorspace are set by
    // VkmSwapChainMetal::createSwapChain (invoked from addSwapChain below) from the
    // engine-chosen swapchain color format, so the layer, backbuffer, and pipelines agree.

    // Create a view and customize its layer to a MetalLayer where
    // the game can render Metal content:
    _view = [[NSView alloc] initWithFrame:_window.contentLayoutRect];
    _view.layer = _metalLayer;
    _window.contentView = _view;
    
    vkm::VKM_DEBUG_INFO("Metal layer & view setup complete");
    
    vkm::VkmWindowInfo windowInfo;
    windowInfo._width = _metalLayer.drawableSize.width;
    windowInfo._height = _metalLayer.drawableSize.height;
    windowInfo._windowHandle = _metalLayer;
    _engine->addSwapChain(windowInfo, /*isImGuiWindow=*/false);
}

// Create the dedicated ImGui window (a plain NSWindow; it need not receive the ESC handler).
- (void)createImGuiWindow
{
    NSWindowStyleMask mask = NSWindowStyleMaskClosable | NSWindowStyleMaskTitled | NSWindowStyleMaskMiniaturizable | NSWindowStyleMaskResizable;
    NSScreen* screen = [NSScreen mainScreen];
    NSRect contentRect = NSMakeRect(0, 0, 960, 640);
    contentRect.origin.x = (screen.frame.size.width / 2) - (contentRect.size.width / 2) + 700;
    contentRect.origin.y = (screen.frame.size.height / 2) - (contentRect.size.height / 2);
    _imguiWindow = [[NSWindow alloc] initWithContentRect:contentRect
                                               styleMask:mask
                                                 backing:NSBackingStoreBuffered
                                                   defer:NO
                                                  screen:screen];
    _imguiWindow.releasedWhenClosed = NO;
    _imguiWindow.title = @"ImGui";
}

// Create the ImGui window's Metal layer + swapchain. Its pixel format must match the ImGui
// Metal pipeline (RGBA16Float), same as the main layer.
- (void)createImGuiView
{
    NSAssert(_imguiWindow, @"You need to create the ImGui window before its view");

    _imguiMetalLayer = [[CAMetalLayer alloc] init];
    _imguiMetalLayer.device = _mtlDevice;
    _imguiMetalLayer.drawableSize = NSMakeSize(960, 640);
    _imguiMetalLayer.opaque = YES;
    _imguiMetalLayer.framebufferOnly = YES;
    _imguiMetalLayer.contentsGravity = kCAGravityResizeAspect;
    _imguiMetalLayer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    _imguiMetalLayer.pixelFormat = MTLPixelFormatRGBA16Float;
    _imguiMetalLayer.wantsExtendedDynamicRangeContent = YES;
    _imguiMetalLayer.colorspace = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearDisplayP3);

    _imguiView = [[NSView alloc] initWithFrame:_imguiWindow.contentLayoutRect];
    _imguiView.layer = _imguiMetalLayer;
    _imguiWindow.contentView = _imguiView;

    vkm::VkmWindowInfo imguiWindowInfo;
    imguiWindowInfo._width = _imguiMetalLayer.drawableSize.width;
    imguiWindowInfo._height = _imguiMetalLayer.drawableSize.height;
    imguiWindowInfo._windowHandle = _imguiMetalLayer;
    _engine->addSwapChain(imguiWindowInfo, /*isImGuiWindow=*/true);
}

- (void)showImGuiWindow
{
    // orderFront (not makeKey) so the main scene window keeps keyboard focus / ESC handling.
    [_imguiWindow setIsVisible:YES];
    [_imguiWindow orderFront:nil];
}

- (void)createGame
{
    NSAssert(_metalLayer, @"You need to create the MetalLayer before creating the game");
    
    // The canvas size represents a relative amount of screen real estate for UI elements.
    // Since the UI elements are fixed size, on macOS set a canvas larger than on iOS to
    // produce a stylized UI with smaller elements.
    NSUInteger uiCanvasSize = 30;
    _rendererCoordinator = [[RendererCoordinatorController alloc] initWithMetalLayer:_metalLayer
                                                                uiCanvasSize:uiCanvasSize];
    
    [_rendererCoordinator setEngine: _engine];
    [_rendererCoordinator setSwapChain: (vkm::VkmSwapChainMetal*)_engine->getSwapChain(0)];
    [_rendererCoordinator setImGuiSwapChain: (vkm::VkmSwapChainMetal*)_engine->getSwapChain(1)];
    [_rendererCoordinator setImGuiMetalLayer: _imguiMetalLayer];
}

- (void)evaluateCommandLine
{
    NSArray<NSString *>* args = [[NSProcessInfo processInfo] arguments];
    BOOL exitAfterOneFrame = [args containsObject:@"--auto-close"];
    if (exitAfterOneFrame)
    {
        NSLog(@"Automatically terminating in 5 seconds...");
        [[NSApplication sharedApplication] performSelector:@selector(terminate:) withObject:self afterDelay:5];
    }
}

- (void)applicationDidFinishLaunching:(NSNotification *)aNotification
{
    [self createWindow];
    [self createView];
    [self createImGuiWindow];
    [self createImGuiView];
    [self createGame];
    [self showImGuiWindow];
    [self showWindow];
    [self evaluateCommandLine];
    // [self updateMaxEDRValue];
}

- (void)applicationWillTerminate:(NSNotification *)notification
{
    self->_rendererCoordinator = nil;
}

- (void)updateWindowTitle:(nonnull NSWindow *) window
{
    NSScreen* screen = window.screen;
    NSString* title = [NSString stringWithFormat:@"%s (%@ @ %ldHz, EDR max: %.2f)",
                    _appName,
                    screen.localizedName,
                    (long)screen.maximumFramesPerSecond,
                    screen.maximumExtendedDynamicRangeColorComponentValue
    ];
    window.title = title;
}

- (void)setAppName:(nonnull const char *) appName
{
    _appName = appName;
}

- (void)setDevice:(nonnull id<MTLDevice>) device
{
    _mtlDevice = device;
}

- (void)setEngine:(nonnull vkm::VkmEngine*) engine
{
    _engine = engine;
}

- (void)windowDidChangeScreen:(NSNotification *)notification
{
    // [self updateMaxEDRValue];
}

- (void)windowDidResize:(NSNotification *)notification
{
    //
}

- (void)destroy
{
}
@end
#endif // defined(VKM_USE_METAL_API)

namespace vkm
{
#if defined(VKM_USE_METAL_API)
    VkmWindow::VkmWindow()
    {
    }

    VkmWindow::~VkmWindow()
    {
    }

    bool VkmWindow::create(uint32_t width, uint32_t height, const char* title)
    {
        (void)width; (void)height; (void)title;
        return true;
    }

    void VkmWindow::destroy()
    {
    }

    static id<MTLDevice> vkmCreateSystemDefaultDevice()
    {
#if defined(VKM_GPU_CAPTURE)
        // MTL_CAPTURE_ENABLED must be in the environment before the Metal framework
        // initializes its capture layer (first device creation); whether MTLCaptureManager
        // re-consults it lazily is undocumented, so set it deterministically beforehand.
        // Engine launch options are parsed later (entryPoint), so peek at the raw process
        // arguments here. A `--enable-gpu-capture=false`-style argument also matches the
        // prefix and sets the env var -- benign, since the env only enables the capture
        // *capability*; capture-scope creation stays gated on the parsed flag.
        // overwrite=0 respects an explicit user-provided MTL_CAPTURE_ENABLED value.
        for (NSString* arg in [[NSProcessInfo processInfo] arguments])
        {
            if ([arg hasPrefix:@"--enable-gpu-capture"] || [arg hasPrefix:@"--gpu-capture-frame"])
            {
                setenv("MTL_CAPTURE_ENABLED", "1", 0);
                break;
            }
        }
#endif // VKM_GPU_CAPTURE
        return MTLCreateSystemDefaultDevice();
    }

    VkmApplication::VkmApplication()
        : _mtlDevice(vkmCreateSystemDefaultDevice()), _engine( new vkm::VkmDriverMetal(_mtlDevice) )
    {
    }

    VkmApplication::~VkmApplication()
    {
        if ( (::VkmApplicationImpl*)_impl != nullptr )
        {
            [(__bridge ::VkmApplicationImpl*)_impl destroy];
            [(__bridge ::VkmApplicationImpl*)_impl release];
            _impl = nullptr;
        }
    }

    int VkmApplication::entryPoint(AppDelegate* appDelegate, int argc, char* argv[])
    {
        {
            // Note : initialize engine first for logger manager initialization precede

            if (_engine.initializeEngine( appDelegate, VkmEngine::parseEngineLaunchOptions(argc, argv) ) == false)
            {
                VKM_DEBUG_ERROR("Failed to initialize VkmEngine for apple platform");
                return -1;
            }

            VkmInitResult driverInitResult = _engine.initializeBackendDriver();
            if (driverInitResult.code != VkmInitResultCode::Success)
            {
                VKM_DEBUG_ERROR(fmt::format("Failed to initialize backend metal api: {}", driverInitResult.reason).c_str());
                return -1;
            }

            _impl = (__bridge vkm::VkmApplicationImpl*)[[::VkmApplicationImpl alloc] init];
            if ( _impl == nil )
            {
                VKM_DEBUG_ERROR("Failed to initialize VkmApplication for apple platform");
                return -1;
            }

            [((::VkmApplicationImpl*)_impl) setAppName:appDelegate->getAppName()];
            [((::VkmApplicationImpl*)_impl) setDevice:(id <MTLDevice>)_mtlDevice];
            [((::VkmApplicationImpl*)_impl) setEngine:&_engine];
        }

        NSApplication* app = [NSApplication sharedApplication];
        [app setDelegate: (::VkmApplicationImpl*)_impl];
        return NSApplicationMain(argc, (const char**)argv);
    }

    void VkmApplication::destroy()
    {
    }
#elif defined(VKM_USE_VULKAN_API)
    VkmWindow::VkmWindow()
        : _windowHandle( nullptr )
    {
    }

    VkmWindow::~VkmWindow()
    {
    }

    bool VkmWindow::create(uint32_t width, uint32_t height, const char* title)
    {
        _windowHandle = glfwCreateWindow(width, height, title, nullptr, nullptr);
        VKM_ASSERT(_windowHandle, fmt::format("Failed to create window(Width : {}, Height : {}, Title : {})", width, height, title).c_str());

        return true;
    }

    void VkmWindow::destroy()
    {
    }

    void VkmWindow::update()
    {
        glfwPollEvents();
    }

    bool VkmWindow::shouldClose() const
    {
        return glfwWindowShouldClose(_windowHandle);
    }

    VkmApplication::VkmApplication()
        : _engine( new VkmDriverVulkan() )
    {
    }

    VkmApplication::~VkmApplication()
    {
        destroy();
    }

    int VkmApplication::entryPoint(AppDelegate* appDelegate, int argc, char* argv[])
    {
        _appName = appDelegate->getAppName();

        if ( _engine.initializeEngine( appDelegate, VkmEngine::parseEngineLaunchOptions(argc, argv)) == false )
        {
            VKM_DEBUG_ERROR("Failed to initialize engine");
            return -1;
        }

        // On modern macOS, bare-name dlopen no longer searches /usr/local/lib, where the Vulkan
        // SDK installs the loader, so GLFW's own probe in glfwVulkanSupported() fails to find it.
        // volkInitialize() locates the loader (it has an explicit /usr/local/lib fallback) and
        // populates vkGetInstanceProcAddr; hand that to GLFW so it skips its own dlopen. A later
        // volkInitialize() in the driver is harmless (it just re-resolves the loader).
        if (volkInitialize() != VK_SUCCESS)
        {
            VKM_DEBUG_ERROR("Failed to initialize the Vulkan loader");
            return -1;
        }
        glfwInitVulkanLoader(vkGetInstanceProcAddr);

        VKM_ASSERT(glfwInit(), "Failed to initialize GLFW");
        VKM_ASSERT(glfwVulkanSupported(), "This system does not support Vulkan API");

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

        VkmInitResult driverInitResult = _engine.initializeBackendDriver();
        if ( driverInitResult.code != VkmInitResultCode::Success )
        {
            VKM_DEBUG_ERROR(fmt::format("Failed to initialize backend vulkan api: {}", driverInitResult.reason).c_str());
            return -1;
        }

        if ( _window.create(  1280, 720, _appName ) == false )
        {
            VKM_DEBUG_ERROR("Failed to initialize window");
            return -1;
        }
        else
        {
            VkmWindowInfo windowInfo = { 1280, 720, _appName, _window.getHandle() };
            _engine.addSwapChain(windowInfo, /*isImGuiWindow=*/false);
        }

        // Dedicated ImGui window with its own swapchain. ImGui installs its own GLFW input
        // callbacks on this window during addSwapChain(..., true).
        if ( _imguiWindow.create( 960, 640, "ImGui" ) == false )
        {
            VKM_DEBUG_ERROR("Failed to initialize ImGui window");
            return -1;
        }
        else
        {
            VkmWindowInfo imguiWindowInfo = { 960, 640, "ImGui", _imguiWindow.getHandle() };
            _engine.addSwapChain(imguiWindowInfo, /*isImGuiWindow=*/true);
        }

        installGlfwInputCallbacks(_window.getHandle(), &_engine);

        // The app quits when the main window closes; closing the ImGui window is vetoed so its
        // swapchain need not be torn down mid-run.
        while (_window.shouldClose() == false && _engine.shouldExit() == false)
        {
            glfwPollEvents(); // services every window; call once per frame
            if (_imguiWindow.shouldClose())
            {
                glfwSetWindowShouldClose(_imguiWindow.getHandle(), GLFW_FALSE);
            }
            _engine.loopInner(glfwGetTime());
        }

        glfwTerminate();
        return 0;
    }

    void VkmApplication::destroy()
    {
        _window.destroy();
        _engine.destroy();
    }
#endif
} // namespace vkm
