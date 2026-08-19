// Copyright (c) 2025 Snowapril
//
// Hand tracking through Vision's VNDetectHumanHandPoseRequest, which returns the 21-joint
// skeleton VkmHandPose carries straight out of an ordinary RGB frame.
//
// Vision is what makes this cheap. The alternative -- estimating a depth map with a monocular
// network -- needs a model file, a conversion pipeline and a lot more compute, and returns
// relative depth too unstable to drive anything downstream. A hand pose request needs none of
// that and ships with the OS.

#include <vkm/platform/common/hand_tracker.h>

#include <vkm/base/common.h>

#import <CoreVideo/CoreVideo.h>
#import <Vision/Vision.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

namespace vkm
{
    namespace
    {
        // A joint Vision is less sure of than this is treated as not seen at all. Low-confidence
        // points wander by tens of pixels between frames, which reads as a joint teleporting.
        constexpr float kJointConfidenceFloor = 0.30f;

        /*
        * @brief Vision's joint names in VkmHandJoint order, so the two can be zipped.
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

        /*
        * @brief The CoreVideo pixel format matching a captured frame's channel order.
        * @return 0 when the format is not one Vision can be handed directly.
        */
        OSType coreVideoPixelFormat(VkmFormat format)
        {
            switch (format)
            {
                case VkmFormat::BGRA8_UNORM:    return kCVPixelFormatType_32BGRA;
                case VkmFormat::R8G8B8A8_UNORM: return kCVPixelFormatType_32RGBA;
                default:                        return 0;
            }
        }

        class AppleHandTracker final : public VkmHandTrackerBase
        {
        public:
            ~AppleHandTracker() override { stop(); }

            bool start(std::string* outError) override;
            void stop() override;
            void submitFrame(const VkmVideoFrame& frame) override;
            bool tryAcquirePose(VkmHandPose* outPose) override;
            const char* getName() const override { return "Vision hand pose"; }

        private:
            void detect();
            void publishPose(const VkmHandPose& pose);

            VNDetectHumanHandPoseRequest* _request = nil;
            dispatch_queue_t _detectQueue = nullptr;

            // Owned by the detection while _detecting is set, so it needs no lock of its own.
            std::vector<uint8_t> _detectPixels;
            uint32_t _detectWidth = 0;
            uint32_t _detectHeight = 0;
            OSType _detectPixelFormat = 0;

            std::mutex _poseMutex;
            VkmHandPose _latestPose;
            uint64_t _poseSequence = 0;
            uint64_t _consumedSequence = 0;

            std::atomic<bool> _detecting{ false };
        };

        bool AppleHandTracker::start(std::string* outError)
        {
            _request = [[VNDetectHumanHandPoseRequest alloc] init];
            if (_request == nil)
            {
                *outError = "could not create a VNDetectHumanHandPoseRequest";
                return false;
            }
            _request.maximumHandCount = 1;

            _detectQueue = dispatch_queue_create("vkm.hand_tracker.detect", DISPATCH_QUEUE_SERIAL);
            return true;
        }

        void AppleHandTracker::stop()
        {
            // The queue is serial, so an empty block that has run means the detection ahead of it
            // has finished touching this object.
            if (_detectQueue != nullptr)
            {
                dispatch_sync(_detectQueue, ^{});
                dispatch_release(_detectQueue);
                _detectQueue = nullptr;
            }

            [_request release];
            _request = nil;
        }

        void AppleHandTracker::submitFrame(const VkmVideoFrame& frame)
        {
            if (_detectQueue == nullptr || frame._width == 0 || frame._height == 0)
            {
                return;
            }

            const OSType pixelFormat = coreVideoPixelFormat(frame._format);
            if (pixelFormat == 0)
            {
                VKM_DEBUG_ERROR("VkmHandTracker was handed a frame in a channel order Vision cannot read");
                return;
            }

            if (_detecting.exchange(true))
            {
                return; // a detection is already in flight; this frame is dropped
            }

            // Copied because the caller may reuse its frame the moment this returns, while the
            // detection below runs for another several milliseconds.
            _detectPixels = frame._pixels;
            _detectWidth = frame._width;
            _detectHeight = frame._height;
            _detectPixelFormat = pixelFormat;

            dispatch_async(_detectQueue, ^{
                @autoreleasepool
                {
                    detect();
                }
                _detecting.store(false);
            });
        }

        void AppleHandTracker::detect()
        {
            CVPixelBufferRef pixelBuffer = nullptr;
            // Wraps the bytes in place rather than copying them again; the buffer is released
            // before this returns, so the vector outliving it is all the lifetime it needs.
            const CVReturn created = CVPixelBufferCreateWithBytes(
                kCFAllocatorDefault, _detectWidth, _detectHeight, _detectPixelFormat,
                _detectPixels.data(), static_cast<size_t>(_detectWidth) * 4,
                nullptr, nullptr, nullptr, &pixelBuffer);
            if (created != kCVReturnSuccess || pixelBuffer == nullptr)
            {
                VKM_DEBUG_ERROR("CVPixelBufferCreateWithBytes failed for a hand tracking frame");
                return;
            }

            VNImageRequestHandler* handler =
                [[VNImageRequestHandler alloc] initWithCVPixelBuffer:pixelBuffer
                                                          orientation:kCGImagePropertyOrientationUp
                                                              options:@{}];

            NSError* requestError = nil;
            const BOOL performed = [handler performRequests:@[ _request ] error:&requestError];
            [handler release];
            CVPixelBufferRelease(pixelBuffer);

            if (!performed)
            {
                VKM_DEBUG_ERROR(requestError != nil ? requestError.localizedDescription.UTF8String
                                                    : "VNDetectHumanHandPoseRequest failed");
                return;
            }

            NSArray<VNHumanHandPoseObservation*>* observations = _request.results;
            if (observations.count == 0)
            {
                publishPose(VkmHandPose{});
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
                publishPose(VkmHandPose{});
                return;
            }

            NSArray<VNHumanHandPoseObservationJointName>* names = handJointNames();

            VkmHandPose pose;
            for (uint32_t i = 0; i < kVkmHandJointCount; ++i)
            {
                VNRecognizedPoint* point = points[names[i]];
                if (point == nil)
                {
                    continue;
                }
                // Vision normalizes to the image with a bottom-left origin; VkmHandPose is
                // top-left, so y inverts and x passes through. The pose stays in image space --
                // a consumer displaying the image mirrored is the one that mirrors the pose.
                pose._joints[i] = glm::vec2(static_cast<float>(point.x), 1.0f - static_cast<float>(point.y));
                pose._confidence[i] = point.confidence;
            }

            // Only the joints that locate the hand have to be trustworthy: one hidden knuckle in
            // the middle of the hand is not a reason to drop the whole pose.
            bool usable = true;
            for (const VkmHandJoint joint : kVkmHandFingertips)
            {
                usable = usable && pose._confidence[static_cast<size_t>(joint)] >= kJointConfidenceFloor;
            }
            for (const VkmHandJoint joint : kVkmHandPalmJoints)
            {
                usable = usable && pose._confidence[static_cast<size_t>(joint)] >= kJointConfidenceFloor;
            }
            pose._valid = usable;

            publishPose(pose);
        }

        void AppleHandTracker::publishPose(const VkmHandPose& pose)
        {
            std::lock_guard<std::mutex> lock(_poseMutex);
            _latestPose = pose;
            ++_poseSequence;
        }

        bool AppleHandTracker::tryAcquirePose(VkmHandPose* outPose)
        {
            std::lock_guard<std::mutex> lock(_poseMutex);
            if (_poseSequence == _consumedSequence)
            {
                return false;
            }
            _consumedSequence = _poseSequence;
            *outPose = _latestPose;
            return true;
        }
    } // namespace

    VkmHandTrackerBase* vkmCreateHandTracker()
    {
        return new AppleHandTracker();
    }
} // namespace vkm
