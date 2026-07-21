#include <doctest/doctest.h>

#include <vkm/platform/common/input_handler.h>

#include <string>
#include <vector>

TEST_CASE("InputHandler key press sets down and pressed edge for one frame only")
{
    vkm::VkmInputHandler input;

    input.onKeyEvent(vkm::VkmKeyCode::A, vkm::VkmKeyAction::Press);
    input.beginFrame();

    CHECK(input.isKeyDown(vkm::VkmKeyCode::A));
    CHECK(input.isKeyPressed(vkm::VkmKeyCode::A));
    CHECK(input.isKeyReleased(vkm::VkmKeyCode::A) == false);

    input.beginFrame();

    CHECK(input.isKeyDown(vkm::VkmKeyCode::A));
    CHECK(input.isKeyPressed(vkm::VkmKeyCode::A) == false);
}

TEST_CASE("InputHandler key release clears down and sets released edge for one frame only")
{
    vkm::VkmInputHandler input;

    input.onKeyEvent(vkm::VkmKeyCode::B, vkm::VkmKeyAction::Press);
    input.beginFrame();

    input.onKeyEvent(vkm::VkmKeyCode::B, vkm::VkmKeyAction::Release);
    input.beginFrame();

    CHECK(input.isKeyDown(vkm::VkmKeyCode::B) == false);
    CHECK(input.isKeyReleased(vkm::VkmKeyCode::B));

    input.beginFrame();

    CHECK(input.isKeyReleased(vkm::VkmKeyCode::B) == false);
}

TEST_CASE("InputHandler registers both edges when press and release land in the same frame")
{
    vkm::VkmInputHandler input;

    input.onKeyEvent(vkm::VkmKeyCode::Space, vkm::VkmKeyAction::Press);
    input.onKeyEvent(vkm::VkmKeyCode::Space, vkm::VkmKeyAction::Release);
    input.beginFrame();

    CHECK(input.isKeyPressed(vkm::VkmKeyCode::Space));
    CHECK(input.isKeyReleased(vkm::VkmKeyCode::Space));
    CHECK(input.isKeyDown(vkm::VkmKeyCode::Space) == false);
}

TEST_CASE("InputHandler auto-repeat keeps the key down without re-triggering the pressed edge")
{
    vkm::VkmInputHandler input;

    input.onKeyEvent(vkm::VkmKeyCode::C, vkm::VkmKeyAction::Press);
    input.beginFrame();

    input.onKeyEvent(vkm::VkmKeyCode::C, vkm::VkmKeyAction::Press, 0, true);
    input.beginFrame();

    CHECK(input.isKeyDown(vkm::VkmKeyCode::C));
    CHECK(input.isKeyPressed(vkm::VkmKeyCode::C) == false);
}

TEST_CASE("InputHandler mouse buttons mirror the key edge behaviour")
{
    vkm::VkmInputHandler input;

    input.onMouseButtonEvent(vkm::VkmMouseButton::Left, vkm::VkmKeyAction::Press);
    input.beginFrame();

    CHECK(input.isMouseButtonDown(vkm::VkmMouseButton::Left));
    CHECK(input.isMouseButtonPressed(vkm::VkmMouseButton::Left));

    input.onMouseButtonEvent(vkm::VkmMouseButton::Left, vkm::VkmKeyAction::Release);
    input.beginFrame();

    CHECK(input.isMouseButtonDown(vkm::VkmMouseButton::Left) == false);
    CHECK(input.isMouseButtonReleased(vkm::VkmMouseButton::Left));
    CHECK(input.isMouseButtonPressed(vkm::VkmMouseButton::Left) == false);
}

TEST_CASE("InputHandler cursor delta is zero on the first move and resets on an idle frame")
{
    vkm::VkmInputHandler input;

    input.onCursorMove(100.0, 50.0);
    input.beginFrame();

    CHECK(input.getCursorX() == doctest::Approx(100.0));
    CHECK(input.getCursorY() == doctest::Approx(50.0));
    CHECK(input.getCursorDeltaX() == doctest::Approx(0.0));
    CHECK(input.getCursorDeltaY() == doctest::Approx(0.0));

    input.onCursorMove(130.0, 45.0);
    input.beginFrame();

    CHECK(input.getCursorDeltaX() == doctest::Approx(30.0));
    CHECK(input.getCursorDeltaY() == doctest::Approx(-5.0));

    input.beginFrame();

    CHECK(input.getCursorDeltaX() == doctest::Approx(0.0));
    CHECK(input.getCursorDeltaY() == doctest::Approx(0.0));
    CHECK(input.getCursorX() == doctest::Approx(130.0));
}

TEST_CASE("InputHandler accumulates scroll within a frame and resets across frames")
{
    vkm::VkmInputHandler input;

    input.onScroll(1.0, 2.0);
    input.onScroll(0.5, -1.0);
    input.beginFrame();

    CHECK(input.getScrollDeltaX() == doctest::Approx(1.5));
    CHECK(input.getScrollDeltaY() == doctest::Approx(1.0));

    input.beginFrame();

    CHECK(input.getScrollDeltaX() == doctest::Approx(0.0));
    CHECK(input.getScrollDeltaY() == doctest::Approx(0.0));
}

TEST_CASE("InputHandler modifiers reflect the most recent event")
{
    vkm::VkmInputHandler input;

    const uint32_t shiftAndControl =
        static_cast<uint32_t>(vkm::VkmInputModifier::Shift) |
        static_cast<uint32_t>(vkm::VkmInputModifier::Control);

    input.onKeyEvent(vkm::VkmKeyCode::D, vkm::VkmKeyAction::Press, shiftAndControl);
    input.beginFrame();

    CHECK(input.getModifiers() == shiftAndControl);
    CHECK(input.hasModifier(vkm::VkmInputModifier::Shift));
    CHECK(input.hasModifier(vkm::VkmInputModifier::Control));
    CHECK(input.hasModifier(vkm::VkmInputModifier::Alt) == false);

    input.onKeyEvent(vkm::VkmKeyCode::D, vkm::VkmKeyAction::Release, 0);
    input.beginFrame();

    CHECK(input.getModifiers() == 0);
}

TEST_CASE("InputHandler dispatches events to listeners during beginFrame only")
{
    vkm::VkmInputHandler input;

    // Names rather than enumerators: doctest cannot stringify VkmKeyCode for failure messages.
    std::vector<std::string> received;
    const uint32_t handle = input.addListener([&received](const vkm::VkmInputEvent& event)
    {
        if (event._type == vkm::VkmInputEventType::Key)
        {
            received.push_back(vkm::toString(event._key));
        }
    });

    input.onKeyEvent(vkm::VkmKeyCode::E, vkm::VkmKeyAction::Press);
    CHECK(received.empty());

    input.onKeyEvent(vkm::VkmKeyCode::F, vkm::VkmKeyAction::Press);
    input.beginFrame();

    REQUIRE(received.size() == 2);
    CHECK(received[0] == "E");
    CHECK(received[1] == "F");

    input.removeListener(handle);
    input.onKeyEvent(vkm::VkmKeyCode::G, vkm::VkmKeyAction::Press);
    input.beginFrame();

    CHECK(received.size() == 2);
}

TEST_CASE("InputHandler latches exit on escape press before any frame is drained")
{
    vkm::VkmInputHandler input;

    CHECK(input.shouldExit() == false);

    input.onKeyEvent(vkm::VkmKeyCode::Escape, vkm::VkmKeyAction::Press);

    CHECK(input.shouldExit());
}

TEST_CASE("InputHandler stringifies key, button and action enumerators")
{
    CHECK(std::string(vkm::toString(vkm::VkmKeyCode::Escape)) == "Escape");
    CHECK(std::string(vkm::toString(vkm::VkmKeyCode::KeypadEqual)) == "KeypadEqual");
    CHECK(std::string(vkm::toString(vkm::VkmMouseButton::Middle)) == "Middle");
    CHECK(std::string(vkm::toString(vkm::VkmKeyAction::Release)) == "Release");
}
