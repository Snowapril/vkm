// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/platform/common/input_handler.h>
#include <vkm/renderer/backend/common/frame_constants.h>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

#include <cstdint>

namespace vkm
{
    /*
    * @brief View and projection state of one camera.
    *
    * Holds no input or scene knowledge: something else decides where the camera is (see
    * VkmOrbitCameraController) and what it looks at. The projection follows the engine's
    * clip-space convention -- +Y up with a [0,1] depth range on every backend, which is why
    * the *_ZO glm variants are used (the Vulkan backend's -fvk-invert-y handles its inverted
    * NDC; see renderer/backend/common/AGENTS.md).
    */
    class VkmCamera
    {
    public:
        VkmCamera() = default;

        /*
        * @brief Places the camera. `up` must not be parallel to (target - eye).
        */
        void lookAt(const glm::vec3& eye, const glm::vec3& target, const glm::vec3& up = glm::vec3(0.0f, 1.0f, 0.0f));

        void setPerspective(float fovYRadians, float nearZ, float farZ);

        /*
        * @brief Drives the projection's aspect ratio. A zero height leaves the aspect at 1.
        */
        void setViewportSize(uint32_t width, uint32_t height);

        glm::mat4 getView() const;
        glm::mat4 getProjection() const;
        glm::mat4 getViewProjection() const;
        glm::mat4 getInverseViewProjection() const;

        /*
        * @brief Fills the set-1 payload the engine publishes each frame. The one place that
        * knows how a camera maps onto VkmFrameConstants.
        */
        void fillFrameConstants(VkmFrameConstants& outConstants) const;

        inline const glm::vec3& getPosition() const { return _eye; }
        inline const glm::vec3& getTarget() const { return _target; }
        inline const glm::vec3& getUp() const { return _up; }
        inline float getFovYRadians() const { return _fovYRadians; }
        inline float getNearZ() const { return _nearZ; }
        inline float getFarZ() const { return _farZ; }

    private:
        glm::vec3 _eye{ 0.0f, 0.0f, 1.0f };
        glm::vec3 _target{ 0.0f, 0.0f, 0.0f };
        glm::vec3 _up{ 0.0f, 1.0f, 0.0f };

        float _fovYRadians = 0.8726646f; // 50 degrees
        float _nearZ = 0.1f;
        float _farZ = 100.0f;

        uint32_t _viewportWidth = 0;
        uint32_t _viewportHeight = 0;
    };

    /*
    * @brief Turntable controller for a VkmCamera: left-drag orbits, scroll dollies in and out.
    *
    * Registers a listener on VkmInputHandler rather than polling it, so a sample only has to
    * hand it the engine's input handler once. Listeners are dispatched from
    * VkmInputHandler::beginFrame(), which VkmEngine::loopInner() runs before update() and
    * render(), so the camera is already current by the time app code reads it -- there is no
    * per-frame tick to call. ImGui capture needs no handling here either: the platform layer
    * already drops events while ImGui owns the mouse.
    *
    * The camera pointer is borrowed and must outlive the controller.
    */
    class VkmOrbitCameraController
    {
    public:
        explicit VkmOrbitCameraController(VkmCamera* camera);
        ~VkmOrbitCameraController();

        VkmOrbitCameraController(const VkmOrbitCameraController&) = delete;
        VkmOrbitCameraController& operator=(const VkmOrbitCameraController&) = delete;

        /*
        * @brief Subscribes to `inputHandler`. Registering twice is a no-op; the handler must
        * outlive this controller or unregister() must be called first.
        */
        void registerTo(VkmInputHandler& inputHandler);
        void unregister();

        /*
        * @brief Frames a bounding sphere: centers the orbit on it and backs off far enough to
        * see all of it, rescaling the zoom limits to match.
        */
        void frame(const glm::vec3& center, float radius);

        inline void setRotateSensitivity(float radiansPerPixel) { _rotateSensitivity = radiansPerPixel; }
        inline void setZoomFactorPerScrollUnit(float factor) { _zoomFactor = factor; }

        inline float getDistance() const { return _distance; }
        inline float getYaw() const { return _yaw; }
        inline float getPitch() const { return _pitch; }

    private:
        void onInputEvent(const VkmInputEvent& event);
        void applyToCamera();

    private:
        VkmCamera* _camera;

        VkmInputHandler* _inputHandler = nullptr;
        uint32_t _listenerHandle = 0;

        glm::vec3 _target{ 0.0f };
        float _distance = 3.0f;
        float _minDistance = 0.1f;
        float _maxDistance = 100.0f;
        float _yaw = 0.6f;
        float _pitch = 0.3f;

        float _rotateSensitivity = 0.005f;
        float _zoomFactor = 0.9f;

        bool _dragging = false;
        bool _hasLastCursor = false;
        double _lastCursorX = 0.0;
        double _lastCursorY = 0.0;
    };

    /*
    * @brief Free-fly controller for a VkmCamera: WASD moves, left-drag looks around.
    *
    * W/S move along the view direction, A/D strafe, Q/E rise and fall along world up, and
    * holding LeftShift multiplies the speed. Unlike VkmOrbitCameraController this one cannot be
    * purely event-driven -- "held" is a duration, so tick(deltaTime) must be called once per
    * frame (from AppDelegate::update, which already receives the frame's delta). Held keys are
    * polled from the handler rather than integrated from key events, because auto-repeat rate is
    * an OS setting and would otherwise decide how fast the camera flies.
    *
    * Leaves the camera's projection alone: a fly camera has no orbit distance to scale near and
    * far by, so whatever the caller set stays.
    *
    * The camera pointer is borrowed and must outlive the controller.
    */
    class VkmFlyCameraController
    {
    public:
        explicit VkmFlyCameraController(VkmCamera* camera);
        ~VkmFlyCameraController();

        VkmFlyCameraController(const VkmFlyCameraController&) = delete;
        VkmFlyCameraController& operator=(const VkmFlyCameraController&) = delete;

        /*
        * @brief Subscribes to `inputHandler`. Registering twice is a no-op; the handler must
        * outlive this controller or unregister() must be called first.
        */
        void registerTo(VkmInputHandler& inputHandler);
        void unregister();

        /*
        * @brief Applies one frame of held movement keys. A no-op while not registered, since
        * held state is read from the handler.
        */
        void tick(double deltaTime);

        /*
        * @brief Adopts the camera's current eye and view direction, so handing control over
        * from another controller does not snap the view.
        */
        void syncFromCamera();

        void setPosition(const glm::vec3& position);

        inline void setMoveSpeed(float unitsPerSecond) { _moveSpeed = unitsPerSecond; }
        inline void setBoostMultiplier(float multiplier) { _boostMultiplier = multiplier; }
        inline void setLookSensitivity(float radiansPerPixel) { _lookSensitivity = radiansPerPixel; }

        inline const glm::vec3& getPosition() const { return _position; }
        inline float getYaw() const { return _yaw; }
        inline float getPitch() const { return _pitch; }
        inline float getMoveSpeed() const { return _moveSpeed; }

    private:
        void onInputEvent(const VkmInputEvent& event);
        void applyToCamera();
        glm::vec3 getForward() const;

    private:
        VkmCamera* _camera;

        VkmInputHandler* _inputHandler = nullptr;
        uint32_t _listenerHandle = 0;

        glm::vec3 _position{ 0.0f, 0.0f, 3.0f };
        // Same convention as VkmOrbitCameraController: yaw around world +Y, pitch above the
        // horizon, both feeding the identical forward-vector formula.
        float _yaw = 0.0f;
        float _pitch = 0.0f;

        float _moveSpeed = 3.0f;
        float _boostMultiplier = 4.0f;
        float _lookSensitivity = 0.005f;

        bool _dragging = false;
        bool _hasLastCursor = false;
        double _lastCursorX = 0.0;
        double _lastCursorY = 0.0;
    };
} // namespace vkm
