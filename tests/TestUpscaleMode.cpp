#include <doctest/doctest.h>

#include <vkm/renderer/backend/common/upscaler.h>

#include <set>
#include <string>

// The upscale presets are pure functions of a mode and a display extent, so the whole policy --
// the scale table, the F2 cycle order and the shared rounding rule -- is testable with no GPU.

TEST_CASE("VkmUpscaleMode - scales match the vendor presets") {
    // Off and Native both render at the display extent; only the mode says whether an upscaler
    // runs there at all.
    CHECK(vkm::vkmUpscaleModeScale(vkm::VkmUpscaleMode::Off) == doctest::Approx(1.0f));
    CHECK(vkm::vkmUpscaleModeScale(vkm::VkmUpscaleMode::Native) == doctest::Approx(1.0f));
    CHECK(vkm::vkmUpscaleModeScale(vkm::VkmUpscaleMode::Quality) == doctest::Approx(0.67f));
    CHECK(vkm::vkmUpscaleModeScale(vkm::VkmUpscaleMode::Balanced) == doctest::Approx(0.59f));
    CHECK(vkm::vkmUpscaleModeScale(vkm::VkmUpscaleMode::Performance) == doctest::Approx(0.50f));

    // Both vendors cap the ratio at 3x, so no preset may ask for more than that: a scale below
    // 1/3 would fail as a null upscaler rather than a clear rejection.
    for (uint32_t i = 0; i < static_cast<uint32_t>(vkm::VkmUpscaleMode::Count); ++i)
    {
        const float scale = vkm::vkmUpscaleModeScale(static_cast<vkm::VkmUpscaleMode>(i));
        CHECK(scale >= 1.0f / 3.0f);
        CHECK(scale <= 1.0f);
    }
}

TEST_CASE("VkmUpscaleMode - F2 cycles every preset and wraps") {
    CHECK(vkm::vkmNextUpscaleMode(vkm::VkmUpscaleMode::Off) == vkm::VkmUpscaleMode::Native);
    CHECK(vkm::vkmNextUpscaleMode(vkm::VkmUpscaleMode::Native) == vkm::VkmUpscaleMode::Quality);
    CHECK(vkm::vkmNextUpscaleMode(vkm::VkmUpscaleMode::Quality) == vkm::VkmUpscaleMode::Balanced);
    CHECK(vkm::vkmNextUpscaleMode(vkm::VkmUpscaleMode::Balanced) == vkm::VkmUpscaleMode::Performance);
    CHECK(vkm::vkmNextUpscaleMode(vkm::VkmUpscaleMode::Performance) == vkm::VkmUpscaleMode::Off);

    // Count presses return to the starting mode from anywhere, so the cycle can never strand a
    // user on a mode they cannot leave.
    for (uint32_t start = 0; start < static_cast<uint32_t>(vkm::VkmUpscaleMode::Count); ++start)
    {
        vkm::VkmUpscaleMode mode = static_cast<vkm::VkmUpscaleMode>(start);
        for (uint32_t step = 0; step < static_cast<uint32_t>(vkm::VkmUpscaleMode::Count); ++step)
        {
            mode = vkm::vkmNextUpscaleMode(mode);
        }
        CHECK(mode == static_cast<vkm::VkmUpscaleMode>(start));
    }
}

TEST_CASE("VkmUpscaleMode - every preset has a distinct name") {
    std::set<std::string> names;
    for (uint32_t i = 0; i < static_cast<uint32_t>(vkm::VkmUpscaleMode::Count); ++i)
    {
        const char* name = vkm::vkmUpscaleModeName(static_cast<vkm::VkmUpscaleMode>(i));
        REQUIRE(name != nullptr);
        names.insert(name);
    }
    CHECK(names.size() == static_cast<size_t>(vkm::VkmUpscaleMode::Count));
}

TEST_CASE("VkmUpscaleRenderExtent - Off and Native share an extent, which is why the mode is the key") {
    const glm::uvec2 display(1920u, 1080u);

    // The whole reason a consumer's rebuild test must compare the MODE and not just the extents:
    // these two produce identical extents while differing in whether an upscaler exists.
    CHECK(vkm::vkmUpscaleRenderExtent(display, vkm::VkmUpscaleMode::Off) == display);
    CHECK(vkm::vkmUpscaleRenderExtent(display, vkm::VkmUpscaleMode::Native) == display);
    CHECK(vkm::vkmUpscaleRenderExtent(display, vkm::VkmUpscaleMode::Off) ==
          vkm::vkmUpscaleRenderExtent(display, vkm::VkmUpscaleMode::Native));
}

TEST_CASE("VkmUpscaleRenderExtent - rounds to nearest and never reaches zero") {
    const glm::uvec2 display(1920u, 1080u);
    CHECK(vkm::vkmUpscaleRenderExtent(display, vkm::VkmUpscaleMode::Quality) == glm::uvec2(1286u, 724u));
    CHECK(vkm::vkmUpscaleRenderExtent(display, vkm::VkmUpscaleMode::Balanced) == glm::uvec2(1133u, 637u));
    CHECK(vkm::vkmUpscaleRenderExtent(display, vkm::VkmUpscaleMode::Performance) == glm::uvec2(960u, 540u));

    // A one-pixel window still yields a legal extent rather than a zero-sized texture.
    CHECK(vkm::vkmUpscaleRenderExtent(glm::uvec2(1u, 1u), vkm::VkmUpscaleMode::Performance) ==
          glm::uvec2(1u, 1u));

    // The render extent never exceeds the display extent, which VkmUpscalerBase::initialize
    // rejects outright.
    for (uint32_t i = 0; i < static_cast<uint32_t>(vkm::VkmUpscaleMode::Count); ++i)
    {
        const glm::uvec2 render =
            vkm::vkmUpscaleRenderExtent(display, static_cast<vkm::VkmUpscaleMode>(i));
        CHECK(render.x <= display.x);
        CHECK(render.y <= display.y);
    }
}
