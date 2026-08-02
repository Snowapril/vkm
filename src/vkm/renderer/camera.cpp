// Copyright (c) 2025 Snowapril

#include <vkm/renderer/camera.h>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/geometric.hpp>

#include <algorithm>
#include <cmath>

namespace vkm
{
    void VkmCamera::lookAt(const glm::vec3& eye, const glm::vec3& target, const glm::vec3& up)
    {
        _eye = eye;
        _target = target;
        _up = up;
    }

    void VkmCamera::setPerspective(float fovYRadians, float nearZ, float farZ)
    {
        _fovYRadians = fovYRadians;
        _nearZ = nearZ;
        _farZ = farZ;
    }

    void VkmCamera::setViewportSize(uint32_t width, uint32_t height)
    {
        _viewportWidth = width;
        _viewportHeight = height;
    }

    glm::mat4 VkmCamera::getView() const
    {
        return glm::lookAtRH(_eye, _target, _up);
    }

    glm::mat4 VkmCamera::getProjection() const
    {
        const float aspect = (_viewportHeight > 0)
            ? static_cast<float>(_viewportWidth) / static_cast<float>(_viewportHeight)
            : 1.0f;
        // *_ZO: the engine's clip space is +Y-up with a [0,1] depth range on every backend
        // (the Vulkan backend's -fvk-invert-y handles its inverted NDC).
        return glm::perspectiveRH_ZO(_fovYRadians, aspect, _nearZ, _farZ);
    }

    glm::mat4 VkmCamera::getViewProjection() const
    {
        return getProjection() * getView();
    }

    glm::mat4 VkmCamera::getInverseViewProjection() const
    {
        return glm::inverse(getViewProjection());
    }

    void VkmCamera::fillFrameConstants(VkmFrameConstants& outConstants) const
    {
        const glm::mat4 view = getView();
        const glm::mat4 projection = getProjection();
        const glm::mat4 viewProjection = projection * view;

        outConstants._view = view;
        outConstants._projection = projection;
        outConstants._viewProjection = viewProjection;
        outConstants._inverseViewProjection = glm::inverse(viewProjection);
        outConstants._cameraPositionWorld = glm::vec4(_eye, 1.0f);

        // Zero until setViewportSize() has been called; the reciprocal is left at zero rather
        // than made infinite, so a shader dividing by it sees an obviously wrong 0 instead of a
        // NaN that propagates silently.
        const float width = static_cast<float>(_viewportWidth);
        const float height = static_cast<float>(_viewportHeight);
        outConstants._viewportSize = glm::vec4(width, height,
                                               width > 0.0f ? 1.0f / width : 0.0f,
                                               height > 0.0f ? 1.0f / height : 0.0f);

        // _prevViewProjection and _frameIndex are the engine's to fill: a camera holds no
        // frame-to-frame state (see VkmEngine::render).
    }

    VkmOrbitCameraController::VkmOrbitCameraController(VkmCamera* camera)
        : _camera(camera)
    {
        applyToCamera();
    }

    VkmOrbitCameraController::~VkmOrbitCameraController()
    {
        unregister();
    }

    void VkmOrbitCameraController::registerTo(VkmInputHandler& inputHandler)
    {
        if (_inputHandler != nullptr)
        {
            return;
        }
        _inputHandler = &inputHandler;
        _listenerHandle = inputHandler.addListener([this](const VkmInputEvent& event) {
            onInputEvent(event);
        });
    }

    void VkmOrbitCameraController::unregister()
    {
        if (_inputHandler == nullptr)
        {
            return;
        }
        _inputHandler->removeListener(_listenerHandle);
        _inputHandler = nullptr;
    }

    void VkmOrbitCameraController::frame(const glm::vec3& center, float radius)
    {
        _target = center;
        _distance = std::max(radius * 2.5f, 0.001f);
        _minDistance = _distance * 0.05f;
        _maxDistance = _distance * 20.0f;
        applyToCamera();
    }

    void VkmOrbitCameraController::onInputEvent(const VkmInputEvent& event)
    {
        switch (event._type)
        {
            case VkmInputEventType::MouseButton:
            {
                if (event._button != VkmMouseButton::Left)
                {
                    return;
                }
                _dragging = (event._action == VkmKeyAction::Press);
                // The cursor may have moved while the events were going elsewhere (ImGui
                // captures the mouse over its windows), so the tracked position can be stale.
                // Dropping it here costs the drag's first sub-pixel delta and avoids a jump.
                _hasLastCursor = false;
                return;
            }
            case VkmInputEventType::CursorMove:
            {
                const bool rotate = _dragging && _hasLastCursor;
                const double deltaX = event._x - _lastCursorX;
                const double deltaY = event._y - _lastCursorY;
                _lastCursorX = event._x;
                _lastCursorY = event._y;
                _hasLastCursor = true;
                if (!rotate)
                {
                    return;
                }
                _yaw += static_cast<float>(deltaX) * _rotateSensitivity;
                _pitch += static_cast<float>(deltaY) * _rotateSensitivity;
                // Just short of the poles, where the up vector would degenerate.
                _pitch = std::clamp(_pitch, -1.5f, 1.5f);
                break;
            }
            case VkmInputEventType::Scroll:
            {
                if (event._y == 0.0)
                {
                    return;
                }
                _distance = std::clamp(_distance * std::pow(_zoomFactor, static_cast<float>(event._y)),
                                       _minDistance, _maxDistance);
                break;
            }
            case VkmInputEventType::WindowFocus:
            {
                // The button release lands on whichever window took focus, so end the drag here
                // or the next cursor move in this window would keep orbiting.
                if (!event._focused)
                {
                    _dragging = false;
                    _hasLastCursor = false;
                }
                return;
            }
            case VkmInputEventType::WindowResize:
            {
                // Dragging the window's left or top edge moves its origin, so the cursor's
                // window-relative position jumps without the pointer having moved. Same fix as
                // the stale-position cases above: drop the baseline so that jump is never
                // applied as a rotation. The aspect ratio needs no handling here -- the engine
                // republishes the viewport size from the swapchain every frame.
                _hasLastCursor = false;
                return;
            }
            case VkmInputEventType::Key:
                return;
        }

        applyToCamera();
    }

    void VkmOrbitCameraController::applyToCamera()
    {
        const glm::vec3 eye = _target + glm::vec3{
            _distance * std::cos(_pitch) * std::sin(_yaw),
            _distance * std::sin(_pitch),
            _distance * std::cos(_pitch) * std::cos(_yaw),
        };
        _camera->lookAt(eye, _target, glm::vec3(0.0f, 1.0f, 0.0f));
        // Near and far slide with the dolly so the depth range stays useful at every zoom.
        _camera->setPerspective(_camera->getFovYRadians(), _distance * 0.01f, _distance * 10.0f);
    }

    namespace
    {
        constexpr glm::vec3 kWorldUp{ 0.0f, 1.0f, 0.0f };
        // Just short of the poles, where the up vector would degenerate. Same limit the orbit
        // controller uses.
        constexpr float kMaxPitch = 1.5f;
    }

    VkmFlyCameraController::VkmFlyCameraController(VkmCamera* camera)
        : _camera(camera)
    {
        applyToCamera();
    }

    VkmFlyCameraController::~VkmFlyCameraController()
    {
        unregister();
    }

    void VkmFlyCameraController::registerTo(VkmInputHandler& inputHandler)
    {
        if (_inputHandler != nullptr)
        {
            return;
        }
        _inputHandler = &inputHandler;
        _listenerHandle = inputHandler.addListener([this](const VkmInputEvent& event) {
            onInputEvent(event);
        });
    }

    void VkmFlyCameraController::unregister()
    {
        if (_inputHandler == nullptr)
        {
            return;
        }
        _inputHandler->removeListener(_listenerHandle);
        _inputHandler = nullptr;
    }

    void VkmFlyCameraController::setPosition(const glm::vec3& position)
    {
        _position = position;
        applyToCamera();
    }

    void VkmFlyCameraController::syncFromCamera()
    {
        _position = _camera->getPosition();

        const glm::vec3 toTarget = _camera->getTarget() - _position;
        const float horizontal = std::sqrt(toTarget.x * toTarget.x + toTarget.z * toTarget.z);
        // A camera looking straight up or down carries no yaw information, so keep the current
        // one rather than letting atan2(0, 0) reset it to zero.
        if (horizontal > 1e-6f)
        {
            _yaw = std::atan2(toTarget.x, toTarget.z);
        }
        _pitch = std::clamp(std::atan2(toTarget.y, horizontal), -kMaxPitch, kMaxPitch);
        applyToCamera();
    }

    void VkmFlyCameraController::tick(double deltaTime)
    {
        if (_inputHandler == nullptr || deltaTime <= 0.0)
        {
            return;
        }

        const glm::vec3 forward = getForward();
        const glm::vec3 right = glm::normalize(glm::cross(forward, kWorldUp));

        glm::vec3 direction{ 0.0f };
        if (_inputHandler->isKeyDown(VkmKeyCode::W)) direction += forward;
        if (_inputHandler->isKeyDown(VkmKeyCode::S)) direction -= forward;
        if (_inputHandler->isKeyDown(VkmKeyCode::D)) direction += right;
        if (_inputHandler->isKeyDown(VkmKeyCode::A)) direction -= right;
        // World up rather than the camera's own, so rising stays vertical while pitched.
        if (_inputHandler->isKeyDown(VkmKeyCode::E)) direction += kWorldUp;
        if (_inputHandler->isKeyDown(VkmKeyCode::Q)) direction -= kWorldUp;

        if (glm::dot(direction, direction) <= 0.0f)
        {
            return;
        }

        // Normalized so diagonals are not faster than the axes.
        const bool boosting = _inputHandler->isKeyDown(VkmKeyCode::LeftShift) ||
                              _inputHandler->isKeyDown(VkmKeyCode::RightShift);
        const float speed = _moveSpeed * (boosting ? _boostMultiplier : 1.0f);
        _position += glm::normalize(direction) * speed * static_cast<float>(deltaTime);
        applyToCamera();
    }

    void VkmFlyCameraController::onInputEvent(const VkmInputEvent& event)
    {
        switch (event._type)
        {
            case VkmInputEventType::MouseButton:
            {
                if (event._button != VkmMouseButton::Left)
                {
                    return;
                }
                _dragging = (event._action == VkmKeyAction::Press);
                // Same reasoning as the orbit controller: the tracked position can be stale
                // because events went elsewhere while ImGui owned the mouse.
                _hasLastCursor = false;
                return;
            }
            case VkmInputEventType::CursorMove:
            {
                const bool look = _dragging && _hasLastCursor;
                const double deltaX = event._x - _lastCursorX;
                const double deltaY = event._y - _lastCursorY;
                _lastCursorX = event._x;
                _lastCursorY = event._y;
                _hasLastCursor = true;
                if (!look)
                {
                    return;
                }
                _yaw += static_cast<float>(deltaX) * _lookSensitivity;
                _pitch = std::clamp(_pitch + static_cast<float>(deltaY) * _lookSensitivity,
                                    -kMaxPitch, kMaxPitch);
                break;
            }
            case VkmInputEventType::WindowFocus:
            {
                // The button release lands on whichever window took focus, so end the drag here
                // or the next cursor move in this window would keep turning the view.
                if (!event._focused)
                {
                    _dragging = false;
                    _hasLastCursor = false;
                }
                return;
            }
            case VkmInputEventType::WindowResize:
            {
                // Same as the orbit controller: a window origin that moved under the cursor
                // makes the tracked position stale, and applying that as a look delta would
                // snap the view.
                _hasLastCursor = false;
                return;
            }
            case VkmInputEventType::Key:
            case VkmInputEventType::Scroll:
                // Movement keys are polled in tick(); there is nothing to do per event.
                return;
        }

        applyToCamera();
    }

    glm::vec3 VkmFlyCameraController::getForward() const
    {
        return glm::vec3{
            std::cos(_pitch) * std::sin(_yaw),
            std::sin(_pitch),
            std::cos(_pitch) * std::cos(_yaw),
        };
    }

    void VkmFlyCameraController::applyToCamera()
    {
        _camera->lookAt(_position, _position + getForward(), kWorldUp);
    }
} // namespace vkm
