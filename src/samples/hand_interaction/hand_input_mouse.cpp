// Copyright (c) 2025 Snowapril
//
// The stand-in hand: a fixed skeleton pinned to the cursor, fingers curling into the palm while
// the left mouse button is held. It exists so the sample is runnable, and the simulation
// verifiable, on every platform and in every configuration -- including the ones with no camera
// implementation at all, and a Mac whose owner denied camera access.

#include "hand_input.h"

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

        // Thumb, index, middle, ring, little -- the order kHandFingertips uses.
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

        class MouseHandInput final : public HandInputSource
        {
        public:
            explicit MouseHandInput(const VkmInputHandler* inputHandler) : _inputHandler(inputHandler) {}

            bool start(std::string* outError) override
            {
                if (_inputHandler == nullptr)
                {
                    *outError = "the cursor hand source needs an input handler";
                    return false;
                }
                return true;
            }

            void stop() override {}

            // No camera behind this source, so the sample draws its own background instead.
            bool tryAcquireFrame(CameraFrame* outFrame) override
            {
                (void)outFrame;
                return false;
            }

            bool tryAcquirePose(HandPose* outPose) override;

            void setViewportSize(uint32_t width, uint32_t height) override
            {
                _viewportWidth = width;
                _viewportHeight = height;
            }

            const char* getName() const override { return "Cursor (no camera)"; }

        private:
            const VkmInputHandler* _inputHandler = nullptr;
            uint32_t _viewportWidth = 0;
            uint32_t _viewportHeight = 0;
        };

        bool MouseHandInput::tryAcquirePose(HandPose* outPose)
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

            // The layout is authored in sim space, where both axes are measured in window
            // widths, so it stays hand-shaped in any window. Getting back to the normalized
            // coordinates HandPose carries means undoing that on y only.
            const float simToNormalizedY = width / height;
            auto place = [&](const glm::vec2& local) {
                return glm::vec2(cursor.x + local.x * kHandScale,
                                 cursor.y - local.y * kHandScale * simToNormalizedY);
            };

            HandPose pose;
            pose._joints[static_cast<size_t>(HandJoint::Wrist)] = place(kWristLocal);

            for (uint32_t finger = 0; finger < 5; ++finger)
            {
                const FingerLayout& layout = kFingers[finger];
                for (uint32_t joint = 0; joint < 4; ++joint)
                {
                    const float fraction = kJointFractions[joint];
                    const glm::vec2 extended = layout._base + layout._direction * (layout._length * fraction);
                    // Curling drags each joint towards the palm centre in proportion to how far
                    // out along the finger it sits.
                    const glm::vec2 local = extended * (1.0f - curl * fraction);

                    const size_t index = 1 + static_cast<size_t>(finger) * 4 + joint;
                    pose._joints[index] = place(local);
                }
            }

            for (uint32_t i = 0; i < kHandJointCount; ++i)
            {
                pose._confidence[i] = 1.0f;
            }
            pose._valid = true;

            *outPose = pose;
            return true;
        }
    } // namespace

    HandInputSource* createMouseHandInput(const VkmInputHandler* inputHandler)
    {
        return new MouseHandInput(inputHandler);
    }
} // namespace vkm
