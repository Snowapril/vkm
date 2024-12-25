// Copyright (c) 2024 Snowapril

#include <vkm/platform/apple/application.h>
#include <vkm/renderer/engine.h>
#include <vkm/renderer/backend/metal/metal_driver.h>

#if TARGET_OS_IOS
#import <UIKit/UIKit.h>
#endif

#include <Metal/Metal.h>
#include <QuartzCore/QuartzCore.h>
#include <QuartzCore/CAMetalLayer.h>
#include <QuartzCore/CAFrameRateRange.h>

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
    std::unique_ptr<vkm::VkmEngine>      _engine;
    CAMetalLayer*                   _metalLayer;
    CAMetalDisplayLink*             _metalDisplayLink;
}

- (nonnull instancetype)initWithMetalLayer:(nonnull CAMetalLayer *)metalLayer uiCanvasSize:(NSUInteger)uiCanvasSize
{
    self = [super init];
    if(self)
    {
        _metalLayer = metalLayer;
        
        _engine = std::make_unique<vkm::VkmEngine>( new vkm::VkmDriverMetal((__bridge MTLDevice *)_metalLayer.device) );
        // TODO(snowapril) : init swapchain in driver base with ui canvas size and pixel format

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
    _engine.reset();
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
    
    // id<CAMetalDrawable> drawable = update.drawable;
    // TODO(snowapril)
    // _engine->draw((__bridge CA::MetalDrawable *)drawable, CACurrentMediaTime());
}
@end

@interface VkmWindow : NSWindow
@end

@implementation VkmWindow
// Overriding this function allows to prevent clicking noise when using keyboard and esc key to go windowed
- (void)keyDown:(NSEvent *)event
{
}
@end

@implementation VkmApplication
{
    VkmWindow*                _window;
    NSView*                   _view;
    CAMetalLayer*             _metalLayer;
    vkm::VkmEngine*            _engine;
    RendererCoordinatorController* _rendererCoordinator;
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
    _window = [[VkmWindow alloc] initWithContentRect:contentRect
                                            styleMask:mask
                                            backing:NSBackingStoreBuffered
                                                defer:NO
                                            screen:screen];
    _window.releasedWhenClosed = NO;
    _window.minSize = NSMakeSize(640, 360);
    _window.delegate = self;
    [self updateWindowTitle:_window];
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
    _metalLayer.device = MTLCreateSystemDefaultDevice();
    
    // Set layer size and make it opaque:
    _metalLayer.drawableSize = NSMakeSize(1920 , 1080);
    _metalLayer.opaque = YES;
    _metalLayer.framebufferOnly = YES;
    
    // Configure CoreAnimation letterboxing:
    _metalLayer.contentsGravity = kCAGravityResizeAspect;
    _metalLayer.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    
    // Prepare the layer for EDR rendering:
    _metalLayer.pixelFormat = MTLPixelFormatRGBA16Float;
    _metalLayer.wantsExtendedDynamicRangeContent = YES;
    _metalLayer.colorspace = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearDisplayP3);
    // Create a view and customize its layer to a MetalLayer where
    // the game can render Metal content:
    _view = [[NSView alloc] initWithFrame:_window.contentLayoutRect];
    _view.layer = _metalLayer;
    _window.contentView = _view;
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
    [self createGame];
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
    NSString* title = [NSString stringWithFormat:@"Game Project (%@ @ %ldHz, EDR max: %.2f)",
                    screen.localizedName,
                    (long)screen.maximumFramesPerSecond,
                    screen.maximumExtendedDynamicRangeColorComponentValue
    ];
    window.title = title;
}

- (void)windowDidChangeScreen:(NSNotification *)notification
{
    // [self updateMaxEDRValue];
}

- (void)windowDidResize:(NSNotification *)notification
{
    //
}

- (int) entryPoint:(int)argc argv:(const char*[])argv
{
    return NSApplicationMain(argc, argv);
}

- (void)destroy
{
}
@end