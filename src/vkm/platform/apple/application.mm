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
#elif defined(VKM_USE_VULKAN_API)
// vulkan_driver.h (volk) must precede glfw3.h: glfw3.h only declares
// glfwInitVulkanLoader() when the Vulkan API types are already visible.
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <GLFW/glfw3.h>
#endif

#include <vkm/platform/common/window.h>

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
@end

@implementation VkmWindowImpl
{
    vkm::VkmEngine* _engine;
}

- (void)setEngine:(nonnull vkm::VkmEngine *)engine
{
    _engine = engine;
}

// Overriding this function allows to prevent clicking noise when using keyboard and esc key to go windowed
- (void)keyDown:(NSEvent *)event
{
    if (_engine != nullptr && event.keyCode == 53) // kVK_Escape (avoids a Carbon.h dependency for one constant)
    {
        _engine->getInputHandler().onKeyEvent(vkm::VkmKeyCode::Escape, vkm::VkmKeyAction::Press);
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
    
    // Prepare the layer for EDR rendering:
    _metalLayer.pixelFormat = MTLPixelFormatRGBA16Float;
    _metalLayer.wantsExtendedDynamicRangeContent = YES;
    _metalLayer.colorspace = CGColorSpaceCreateWithName(kCGColorSpaceExtendedLinearDisplayP3);
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
    _engine->addSwapChain(windowInfo);
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
    [_rendererCoordinator setSwapChain: (vkm::VkmSwapChainMetal*)_engine->getMainSwapChain()];
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

    VkmApplication::VkmApplication()
        : _mtlDevice(MTLCreateSystemDefaultDevice()), _engine( new vkm::VkmDriverMetal(_mtlDevice) )
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
            _engine.addSwapChain(windowInfo);
        }

        glfwSetWindowUserPointer(_window.getHandle(), &_engine);
        glfwSetKeyCallback(_window.getHandle(), [](GLFWwindow* window, int key, int /*scancode*/, int action, int /*mods*/)
        {
            if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS)
            {
                static_cast<VkmEngine*>(glfwGetWindowUserPointer(window))->getInputHandler().onKeyEvent(VkmKeyCode::Escape, VkmKeyAction::Press);
            }
        });

        while (_window.shouldClose() == false && _engine.shouldExit() == false)
        {
            _window.update();
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
