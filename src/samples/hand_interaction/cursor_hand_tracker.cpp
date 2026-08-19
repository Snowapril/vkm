// Copyright (c) 2025 Snowapril
//
// The stand-in hand. It exists so the sample is runnable, and the simulation verifiable, in every
// configuration with no camera hand tracking behind it -- which is every platform but Apple, and
// any Mac whose owner refused the camera.

#include "cursor_hand_tracker.h"

#include <vkm/platform/common/input_handler.h>

#include <glm/common.hpp>
#include <glm/vec2.hpp>

namespace vkm
{
    namespace
    {
        /*
        * @brief One finger of the stand-in skeleton, in a hand-local space: origin at the palm
        * centre, +y towards the fingertips, one unit spanning roughly a whole hand.
        */
        struct FingerLayout
        {
            glm::vec2 _base;
            glm::vec2 _direction;
            float _length;
        };

        // Thumb, index, middle, ring, little -- the order kVkmHandFingertips uses.
        constexpr FingerLayout kFingers[5] = {
            { { -0.40f, -0.10f }, { -0.62f, 0.78f }, 0.55f },
            { { -0.17f,  0.12f }, { -0.16f, 0.99f }, 0.72f },
            { {  0.01f,  0.15f }, {  0.00f, 1.00f }, 0.78f },
            { {  0.18f,  0.12f }, {  0.16f, 0.99f }, 0.72f },
            { {  0.32f,  0.03f }, {  0.34f, 0.94f }, 0.58f },
        };

        // How far along each finger its four joints sit: MCP, PIP, DIP, TIP.
        constexpr float kJointFractions[4] = { 0.0f, 0.42f, 0.72f, 1.0f };

        constexpr glm::vec2 kWristLocal{ 0.0f, -0.55f };

        // Hand width as a fraction of the window width.
        constexpr float kHandScale = 0.21f;

        // How far a closed finger is dragged towards the palm. The knuckle barely moves and the
        // tip travels most of the way, which is what turns a press into a grabbing motion.
        constexpr float kMaxCurl = 0.85f;
    } // namespace

    CursorHandTracker::CursorHandTracker(const VkmInputHandler* inputHandler)
        : _inputHandler(inputHandler)
    {
    }

    bool CursorHandTracker::start(std::string* outError)
    {
        if (_inputHandler == nullptr)
        {
            *outError = "the cursor hand tracker needs an input handler";
            return false;
        }
        return true;
    }

    void CursorHandTracker::stop() {}

    void CursorHandTracker::submitFrame(const VkmVideoFrame& frame)
    {
        (void)frame;
    }

    void CursorHandTracker::setViewportSize(uint32_t width, uint32_t height)
    {
        _viewportWidth = width;
        _viewportHeight = height;
    }

    bool CursorHandTracker::tryAcquirePose(VkmHandPose* outPose)
    {
        if (_inputHandler == nullptr || _viewportWidth == 0 || _viewportHeight == 0)
        {
            return false;
        }

        const float width = static_cast<float>(_viewportWidth);
        const float height = static_cast<float>(_viewportHeight);
        const glm::vec2 cursor(
            glm::clamp(static_cast<float>(_inputHandler->getCursorX()) / width, 0.0f, 1.0f),
            glm::clamp(static_cast<float>(_inputHandler->getCursorY()) / height, 0.0f, 1.0f));

        const float curl = _inputHandler->isMouseButtonDown(VkmMouseButton::Left) ? kMaxCurl : 0.0f;

        // The layout is authored in units of window width on both axes so it stays hand-shaped in
        // any window; the normalized coordinates VkmHandPose carries undo that on y only.
        const float aspect = width / height;
        auto place = [&](const glm::vec2& local) {
            return glm::vec2(cursor.x + local.x * kHandScale,
                             cursor.y - local.y * kHandScale * aspect);
        };

        VkmHandPose pose;
        pose._joints[static_cast<size_t>(VkmHandJoint::Wrist)] = place(kWristLocal);

        for (uint32_t finger = 0; finger < 5; ++finger)
        {
            const FingerLayout& layout = kFingers[finger];
            for (uint32_t joint = 0; joint < 4; ++joint)
            {
                const float fraction = kJointFractions[joint];
                const glm::vec2 extended = layout._base + layout._direction * (layout._length * fraction);
                // Curling drags each joint towards the palm centre in proportion to how far out
                // along the finger it sits.
                const glm::vec2 local = extended * (1.0f - curl * fraction);

                const size_t index = 1 + static_cast<size_t>(finger) * 4 + joint;
                pose._joints[index] = place(local);
            }
        }

        for (uint32_t i = 0; i < kVkmHandJointCount; ++i)
        {
            pose._confidence[i] = 1.0f;
        }
        pose._valid = true;

        *outPose = pose;
        return true;
    }
} // namespace vkm
