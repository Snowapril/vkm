// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/platform/common/hand_tracker.h>

#include <cstdint>

namespace vkm
{
    class VkmInputHandler;

    /*
    * @brief A hand tracker that follows the mouse instead of a camera.
    * @details The stand-in for every configuration `vkmCreateHandTracker()` cannot serve: a
    * platform with no hand tracking model, a machine whose camera was refused, or a run that asked
    * for it explicitly. A fixed hand skeleton is pinned to the cursor and its fingers curl into
    * the palm while the left button is held, which exercises everything downstream of a pose.
    * Submitted frames are ignored -- it has no use for an image.
    */
    class CursorHandTracker final : public VkmHandTrackerBase
    {
    public:
        /*
        * @param inputHandler Polled for cursor position and button state; must outlive this.
        */
        explicit CursorHandTracker(const VkmInputHandler* inputHandler);

        bool start(std::string* outError) override;
        void stop() override;
        void submitFrame(const VkmVideoFrame& frame) override;
        bool tryAcquirePose(VkmHandPose* outPose) override;
        const char* getName() const override { return "Cursor (no hand tracking)"; }

        /*
        * @brief Tells the tracker how large the window is, so it can normalize the cursor.
        * @details A camera tracker normalizes against its own image and needs no equivalent; this
        * one has no image, so the window is the only frame of reference it has.
        * @param width Framebuffer width in pixels.
        * @param height Framebuffer height in pixels.
        */
        void setViewportSize(uint32_t width, uint32_t height);

    private:
        const VkmInputHandler* _inputHandler = nullptr;
        uint32_t _viewportWidth = 0;
        uint32_t _viewportHeight = 0;
    };
} // namespace vkm
