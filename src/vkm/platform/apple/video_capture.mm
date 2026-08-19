// Copyright (c) 2025 Snowapril
//
// AVFoundation camera capture. Frames arrive as BGRA on AVFoundation's own serial queue and are
// published under a mutex as "latest frame plus a sequence number", which is what makes
// tryAcquireFrame() a cheap poll and lets the producer drop frames rather than queue them.

#include <vkm/platform/common/video_capture.h>

#include <vkm/base/common.h>

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <TargetConditionals.h>

#include <cstring>
#include <mutex>

namespace vkm
{
    namespace
    {
        class AppleVideoCapture;
    } // namespace
} // namespace vkm

/*
* @brief Forwards AVFoundation's sample buffers to the capture object that owns the session.
*/
@interface VkmVideoCaptureDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property(nonatomic, assign) vkm::AppleVideoCapture* owner;
@end

namespace vkm
{
    namespace
    {
        class AppleVideoCapture final : public VkmVideoCaptureBase
        {
        public:
            ~AppleVideoCapture() override { stop(); }

            bool start(std::string* outError) override;
            void stop() override;
            bool tryAcquireFrame(VkmVideoFrame* outFrame) override;
            const char* getName() const override { return "AVFoundation camera"; }

            // Called on AVFoundation's capture queue.
            void onSampleBuffer(CMSampleBufferRef sampleBuffer);

        private:
            bool configureSession(std::string* outError);

            AVCaptureSession* _session = nil;
            AVCaptureVideoDataOutput* _output = nil;
            VkmVideoCaptureDelegate* _delegate = nil;
            dispatch_queue_t _captureQueue = nullptr;
            dispatch_queue_t _sessionQueue = nullptr;

            std::mutex _frameMutex;
            VkmVideoFrame _latestFrame;
            uint64_t _frameSequence = 0;
            uint64_t _consumedSequence = 0;
        };

        bool AppleVideoCapture::start(std::string* outError)
        {
            const AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
            if (status == AVAuthorizationStatusDenied || status == AVAuthorizationStatusRestricted)
            {
                *outError = "camera access is denied for this app (System Settings > Privacy & Security > Camera)";
                return false;
            }
            if (status == AVAuthorizationStatusNotDetermined)
            {
                // Deliberately does not wait for the answer. An app typically starts capture
                // before its run loop is running, and blocking here would stall the very loop
                // that has to service the permission prompt.
                [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL granted) {
                    VKM_DEBUG_LOG(granted ? "Camera access granted; restart to use the camera"
                                          : "Camera access refused");
                }];
                *outError = "camera access has not been granted yet; answer the prompt and restart";
                return false;
            }

            if (!configureSession(outError))
            {
                stop();
                return false;
            }

            // startRunning blocks until the camera has powered up, which is seconds on some
            // devices. It runs on its own queue so a caller's startup is not held up; the session
            // simply delivers nothing until it is ready.
            dispatch_async(_sessionQueue, ^{
                [_session startRunning];
            });
            return true;
        }

        bool AppleVideoCapture::configureSession(std::string* outError)
        {
#if TARGET_OS_OSX
            NSArray<AVCaptureDeviceType>* deviceTypes =
                @[ AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeExternal ];
#else
            NSArray<AVCaptureDeviceType>* deviceTypes = @[ AVCaptureDeviceTypeBuiltInWideAngleCamera ];
#endif
            AVCaptureDeviceDiscoverySession* discovery =
                [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:deviceTypes
                                                                       mediaType:AVMediaTypeVideo
                                                                        position:AVCaptureDevicePositionUnspecified];
            AVCaptureDevice* device = discovery.devices.firstObject;
            if (device == nil)
            {
                *outError = "no video capture device found";
                return false;
            }

            NSError* deviceError = nil;
            AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&deviceError];
            if (input == nil)
            {
                *outError = deviceError != nil ? deviceError.localizedDescription.UTF8String
                                               : "could not open the capture device";
                return false;
            }

            _session = [[AVCaptureSession alloc] init];
            [_session beginConfiguration];

            // 720p is more than a hand pose request needs and still uploads in well under a
            // millisecond; a higher preset buys nothing but bandwidth.
            if ([_session canSetSessionPreset:AVCaptureSessionPreset1280x720])
            {
                _session.sessionPreset = AVCaptureSessionPreset1280x720;
            }

            if (![_session canAddInput:input])
            {
                [_session commitConfiguration];
                *outError = "the capture session refused the device input";
                return false;
            }
            [_session addInput:input];

            _delegate = [[VkmVideoCaptureDelegate alloc] init];
            _delegate.owner = this;

            _captureQueue = dispatch_queue_create("vkm.video_capture.frames", DISPATCH_QUEUE_SERIAL);
            // Serial, so the start and the stop of the session cannot overlap.
            _sessionQueue = dispatch_queue_create("vkm.video_capture.session", DISPATCH_QUEUE_SERIAL);

            _output = [[AVCaptureVideoDataOutput alloc] init];
            // BGRA is what the capture hardware produces without a conversion pass, and
            // VkmFormat::BGRA8_UNORM exists on every backend, so the bytes never need reordering.
            _output.videoSettings = @{ (NSString*)kCVPixelBufferPixelFormatTypeKey : @(kCVPixelFormatType_32BGRA) };
            _output.alwaysDiscardsLateVideoFrames = YES;
            [_output setSampleBufferDelegate:_delegate queue:_captureQueue];

            if (![_session canAddOutput:_output])
            {
                [_session commitConfiguration];
                *outError = "the capture session refused the video data output";
                return false;
            }
            [_session addOutput:_output];

            [_session commitConfiguration];
            return true;
        }

        void AppleVideoCapture::stop()
        {
            if (_session != nil && _sessionQueue != nullptr)
            {
                // Ordered behind whatever startRunning is still doing on this queue.
                dispatch_sync(_sessionQueue, ^{
                    [_session stopRunning];
                });
            }
            if (_output != nil)
            {
                [_output setSampleBufferDelegate:nil queue:nil];
            }
            // The queue is serial, so an empty block that has run means everything queued ahead of
            // it has finished touching this object.
            if (_captureQueue != nullptr)
            {
                dispatch_sync(_captureQueue, ^{});
            }

            // Compiled without ARC, like the rest of the Apple platform layer.
            [_session release];
            [_output release];
            [_delegate release];
            _session = nil;
            _output = nil;
            _delegate = nil;

            if (_captureQueue != nullptr)
            {
                dispatch_release(_captureQueue);
                _captureQueue = nullptr;
            }
            if (_sessionQueue != nullptr)
            {
                dispatch_release(_sessionQueue);
                _sessionQueue = nullptr;
            }
        }

        void AppleVideoCapture::onSampleBuffer(CMSampleBufferRef sampleBuffer)
        {
            @autoreleasepool
            {
                CVImageBufferRef imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
                if (imageBuffer == nullptr)
                {
                    return;
                }
                CVPixelBufferRef pixelBuffer = static_cast<CVPixelBufferRef>(imageBuffer);

                const CVReturn locked = CVPixelBufferLockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
                if (locked != kCVReturnSuccess)
                {
                    VKM_DEBUG_ERROR("CVPixelBufferLockBaseAddress failed for a captured frame");
                    return;
                }

                const size_t width = CVPixelBufferGetWidth(pixelBuffer);
                const size_t height = CVPixelBufferGetHeight(pixelBuffer);
                const size_t sourceStride = CVPixelBufferGetBytesPerRow(pixelBuffer);
                const uint8_t* source = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(pixelBuffer));

                if (source != nullptr && width > 0 && height > 0)
                {
                    const size_t rowBytes = width * 4;
                    std::lock_guard<std::mutex> lock(_frameMutex);
                    _latestFrame._width = static_cast<uint32_t>(width);
                    _latestFrame._height = static_cast<uint32_t>(height);
                    _latestFrame._format = VkmFormat::BGRA8_UNORM;
                    _latestFrame._pixels.resize(rowBytes * height);
                    // uploadToTexture takes tightly packed pixels, and a capture buffer's rows are
                    // padded out to the hardware's alignment, so the rows are copied one at a time.
                    for (size_t row = 0; row < height; ++row)
                    {
                        std::memcpy(_latestFrame._pixels.data() + row * rowBytes,
                                    source + row * sourceStride, rowBytes);
                    }
                    _latestFrame._sequence = ++_frameSequence;
                }

                CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
            }
        }

        bool AppleVideoCapture::tryAcquireFrame(VkmVideoFrame* outFrame)
        {
            std::lock_guard<std::mutex> lock(_frameMutex);
            if (_frameSequence == _consumedSequence)
            {
                return false;
            }
            _consumedSequence = _frameSequence;
            // Copy-assign rather than move: the caller's buffer is reused every frame, so after
            // the first frame this is one memcpy with no allocation.
            outFrame->_pixels = _latestFrame._pixels;
            outFrame->_width = _latestFrame._width;
            outFrame->_height = _latestFrame._height;
            outFrame->_format = _latestFrame._format;
            outFrame->_sequence = _latestFrame._sequence;
            return true;
        }
    } // namespace

    VkmVideoCaptureBase* vkmCreateVideoCapture()
    {
        return new AppleVideoCapture();
    }
} // namespace vkm

@implementation VkmVideoCaptureDelegate

- (void)captureOutput:(AVCaptureOutput*)output
    didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
           fromConnection:(AVCaptureConnection*)connection
{
    (void)output;
    (void)connection;
    if (self.owner != nullptr)
    {
        self.owner->onSampleBuffer(sampleBuffer);
    }
}

@end
