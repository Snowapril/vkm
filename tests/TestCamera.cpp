#include <doctest/doctest.h>

#include <vkm/platform/common/input_handler.h>
#include <vkm/renderer/camera.h>

#include <glm/vec4.hpp>

#include <cmath>

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

TEST_CASE("Orbit controller clamps pitch short of the poles")
{
    vkm::VkmCamera camera;
    vkm::VkmInputHandler input;
    vkm::VkmOrbitCameraController controller(&camera);
    controller.registerTo(input);

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
