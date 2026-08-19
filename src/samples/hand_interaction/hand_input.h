// Copyright (c) 2025 Snowapril

#pragma once

#include "hand_pose.h"

#include <cstdint>
#include <string>
#include <vector>

namespace vkm
{
    class VkmInputHandler;

    /*
    * @brief One captured camera frame, ready to hand to VkmDriverBase::uploadToTexture().
    * @details Pixels are tightly packed 8-bit BGRA with a top-left origin -- the order
    * AVFoundation delivers and the order the fragment shader swizzles back, since there is no
    * BGRA VkmFormat to upload into. _sequence increments once per delivered frame and is what
    * tells a consumer whether it has already seen this one.
    */
    struct CameraFrame
    {
        std::vector<uint8_t> _pixels;
        uint32_t _width = 0;
        uint32_t _height = 0;
        uint64_t _sequence = 0;
    };

    /*
    * @brief A source of camera frames and hand poses.
    * @details One interface covers both because the two are produced together: the platform
    * implementation feeds its detector from its own capture callback, so nothing outside has to
    * route frames between them. Both acquire calls are non-blocking polls meant to be made once
    * per frame from the engine loop thread, while production happens on the platform's own
    * queues -- the same producer/consumer split VkmInputHandler uses.
    */
    class HandInputSource
    {
    public:
        virtual ~HandInputSource() = default;

        /*
        * @brief Begins producing. Nothing is delivered until this returns true.
        * @param outError Receives the reason on failure.
        * @return False when the source cannot run: no device, no permission, no detector.
        */
        virtual bool start(std::string* outError) = 0;

        // Stops production and releases the platform objects. Safe to call without start().
        virtual void stop() = 0;

        /*
        * @brief Takes the newest frame, if one arrived since the last call.
        * @param outFrame Receives the pixels; untouched when this returns false.
        * @return False when no new frame is available, or when the source produces no video at
        * all (the cursor stand-in never does).
        */
        virtual bool tryAcquireFrame(CameraFrame* outFrame) = 0;

        /*
        * @brief Takes the newest completed detection. Never waits on the detector.
        * @param outPose Receives the pose; untouched when this returns false.
        * @return False when no detection has completed since the last call.
        */
        virtual bool tryAcquirePose(HandPose* outPose) = 0;

        /*
        * @brief Tells the source how large the window currently is.
        * @details Only a source that has no image of its own needs this: the cursor stand-in
        * normalizes framebuffer-pixel cursor positions with it. A camera source normalizes
        * against its own capture extent instead, so the default ignores it.
        * @param width Framebuffer width in pixels.
        * @param height Framebuffer height in pixels.
        */
        virtual void setViewportSize(uint32_t width, uint32_t height) { (void)width; (void)height; }

        // Short name for the UI, so it is obvious which source is live.
        virtual const char* getName() const = 0;
    };

    /*
    * @brief Creates the platform's real camera-and-hand-tracking source.
    * @return The source, or null on a platform that has no implementation. Ownership passes to
    * the caller. A non-null result still has to start() successfully.
    */
    HandInputSource* createPlatformHandInput();

    /*
    * @brief Creates the cursor-driven stand-in: a hand shape that follows the mouse, its fingers
    * closing while the left button is held.
    * @details Always available, and always the fallback when the platform source is missing or
    * refuses to start, so the sample stays runnable and testable on every platform. Produces no
    * video frames, and relies on setViewportSize() to normalize the cursor.
    * @param inputHandler Polled for cursor position and button state; must outlive the source.
    */
    HandInputSource* createMouseHandInput(const VkmInputHandler* inputHandler);
} // namespace vkm
