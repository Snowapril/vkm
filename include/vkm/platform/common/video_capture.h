// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/renderer/backend/common/renderer_common.h>

#include <cstdint>
#include <string>
#include <vector>

namespace vkm
{
    /*
    * @brief One captured video frame, ready to hand to VkmDriverBase::uploadToTexture().
    * @details Pixels are tightly packed 8-bit RGBA or BGRA with a top-left origin. Which of the
    * two is a property of the platform rather than a choice -- AVFoundation delivers BGRA and a
    * browser canvas delivers RGBA -- so the channel order travels with the frame in _format and
    * the consumer creates a texture of that format. Converting on the CPU instead would cost a
    * pass over a megapixel every frame for nothing, since every backend can sample both.
    * _sequence increments once per delivered frame and is what tells a consumer whether it has
    * already seen this one.
    */
    struct VkmVideoFrame
    {
        std::vector<uint8_t> _pixels;
        uint32_t _width = 0;
        uint32_t _height = 0;
        // Always BGRA8_UNORM or R8G8B8A8_UNORM.
        VkmFormat _format = VkmFormat::R8G8B8A8_UNORM;
        uint64_t _sequence = 0;

        inline uint64_t getByteSize() const { return static_cast<uint64_t>(_width) * _height * 4; }
    };

    /*
    * @brief A camera delivering frames.
    * @details Frames are produced on whatever thread the platform's capture API uses and taken by
    * a non-blocking poll, meant to be made once per frame from the engine loop thread -- the same
    * producer/consumer split VkmInputHandler uses. Only the newest frame is kept: a consumer that
    * falls behind skips frames rather than working through a backlog of stale ones.
    */
    class VkmVideoCaptureBase
    {
    public:
        virtual ~VkmVideoCaptureBase() = default;

        /*
        * @brief Opens a camera and begins capturing. Nothing is delivered until this returns true.
        * @details May return true before the first frame is available: a camera can take seconds
        * to power up, and permission may still be pending, so tryAcquireFrame() simply reports
        * nothing until then.
        * @param outError Receives the reason on failure.
        * @return False when no camera can be opened, or when access to one was refused.
        */
        virtual bool start(std::string* outError) = 0;

        // Stops capturing and releases the platform objects. Safe to call without start().
        virtual void stop() = 0;

        /*
        * @brief Takes the newest frame, if one arrived since the last call.
        * @param outFrame Receives the pixels; untouched when this returns false.
        * @return False when no new frame is available.
        */
        virtual bool tryAcquireFrame(VkmVideoFrame* outFrame) = 0;

        // Short name for logs and UI.
        virtual const char* getName() const = 0;
    };

    /*
    * @brief Creates the platform's camera capture.
    * @details Implemented on Apple platforms (AVFoundation) and in the browser (getUserMedia).
    * @return The capture, or null on a platform with no implementation. Ownership passes to the
    * caller, which must still start() it.
    */
    VkmVideoCaptureBase* vkmCreateVideoCapture();
} // namespace vkm
