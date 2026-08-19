// Copyright (c) 2025 Snowapril

#pragma once

#include <glm/vec2.hpp>

#include <cstdint>

namespace vkm
{
    /*
    * @brief The 21 hand joints a pose carries: the wrist, then thumb through little finger,
    * each finger listed base to tip.
    * @details The order and the count match Apple Vision's VNHumanHandPoseObservation joint
    * names, which is what the platform HandInputSource fills them from. Cast to size_t to
    * index HandPose's arrays.
    */
    enum class HandJoint : uint32_t
    {
        Wrist = 0,

        ThumbCmc, ThumbMp, ThumbIp, ThumbTip,
        IndexMcp, IndexPip, IndexDip, IndexTip,
        MiddleMcp, MiddlePip, MiddleDip, MiddleTip,
        RingMcp, RingPip, RingDip, RingTip,
        LittleMcp, LittlePip, LittleDip, LittleTip,

        Count
    };

    inline constexpr uint32_t kHandJointCount = static_cast<uint32_t>(HandJoint::Count);

    // The five fingertips, thumb to little. These become the moving collision proxies.
    inline constexpr HandJoint kHandFingertips[5] = {
        HandJoint::ThumbTip, HandJoint::IndexTip, HandJoint::MiddleTip,
        HandJoint::RingTip, HandJoint::LittleTip,
    };

    // Joints averaged into the palm proxy: the wrist plus the four finger knuckles.
    inline constexpr HandJoint kHandPalmJoints[5] = {
        HandJoint::Wrist, HandJoint::IndexMcp, HandJoint::MiddleMcp,
        HandJoint::RingMcp, HandJoint::LittleMcp,
    };

    /*
    * @brief One detected hand, in the same space the camera image is displayed in.
    * @details Joint positions are normalized to [0, 1] on both axes with a top-left origin --
    * the convention VkmInputHandler uses for the cursor, not Vision's bottom-left one. They are
    * also already mirrored to match the mirrored background, so no consumer re-flips them: the
    * whole conversion lives in whichever HandInputSource produced the pose. A pose whose joints
    * did not all clear the source's confidence floor leaves _valid false rather than reporting
    * positions nothing measured.
    */
    struct HandPose
    {
        glm::vec2 _joints[kHandJointCount]{};
        float _confidence[kHandJointCount]{};
        bool _valid = false;
    };
} // namespace vkm
