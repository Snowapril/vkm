#include <doctest/doctest.h>

#include <vkm/platform/common/input_handler.h>
#include <vkm/renderer/camera.h>

#include <glm/vec4.hpp>

#include <cmath>
#include <cstddef>

// VkmCamera and VkmOrbitCameraController are pure CPU state, and VkmInputHandler needs no
// window, so the whole camera pipeline -- events in, matrices out -- is testable headlessly.

namespace
{
    // Drives one frame of input: the handler only folds pushed events into its state (and
    // dispatches listeners) from beginFrame().
    void pumpFrame(vkm::VkmInputHandler& input)
    {
        input.beginFrame();
    }

    glm::vec3 projectPoint(const glm::mat4& matrix, const glm::vec3& point)
    {
        const glm::vec4 clip = matrix * glm::vec4(point, 1.0f);
        return glm::vec3(clip) / clip.w;
    }
}

TEST_CASE("VkmFrameConstants - layout matches the shader-side mirror") {
    // The C++ struct, vkm_frame_constants.hlsli and vkm-compiler's Metal binding pin are three
    // halves of one ABI, and nothing in the build fails if they drift -- a shader would simply
    // read the wrong bytes. The static_assert in frame_constants.h pins the total size; these pin
    // every member offset, which is what actually catches a field inserted in the middle.
    CHECK(offsetof(vkm::VkmFrameConstants, _view) == 0);
    CHECK(offsetof(vkm::VkmFrameConstants, _projection) == 64);
    CHECK(offsetof(vkm::VkmFrameConstants, _viewProjection) == 128);
    CHECK(offsetof(vkm::VkmFrameConstants, _inverseViewProjection) == 192);
    CHECK(offsetof(vkm::VkmFrameConstants, _prevViewProjection) == 256);
    CHECK(offsetof(vkm::VkmFrameConstants, _cameraPositionWorld) == 320);
    CHECK(offsetof(vkm::VkmFrameConstants, _viewportSize) == 336);
    CHECK(offsetof(vkm::VkmFrameConstants, _frameIndex) == 352);
    CHECK(offsetof(vkm::VkmFrameConstants, _prevCameraPositionWorld) == 368);
    CHECK(offsetof(vkm::VkmFrameConstants, _viewProjectionNoJitter) == 384);
    CHECK(offsetof(vkm::VkmFrameConstants, _jitter) == 448);
    CHECK(sizeof(vkm::VkmFrameConstants) == 464);

    // One region per frame slot, so the stride must stay a multiple of every backend's minimum
    // uniform-buffer offset alignment even as the struct grows.
    CHECK(vkm::kVkmFrameConstantStride % vkm::kVkmFrameConstantAlignment == 0);
    CHECK(vkm::kVkmFrameConstantStride >= sizeof(vkm::VkmFrameConstants));
}

TEST_CASE("VkmCamera - fillFrameConstants publishes the viewport size and its reciprocal") {
    vkm::VkmCamera camera;
    camera.setPerspective(1.0f, 0.1f, 100.0f);
    camera.setViewportSize(1920, 1080);

    vkm::VkmFrameConstants constants{};
    camera.fillFrameConstants(constants);

    CHECK(constants._viewportSize.x == doctest::Approx(1920.0f));
    CHECK(constants._viewportSize.y == doctest::Approx(1080.0f));
    CHECK(constants._viewportSize.z == doctest::Approx(1.0f / 1920.0f));
    CHECK(constants._viewportSize.w == doctest::Approx(1.0f / 1080.0f));
}

TEST_CASE("VkmCamera - a zero viewport reports a zero reciprocal rather than infinity") {
    // Before setViewportSize() is called. A shader dividing by zero here sees an obviously wrong
    // 0 instead of a NaN that propagates silently through a whole frame.
    vkm::VkmCamera camera;
    vkm::VkmFrameConstants constants{};
    camera.fillFrameConstants(constants);

    CHECK(constants._viewportSize.z == 0.0f);
    CHECK(constants._viewportSize.w == 0.0f);
}

TEST_CASE("Camera jitter shifts NDC by exactly the sub-pixel offset, and only the jittered pair")
{
    vkm::VkmCamera camera;
    camera.setViewportSize(128, 64);
    camera.lookAt(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f, 0.0f, 0.0f));
    camera.setPerspective(1.0f, 0.1f, 100.0f);

    // Zero jitter (the default): the jittered and jitter-free projections are identical.
    CHECK(camera.getProjection() == camera.getProjectionNoJitter());

    const glm::vec3 world(0.3f, -0.2f, 1.0f);
    const glm::vec3 baseNdc = projectPoint(camera.getViewProjection(), world);

    camera.setJitterPixels(glm::vec2(0.5f, 0.5f));
    const glm::vec3 jitteredNdc = projectPoint(camera.getViewProjection(), world);

    // A pixel is 2/extent wide in NDC; +x jitter moves samples right. Pixel space is +y down
    // while clip space is +y up, so +y jitter moves the NDC point down.
    CHECK(jitteredNdc.x - baseNdc.x == doctest::Approx(2.0f * 0.5f / 128.0f).epsilon(1e-4));
    CHECK(jitteredNdc.y - baseNdc.y == doctest::Approx(-2.0f * 0.5f / 64.0f).epsilon(1e-4));
    CHECK(jitteredNdc.z == doctest::Approx(baseNdc.z).epsilon(1e-5));

    // The jitter-free projection ignores the jitter entirely.
    const glm::vec3 noJitterNdc =
        projectPoint(camera.getProjectionNoJitter() * camera.getView(), world);
    CHECK(noJitterNdc.x == doctest::Approx(baseNdc.x).epsilon(1e-5));
    CHECK(noJitterNdc.y == doctest::Approx(baseNdc.y).epsilon(1e-5));

    // fillFrameConstants publishes the jittered set, the jitter-free matrix, and the jitter
    // itself (zw seeded equal to xy; the engine overwrites zw with last frame's value).
    vkm::VkmFrameConstants constants{};
    camera.fillFrameConstants(constants);
    CHECK(constants._viewProjection == camera.getViewProjection());
    CHECK(constants._viewProjectionNoJitter == camera.getProjectionNoJitter() * camera.getView());
    CHECK(constants._jitter.x == doctest::Approx(0.5f));
    CHECK(constants._jitter.y == doctest::Approx(0.5f));
    CHECK(constants._jitter.z == doctest::Approx(0.5f));
    CHECK(constants._jitter.w == doctest::Approx(0.5f));
}

TEST_CASE("Camera projects the target to the center of clip space")
{
    vkm::VkmCamera camera;
    camera.setViewportSize(128, 64);
    camera.lookAt(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f, 0.0f, 0.0f));
    camera.setPerspective(1.0f, 0.1f, 100.0f);

    const glm::vec3 ndc = projectPoint(camera.getViewProjection(), camera.getTarget());

    CHECK(std::abs(ndc.x) < 1e-5f);
    CHECK(std::abs(ndc.y) < 1e-5f);
    // The engine's clip space is +Y up with a [0,1] depth range, so a point between the near
    // and far planes lands strictly inside that range rather than around zero.
    CHECK(ndc.z > 0.0f);
    CHECK(ndc.z < 1.0f);
}

TEST_CASE("Camera clip space is +Y up and honors the viewport aspect ratio")
{
    vkm::VkmCamera camera;
    camera.lookAt(glm::vec3(0.0f, 0.0f, 5.0f), glm::vec3(0.0f, 0.0f, 0.0f));
    camera.setPerspective(1.0f, 0.1f, 100.0f);

    camera.setViewportSize(64, 64);
    const glm::vec3 squareNdc = projectPoint(camera.getViewProjection(), glm::vec3(1.0f, 1.0f, 0.0f));
    // A point above the target must land in the upper half of clip space.
    CHECK(squareNdc.y > 0.0f);
    CHECK(squareNdc.x == doctest::Approx(squareNdc.y));

    // Widening the viewport compresses X only; Y is what the vertical FOV pins.
    camera.setViewportSize(128, 64);
    const glm::vec3 wideNdc = projectPoint(camera.getViewProjection(), glm::vec3(1.0f, 1.0f, 0.0f));
    CHECK(wideNdc.y == doctest::Approx(squareNdc.y));
    CHECK(wideNdc.x < squareNdc.x);
}

TEST_CASE("Camera inverse view projection round-trips a projected point")
{
    vkm::VkmCamera camera;
    camera.setViewportSize(100, 50);
    camera.lookAt(glm::vec3(2.0f, 3.0f, 4.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    camera.setPerspective(0.9f, 0.05f, 50.0f);

    const glm::vec3 world(0.25f, 1.5f, -0.75f);
    const glm::vec3 ndc = projectPoint(camera.getViewProjection(), world);
    const glm::vec3 roundTripped = projectPoint(camera.getInverseViewProjection(), ndc);

    CHECK(roundTripped.x == doctest::Approx(world.x).epsilon(1e-4));
    CHECK(roundTripped.y == doctest::Approx(world.y).epsilon(1e-4));
    CHECK(roundTripped.z == doctest::Approx(world.z).epsilon(1e-4));
}

TEST_CASE("Orbit controller frames a bounding sphere and keeps the camera outside it")
{
    vkm::VkmCamera camera;
    vkm::VkmOrbitCameraController controller(&camera);

    const glm::vec3 center(1.0f, 2.0f, 3.0f);
    controller.frame(center, 4.0f);

    CHECK(camera.getTarget().x == doctest::Approx(center.x));
    CHECK(camera.getTarget().y == doctest::Approx(center.y));
    CHECK(camera.getTarget().z == doctest::Approx(center.z));
    CHECK(controller.getDistance() == doctest::Approx(10.0f)); // radius * 2.5
    CHECK(glm::length(camera.getPosition() - center) == doctest::Approx(controller.getDistance()));
    // Near/far slide with the dolly so the depth range stays useful at every zoom.
    CHECK(camera.getNearZ() < camera.getFarZ());
    CHECK(camera.getNearZ() > 0.0f);
}

TEST_CASE("Orbit controller only rotates while the left button is held")
{
    vkm::VkmCamera camera;
    vkm::VkmInputHandler input;
    vkm::VkmOrbitCameraController controller(&camera);
    controller.registerTo(input);
    controller.frame(glm::vec3(0.0f), 1.0f);
    // Rotation is measured as a fraction of the viewport, so the controller stays put until
    // the engine has published a viewport size.
    camera.setViewportSize(1280, 720);

    const float yawBefore = controller.getYaw();

    // Moving without dragging must not rotate, but it does establish the tracked position.
    input.onCursorMove(100.0, 100.0);
    input.onCursorMove(140.0, 100.0);
    pumpFrame(input);
    CHECK(controller.getYaw() == doctest::Approx(yawBefore));

    // Pressing drops the tracked position (it may be stale -- ImGui could have owned the
    // mouse), so the first move after the press only re-establishes it.
    input.onMouseButtonEvent(vkm::VkmMouseButton::Left, vkm::VkmKeyAction::Press);
    input.onCursorMove(140.0, 100.0);
    input.onCursorMove(160.0, 100.0);
    pumpFrame(input);
    CHECK(controller.getYaw() > yawBefore);

    const float yawWhileDragging = controller.getYaw();
    input.onMouseButtonEvent(vkm::VkmMouseButton::Left, vkm::VkmKeyAction::Release);
    input.onCursorMove(300.0, 100.0);
    pumpFrame(input);
    CHECK(controller.getYaw() == doctest::Approx(yawWhileDragging));
}

namespace
{
    // Presses, drags `deltaPixels` horizontally, and reports how far the yaw moved.
    float yawFromHorizontalDrag(uint32_t viewportHeight, double deltaPixels)
    {
        vkm::VkmCamera camera;
        vkm::VkmInputHandler input;
        vkm::VkmOrbitCameraController controller(&camera);
        controller.registerTo(input);
        camera.setViewportSize(1280, viewportHeight);

        const float yawBefore = controller.getYaw();
        input.onMouseButtonEvent(vkm::VkmMouseButton::Left, vkm::VkmKeyAction::Press);
        input.onCursorMove(0.0, 0.0);          // re-establishes the tracked position
        input.onCursorMove(deltaPixels, 0.0);  // the drag proper
        pumpFrame(input);
        return controller.getYaw() - yawBefore;
    }
}

TEST_CASE("Orbit controller rotates by a fraction of the viewport, not by pixels")
{
    // Cursor deltas arrive in framebuffer pixels, so a Retina display doubles them for the
    // same physical hand movement. Sensitivity is radians per full viewport height precisely
    // so that doubling the pixel density does not double the rotation.
    const float yawOnHalfHeight = yawFromHorizontalDrag(720, 100.0);
    const float yawOnFullHeight = yawFromHorizontalDrag(1440, 100.0);
    CHECK(yawOnHalfHeight == doctest::Approx(yawOnFullHeight * 2.0f));

    // The same drag expressed as the same fraction of the viewport rotates identically.
    CHECK(yawFromHorizontalDrag(1440, 200.0) == doctest::Approx(yawOnHalfHeight));
}

TEST_CASE("Orbit controller does not rotate before the viewport size is known")
{
    // The engine publishes the viewport during render(), but input is drained before that, so
    // the very first frame's moves arrive with the height still zero.
    vkm::VkmCamera camera;
    vkm::VkmInputHandler input;
    vkm::VkmOrbitCameraController controller(&camera);
    controller.registerTo(input);

    const float yawBefore = controller.getYaw();
    const float pitchBefore = controller.getPitch();

    input.onMouseButtonEvent(vkm::VkmMouseButton::Left, vkm::VkmKeyAction::Press);
    input.onCursorMove(0.0, 0.0);
    input.onCursorMove(200.0, 200.0);
    pumpFrame(input);

    CHECK(controller.getYaw() == doctest::Approx(yawBefore));
    CHECK(controller.getPitch() == doctest::Approx(pitchBefore));
}

TEST_CASE("Orbit controller clamps pitch short of the poles")
{
    vkm::VkmCamera camera;
    vkm::VkmInputHandler input;
    vkm::VkmOrbitCameraController controller(&camera);
    controller.registerTo(input);
    camera.setViewportSize(1280, 720);

    input.onMouseButtonEvent(vkm::VkmMouseButton::Left, vkm::VkmKeyAction::Press);
    input.onCursorMove(0.0, 0.0);
    for (int i = 0; i < 20; ++i)
    {
        input.onCursorMove(0.0, static_cast<double>((i + 1) * 100));
    }
    pumpFrame(input);

    CHECK(controller.getPitch() == doctest::Approx(1.5f));
}

TEST_CASE("Orbit controller dollies on scroll within the framed limits")
{
    vkm::VkmCamera camera;
    vkm::VkmInputHandler input;
    vkm::VkmOrbitCameraController controller(&camera);
    controller.registerTo(input);
    controller.frame(glm::vec3(0.0f), 4.0f); // distance 10, limits [0.5, 200]

    input.onScroll(0.0, 1.0);
    pumpFrame(input);
    CHECK(controller.getDistance() == doctest::Approx(9.0f)); // 10 * 0.9

    input.onScroll(0.0, -1.0);
    pumpFrame(input);
    CHECK(controller.getDistance() == doctest::Approx(10.0f));

    // Far past the near limit: the clamp holds and the camera stays on the orbit.
    for (int i = 0; i < 100; ++i)
    {
        input.onScroll(0.0, 5.0);
    }
    pumpFrame(input);
    CHECK(controller.getDistance() == doctest::Approx(0.5f));
    CHECK(glm::length(camera.getPosition()) == doctest::Approx(0.5f));
}

TEST_CASE("Orbit controller stops receiving events after unregister")
{
    vkm::VkmCamera camera;
    vkm::VkmInputHandler input;
    vkm::VkmOrbitCameraController controller(&camera);
    controller.registerTo(input);
    controller.frame(glm::vec3(0.0f), 1.0f);

    const float distanceBefore = controller.getDistance();
    controller.unregister();

    input.onScroll(0.0, 1.0);
    pumpFrame(input);

    CHECK(controller.getDistance() == doctest::Approx(distanceBefore));
}

namespace
{
    // Holds `key` down and runs one tick's worth of frames. Movement is polled rather than
    // event-driven, so the press has to be folded into the handler's state (beginFrame) before
    // tick() can see it.
    void holdKeyForTick(vkm::VkmInputHandler& input, vkm::VkmFlyCameraController& controller,
                        vkm::VkmKeyCode key, double deltaTime)
    {
        input.onKeyEvent(key, vkm::VkmKeyAction::Press);
        pumpFrame(input);
        controller.tick(deltaTime);
        input.onKeyEvent(key, vkm::VkmKeyAction::Release);
        pumpFrame(input);
    }
}

TEST_CASE("Fly controller moves along the camera basis at the configured speed")
{
    vkm::VkmCamera camera;
    vkm::VkmInputHandler input;
    vkm::VkmFlyCameraController controller(&camera);
    controller.registerTo(input);
    controller.setPosition(glm::vec3(0.0f));
    controller.setMoveSpeed(2.0f);

    // Yaw and pitch start at zero, so forward is +Z and screen-right is -X (the engine's
    // right-handed convention -- see VkmCamera::getView).
    holdKeyForTick(input, controller, vkm::VkmKeyCode::W, 0.5);
    CHECK(controller.getPosition().x == doctest::Approx(0.0f));
    CHECK(controller.getPosition().y == doctest::Approx(0.0f));
    CHECK(controller.getPosition().z == doctest::Approx(1.0f)); // 2 u/s * 0.5 s

    controller.setPosition(glm::vec3(0.0f));
    holdKeyForTick(input, controller, vkm::VkmKeyCode::S, 0.5);
    CHECK(controller.getPosition().z == doctest::Approx(-1.0f));

    controller.setPosition(glm::vec3(0.0f));
    holdKeyForTick(input, controller, vkm::VkmKeyCode::D, 0.5);
    CHECK(controller.getPosition().x == doctest::Approx(-1.0f));
    CHECK(controller.getPosition().z == doctest::Approx(0.0f));

    // Up and down follow world up, not the camera's, so they stay vertical when pitched.
    controller.setPosition(glm::vec3(0.0f));
    holdKeyForTick(input, controller, vkm::VkmKeyCode::E, 0.5);
    CHECK(controller.getPosition().y == doctest::Approx(1.0f));

    controller.setPosition(glm::vec3(0.0f));
    holdKeyForTick(input, controller, vkm::VkmKeyCode::Q, 0.5);
    CHECK(controller.getPosition().y == doctest::Approx(-1.0f));
}

TEST_CASE("Fly controller movement is proportional to elapsed time, not to frame count")
{
    vkm::VkmCamera camera;
    vkm::VkmInputHandler input;
    vkm::VkmFlyCameraController controller(&camera);
    controller.registerTo(input);
    controller.setMoveSpeed(1.0f);
    controller.setPosition(glm::vec3(0.0f));

    input.onKeyEvent(vkm::VkmKeyCode::W, vkm::VkmKeyAction::Press);
    pumpFrame(input);

    controller.tick(0.25);
    controller.tick(0.25);
    const float twoShortFrames = controller.getPosition().z;

    controller.setPosition(glm::vec3(0.0f));
    controller.tick(0.5);
    CHECK(controller.getPosition().z == doctest::Approx(twoShortFrames));
    CHECK(twoShortFrames == doctest::Approx(0.5f));
}

TEST_CASE("Fly controller boosts while shift is held and normalizes diagonals")
{
    vkm::VkmCamera camera;
    vkm::VkmInputHandler input;
    vkm::VkmFlyCameraController controller(&camera);
    controller.registerTo(input);
    controller.setMoveSpeed(1.0f);
    controller.setBoostMultiplier(3.0f);

    controller.setPosition(glm::vec3(0.0f));
    input.onKeyEvent(vkm::VkmKeyCode::W, vkm::VkmKeyAction::Press);
    input.onKeyEvent(vkm::VkmKeyCode::LeftShift, vkm::VkmKeyAction::Press);
    pumpFrame(input);
    controller.tick(1.0);
    CHECK(controller.getPosition().z == doctest::Approx(3.0f));

    // Two axes at once must not travel further than one; the direction is normalized first.
    controller.setPosition(glm::vec3(0.0f));
    input.onKeyEvent(vkm::VkmKeyCode::LeftShift, vkm::VkmKeyAction::Release);
    input.onKeyEvent(vkm::VkmKeyCode::D, vkm::VkmKeyAction::Press);
    pumpFrame(input);
    controller.tick(1.0);
    CHECK(glm::length(controller.getPosition()) == doctest::Approx(1.0f));
}

TEST_CASE("Fly controller looks on left-drag only, and clamps pitch short of the poles")
{
    vkm::VkmCamera camera;
    vkm::VkmInputHandler input;
    vkm::VkmFlyCameraController controller(&camera);
    controller.registerTo(input);
    controller.setPosition(glm::vec3(1.0f, 2.0f, 3.0f));
    // Looking is measured as a fraction of the viewport, so nothing turns until the engine has
    // published a viewport size.
    camera.setViewportSize(1280, 720);

    const float yawBefore = controller.getYaw();
    input.onCursorMove(100.0, 100.0);
    input.onCursorMove(140.0, 100.0);
    pumpFrame(input);
    CHECK(controller.getYaw() == doctest::Approx(yawBefore));

    input.onMouseButtonEvent(vkm::VkmMouseButton::Left, vkm::VkmKeyAction::Press);
    input.onCursorMove(140.0, 100.0);
    input.onCursorMove(160.0, 100.0);
    pumpFrame(input);
    CHECK(controller.getYaw() > yawBefore);
    // Looking around must not move the camera.
    CHECK(controller.getPosition().x == doctest::Approx(1.0f));
    CHECK(controller.getPosition().z == doctest::Approx(3.0f));

    for (int i = 0; i < 20; ++i)
    {
        input.onCursorMove(160.0, static_cast<double>((i + 1) * 100));
    }
    pumpFrame(input);
    CHECK(controller.getPitch() == doctest::Approx(1.5f));
}

TEST_CASE("Fly controller adopts the camera's current view on syncFromCamera")
{
    vkm::VkmCamera camera;
    vkm::VkmFlyCameraController controller(&camera);

    const glm::vec3 eye(4.0f, 5.0f, 6.0f);
    camera.lookAt(eye, eye + glm::vec3(1.0f, 0.0f, 0.0f));
    controller.syncFromCamera();

    CHECK(controller.getPosition().x == doctest::Approx(eye.x));
    CHECK(controller.getPosition().y == doctest::Approx(eye.y));
    CHECK(controller.getPosition().z == doctest::Approx(eye.z));
    CHECK(controller.getPitch() == doctest::Approx(0.0f));

    // Round-trip: the view the controller re-applies must look the same way it was handed.
    const glm::vec3 target = camera.getTarget() - camera.getPosition();
    CHECK(target.x == doctest::Approx(1.0f));
    CHECK(target.y == doctest::Approx(0.0f).epsilon(1e-5));
    CHECK(target.z == doctest::Approx(0.0f).epsilon(1e-5));
}

TEST_CASE("Fly controller stops moving once its window loses focus")
{
    vkm::VkmCamera camera;
    vkm::VkmInputHandler input;
    vkm::VkmFlyCameraController controller(&camera);
    controller.registerTo(input);
    controller.setMoveSpeed(1.0f);
    controller.setPosition(glm::vec3(0.0f));

    input.onKeyEvent(vkm::VkmKeyCode::W, vkm::VkmKeyAction::Press);
    pumpFrame(input);
    controller.tick(1.0);
    CHECK(controller.getPosition().z == doctest::Approx(1.0f));

    // The matching key release goes to whichever window took focus, so without the focus event
    // clearing held state the camera would fly on forever.
    input.onWindowFocusChanged(0, false);
    pumpFrame(input);
    controller.tick(1.0);
    CHECK(controller.getPosition().z == doctest::Approx(1.0f));
}

TEST_CASE("Fly controller stops receiving events after unregister")
{
    vkm::VkmCamera camera;
    vkm::VkmInputHandler input;
    vkm::VkmFlyCameraController controller(&camera);
    controller.registerTo(input);
    controller.setPosition(glm::vec3(0.0f));

    const float yawBefore = controller.getYaw();
    controller.unregister();

    input.onMouseButtonEvent(vkm::VkmMouseButton::Left, vkm::VkmKeyAction::Press);
    input.onCursorMove(0.0, 0.0);
    input.onCursorMove(200.0, 0.0);
    input.onKeyEvent(vkm::VkmKeyCode::W, vkm::VkmKeyAction::Press);
    pumpFrame(input);
    // tick() polls the handler, so unregistering has to stop that too, not just the listener.
    controller.tick(1.0);

    CHECK(controller.getYaw() == doctest::Approx(yawBefore));
    CHECK(controller.getPosition().z == doctest::Approx(0.0f));
}
