// Copyright (c) 2025 Snowapril
//
// Camera capture and hand tracking on Apple platforms: AVFoundation delivers BGRA frames, and
// Vision's VNDetectHumanHandPoseRequest turns them into 21 joints.
//
// Vision is what makes this sample cheap. The alternative -- estimating a depth map with a
// monocular network -- needs a model file, a conversion pipeline and a lot more compute, and
// returns relative depth that is too unstable to drive a collision test. A hand pose request
// needs none of that, ships with the OS, and returns exactly the skeleton the simulation wants.
//
// Three threads meet here. AVFoundation calls the delegate on its own serial queue, detection
// runs on a second serial queue, and the engine loop polls through tryAcquireFrame/
// tryAcquirePose. Both handoffs are a mutex around a "latest value plus a sequence number",
// which is what makes a poll cheap and lets the producers drop work rather than queue it.

#include "hand_input.h"

#include <vkm/base/common.h>

#import <AVFoundation/AVFoundation.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <TargetConditionals.h>
#import <Vision/Vision.h>

#include <atomic>
#include <cstring>
#include <mutex>

namespace vkm
{
    namespace
    {
        // A joint Vision is less sure of than this is treated as not seen at all. Low-confidence
        // points wander by tens of pixels between frames, which reads as a fingertip teleporting
        // through the ball.
        constexpr float kJointConfidenceFloor = 0.30f;

        class AppleHandInput;
    } // namespace
} // namespace vkm

/*
* @brief Forwards AVFoundation's sample buffers to the C++ source that owns the session.
*/
@interface VkmHandInteractionCaptureDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate>
@property(nonatomic, assign) vkm::AppleHandInput* owner;
@end

namespace vkm
{
    namespace
    {
        /*
        * @brief Vision's joint names in HandJoint order, so the two enums can be zipped.
        */
        NSArray<VNHumanHandPoseObservationJointName>* handJointNames()
        {
            // Retained: these sources compile without ARC, and an array literal is autoreleased,
            // so a plain static would dangle after the first pool drain.
            static NSArray<VNHumanHandPoseObservationJointName>* names = [@[
                VNHumanHandPoseObservationJointNameWrist,
                VNHumanHandPoseObservationJointNameThumbCMC,
                VNHumanHandPoseObservationJointNameThumbMP,
                VNHumanHandPoseObservationJointNameThumbIP,
                VNHumanHandPoseObservationJointNameThumbTip,
                VNHumanHandPoseObservationJointNameIndexMCP,
                VNHumanHandPoseObservationJointNameIndexPIP,
                VNHumanHandPoseObservationJointNameIndexDIP,
                VNHumanHandPoseObservationJointNameIndexTip,
                VNHumanHandPoseObservationJointNameMiddleMCP,
                VNHumanHandPoseObservationJointNameMiddlePIP,
                VNHumanHandPoseObservationJointNameMiddleDIP,
                VNHumanHandPoseObservationJointNameMiddleTip,
                VNHumanHandPoseObservationJointNameRingMCP,
                VNHumanHandPoseObservationJointNameRingPIP,
                VNHumanHandPoseObservationJointNameRingDIP,
                VNHumanHandPoseObservationJointNameRingTip,
                VNHumanHandPoseObservationJointNameLittleMCP,
                VNHumanHandPoseObservationJointNameLittlePIP,
                VNHumanHandPoseObservationJointNameLittleDIP,
                VNHumanHandPoseObservationJointNameLittleTip,
            ] retain];
            return names;
        }

        class AppleHandInput final : public HandInputSource
        {
        public:
            ~AppleHandInput() override { stop(); }

            bool start(std::string* outError) override;
            void stop() override;
            bool tryAcquireFrame(CameraFrame* outFrame) override;
            bool tryAcquirePose(HandPose* outPose) override;
            const char* getName() const override { return "Camera + Vision hand pose"; }

            // Called on AVFoundation's capture queue.
            void onSampleBuffer(CMSampleBufferRef sampleBuffer);

        private:
            bool configureSession(std::string* outError);
            void publishFrame(CVPixelBufferRef pixelBuffer);
            void detect(CVPixelBufferRef pixelBuffer);
            void publishPose(const HandPose& pose);

            AVCaptureSession* _session = nil;
            AVCaptureVideoDataOutput* _output = nil;
            VkmHandInteractionCaptureDelegate* _delegate = nil;
            VNDetectHumanHandPoseRequest* _request = nil;
            dispatch_queue_t _captureQueue = nullptr;
            dispatch_queue_t _detectQueue = nullptr;
            dispatch_queue_t _sessionQueue = nullptr;

            std::mutex _frameMutex;
            CameraFrame _latestFrame;
            uint64_t _frameSequence = 0;
            uint64_t _consumedFrameSequence = 0;

            std::mutex _poseMutex;
            HandPose _latestPose;
            uint64_t _poseSequence = 0;
            uint64_t _consumedPoseSequence = 0;

            // Detection is slower than capture, so at most one runs at a time and the frames that
            // arrive meanwhile are dropped. Queueing them instead would only grow a backlog of
            // poses that are already stale by the time they are produced.
            std::atomic<bool> _detecting{ false };
        };

        bool AppleHandInput::start(std::string* outError)
        {
            const AVAuthorizationStatus status = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
            if (status == AVAuthorizationStatusDenied || status == AVAuthorizationStatusRestricted)
            {
                *outError = "camera access is denied for this app (System Settings > Privacy & Security > Camera)";
                return false;
            }
            if (status == AVAuthorizationStatusNotDetermined)
            {
                // Deliberately does not wait for the answer. postDriverReady runs before the app's
                // run loop starts, so blocking here would stall the very loop that has to service
                // the permission prompt.
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
            // devices. It runs on its own queue so the window still appears immediately; the
            // session simply delivers nothing until it is ready.
            dispatch_async(_sessionQueue, ^{
                [_session startRunning];
            });
            return true;
        }

        bool AppleHandInput::configureSession(std::string* outError)
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

            // 720p is far more than the hand pose request needs and still uploads in well under a
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

            _delegate = [[VkmHandInteractionCaptureDelegate alloc] init];
            _delegate.owner = this;

            _captureQueue = dispatch_queue_create("vkm.hand_interaction.capture", DISPATCH_QUEUE_SERIAL);
            _detectQueue = dispatch_queue_create("vkm.hand_interaction.detect", DISPATCH_QUEUE_SERIAL);
            // Serial, so the start and the stop of the session cannot overlap.
            _sessionQueue = dispatch_queue_create("vkm.hand_interaction.session", DISPATCH_QUEUE_SERIAL);

            _output = [[AVCaptureVideoDataOutput alloc] init];
            // BGRA because it is what the capture hardware produces without a conversion pass;
            // hand_background.hlsl swizzles the channels back for free.
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

            _request = [[VNDetectHumanHandPoseRequest alloc] init];
            _request.maximumHandCount = 1;

            return true;
        }

        void AppleHandInput::stop()
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

            // Both queues are serial, so an empty block that has run means everything queued ahead
            // of it has finished touching this object.
            if (_captureQueue != nullptr)
            {
                dispatch_sync(_captureQueue, ^{});
            }
            if (_detectQueue != nullptr)
            {
                dispatch_sync(_detectQueue, ^{});
            }

            // Compiled without ARC, like the Metal backend, so every alloc here has its release.
            [_session release];
            [_output release];
            [_delegate release];
            [_request release];
            _session = nil;
            _output = nil;
            _delegate = nil;
            _request = nil;

            if (_captureQueue != nullptr)
            {
                dispatch_release(_captureQueue);
                _captureQueue = nullptr;
            }
            if (_detectQueue != nullptr)
            {
                dispatch_release(_detectQueue);
                _detectQueue = nullptr;
            }
            if (_sessionQueue != nullptr)
            {
                dispatch_release(_sessionQueue);
                _sessionQueue = nullptr;
            }
        }

        void AppleHandInput::onSampleBuffer(CMSampleBufferRef sampleBuffer)
        {
            @autoreleasepool
            {
            CVImageBufferRef imageBuffer = CMSampleBufferGetImageBuffer(sampleBuffer);
            if (imageBuffer == nullptr)
            {
                return;
            }
            CVPixelBufferRef pixelBuffer = static_cast<CVPixelBufferRef>(imageBuffer);

            publishFrame(pixelBuffer);

            if (_detecting.exchange(true))
            {
                return; // a detection is already in flight; this frame is dropped
            }

            CVPixelBufferRetain(pixelBuffer);
            dispatch_async(_detectQueue, ^{
                @autoreleasepool
                {
                    detect(pixelBuffer);
                }
                CVPixelBufferRelease(pixelBuffer);
                _detecting.store(false);
            });
            }
        }

        void AppleHandInput::publishFrame(CVPixelBufferRef pixelBuffer)
        {
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
                _latestFrame._pixels.resize(rowBytes * height);
                // uploadToTexture takes tightly packed pixels, and a capture buffer's rows are
                // padded out to the hardware's alignment, so the rows are copied one at a time.
                for (size_t row = 0; row < height; ++row)
                {
                    std::memcpy(_latestFrame._pixels.data() + row * rowBytes, source + row * sourceStride, rowBytes);
                }
                _latestFrame._sequence = ++_frameSequence;
            }

            CVPixelBufferUnlockBaseAddress(pixelBuffer, kCVPixelBufferLock_ReadOnly);
        }

        void AppleHandInput::detect(CVPixelBufferRef pixelBuffer)
        {
            VNImageRequestHandler* handler =
                [[VNImageRequestHandler alloc] initWithCVPixelBuffer:pixelBuffer
                                                          orientation:kCGImagePropertyOrientationUp
                                                              options:@{}];

            NSError* requestError = nil;
            const BOOL performed = [handler performRequests:@[ _request ] error:&requestError];
            [handler release];
            if (!performed)
            {
                VKM_DEBUG_ERROR(requestError != nil ? requestError.localizedDescription.UTF8String
                                                    : "VNDetectHumanHandPoseRequest failed");
                return;
            }

            NSArray<VNHumanHandPoseObservation*>* observations = _request.results;
            if (observations.count == 0)
            {
                publishPose(HandPose{});
                return;
            }

            NSError* pointsError = nil;
            NSDictionary<VNHumanHandPoseObservationJointName, VNRecognizedPoint*>* points =
                [observations.firstObject recognizedPointsForGroupKey:VNHumanHandPoseObservationJointsGroupNameAll
                                                                error:&pointsError];
            if (points == nil)
            {
                VKM_DEBUG_ERROR(pointsError != nil ? pointsError.localizedDescription.UTF8String
                                                   : "the hand observation carried no joints");
                publishPose(HandPose{});
                return;
            }

            NSArray<VNHumanHandPoseObservationJointName>* names = handJointNames();

            HandPose pose;
            for (uint32_t i = 0; i < kHandJointCount; ++i)
            {
                VNRecognizedPoint* point = points[names[i]];
                if (point == nil)
                {
                    continue;
                }
                // Vision normalizes to the image with a bottom-left origin. HandPose is top-left,
                // and mirrored to match the mirrored background, so both axes invert.
                pose._joints[i] = glm::vec2(1.0f - static_cast<float>(point.x), 1.0f - static_cast<float>(point.y));
                pose._confidence[i] = point.confidence;
            }

            // Only the joints the simulation actually reads have to be trustworthy: a hidden
            // knuckle in the middle of the hand is not a reason to drop the whole pose.
            bool usable = true;
            for (const HandJoint joint : kHandFingertips)
            {
                usable = usable && pose._confidence[static_cast<size_t>(joint)] >= kJointConfidenceFloor;
            }
            for (const HandJoint joint : kHandPalmJoints)
            {
                usable = usable && pose._confidence[static_cast<size_t>(joint)] >= kJointConfidenceFloor;
            }
            pose._valid = usable;

            publishPose(pose);
        }

        void AppleHandInput::publishPose(const HandPose& pose)
        {
            std::lock_guard<std::mutex> lock(_poseMutex);
            _latestPose = pose;
            ++_poseSequence;
        }

        bool AppleHandInput::tryAcquireFrame(CameraFrame* outFrame)
        {
            std::lock_guard<std::mutex> lock(_frameMutex);
            if (_frameSequence == _consumedFrameSequence)
            {
                return false;
            }
            _consumedFrameSequence = _frameSequence;
            // Copy-assign rather than move: the caller's buffer is reused every frame, so after
            // the first frame this is one memcpy with no allocation.
            outFrame->_pixels = _latestFrame._pixels;
            outFrame->_width = _latestFrame._width;
            outFrame->_height = _latestFrame._height;
            outFrame->_sequence = _latestFrame._sequence;
            return true;
        }

        bool AppleHandInput::tryAcquirePose(HandPose* outPose)
        {
            std::lock_guard<std::mutex> lock(_poseMutex);
            if (_poseSequence == _consumedPoseSequence)
            {
                return false;
            }
            _consumedPoseSequence = _poseSequence;
            *outPose = _latestPose;
            return true;
        }
    } // namespace

    HandInputSource* createPlatformHandInput()
    {
        return new AppleHandInput();
    }
} // namespace vkm

@implementation VkmHandInteractionCaptureDelegate

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
