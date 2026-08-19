// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/platform/common/video_capture.h>

#include <glm/vec2.hpp>

#include <cstdint>
#include <string>

namespace vkm
{
    /*
    * @brief The 21 hand joints a pose carries: the wrist, then thumb through little finger, each
    * finger listed base to tip.
    * @details The order and the count match Apple Vision's VNHumanHandPoseObservation joint
    * names, and the same 21-joint skeleton is what every other hand tracking model in common use
    * reports. Cast to size_t to index VkmHandPose's arrays.
    */
    enum class VkmHandJoint : uint32_t
    {
        Wrist = 0,

        ThumbCmc, ThumbMp, ThumbIp, ThumbTip,
        IndexMcp, IndexPip, IndexDip, IndexTip,
        MiddleMcp, MiddlePip, MiddleDip, MiddleTip,
        RingMcp, RingPip, RingDip, RingTip,
        LittleMcp, LittlePip, LittleDip, LittleTip,

        Count
    };

    inline constexpr uint32_t kVkmHandJointCount = static_cast<uint32_t>(VkmHandJoint::Count);

    // The five fingertips, thumb to little.
    inline constexpr VkmHandJoint kVkmHandFingertips[5] = {
        VkmHandJoint::ThumbTip, VkmHandJoint::IndexTip, VkmHandJoint::MiddleTip,
        VkmHandJoint::RingTip, VkmHandJoint::LittleTip,
    };

    // The wrist plus the four finger knuckles, which together locate the palm.
    inline constexpr VkmHandJoint kVkmHandPalmJoints[5] = {
        VkmHandJoint::Wrist, VkmHandJoint::IndexMcp, VkmHandJoint::MiddleMcp,
        VkmHandJoint::RingMcp, VkmHandJoint::LittleMcp,
    };

    /*
    * @brief One detected hand.
    * @details Joint positions are normalized to [0, 1] on both axes with a top-left origin -- the
    * convention VkmInputHandler uses for the cursor, and the one a camera frame's pixels are laid
    * out in, so a pose indexes straight into the image it came from. A tracker whose underlying
    * API uses another origin converts on the way out, so no consumer ever has to. A pose whose
    * joints did not all clear the tracker's confidence floor leaves _valid false rather than
    * reporting positions nothing measured.
    *
    * Positions are two-dimensional: no tracker here reports depth, and inferring it from apparent
    * hand size is too noisy to drive anything that needs stability.
    */
    struct VkmHandPose
    {
        glm::vec2 _joints[kVkmHandJointCount]{};
        float _confidence[kVkmHandJointCount]{};
        bool _valid = false;
    };

    /*
    * @brief Turns camera frames into hand poses.
    * @details Detection runs off the caller's thread: submitFrame() hands a frame over and
    * returns immediately, and tryAcquirePose() takes whatever has completed since the last call.
    * Frames submitted while a detection is already running are dropped rather than queued --
    * queueing them would only build a backlog of poses that are stale by the time they emerge.
    * Both calls are meant to be made once per frame from the engine loop thread.
    */
    class VkmHandTrackerBase
    {
    public:
        virtual ~VkmHandTrackerBase() = default;

        /*
        * @brief Prepares the detector. Nothing is detected until this returns true.
        * @param outError Receives the reason on failure.
        */
        virtual bool start(std::string* outError) = 0;

        // Releases the detector and waits for any detection still running. Safe without start().
        virtual void stop() = 0;

        /*
        * @brief Offers a frame for detection; returns immediately.
        * @param frame Pixels to detect in. Nothing is retained: the tracker copies whatever it
        * needs before returning, so the caller may reuse the frame at once.
        */
        virtual void submitFrame(const VkmVideoFrame& frame) = 0;

        /*
        * @brief Takes the newest completed detection. Never waits on the detector.
        * @param outPose Receives the pose; untouched when this returns false.
        * @return False when no detection has completed since the last call.
        */
        virtual bool tryAcquirePose(VkmHandPose* outPose) = 0;

        // Short name for logs and UI.
        virtual const char* getName() const = 0;
    };

    /*
    * @brief Creates the platform's hand tracker.
    * @details Implemented on Apple platforms only (Vision's VNDetectHumanHandPoseRequest). No
    * other platform here ships a hand tracking model, and bundling one is a much larger decision
    * than this interface -- so elsewhere this returns null and the caller supplies its own input.
    * @return The tracker, or null on a platform with no implementation. Ownership passes to the
    * caller, which must still start() it.
    */
    VkmHandTrackerBase* vkmCreateHandTracker();
} // namespace vkm
