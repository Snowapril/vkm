// Copyright (c) 2025 Snowapril

#include <vkm/renderer/camera.h>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>

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
} // namespace vkm
