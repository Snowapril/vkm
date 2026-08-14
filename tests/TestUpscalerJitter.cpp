#include <doctest/doctest.h>

#include <vkm/renderer/backend/common/upscaler.h>

#include <cmath>

// The jitter helpers are pure functions, so the sequence a temporal upscaler sees is testable
// without any GPU: known Halton values, the vendor-recommended phase count, and the offset range.

TEST_CASE("vkmHalton - matches the radical-inverse sequence in bases 2 and 3") {
    // Base 2: 1/2, 1/4, 3/4, 1/8, 5/8, 3/8, 7/8 ...
    CHECK(vkm::vkmHalton(1, 2) == doctest::Approx(0.5f));
    CHECK(vkm::vkmHalton(2, 2) == doctest::Approx(0.25f));
    CHECK(vkm::vkmHalton(3, 2) == doctest::Approx(0.75f));
    CHECK(vkm::vkmHalton(4, 2) == doctest::Approx(0.125f));
    CHECK(vkm::vkmHalton(5, 2) == doctest::Approx(0.625f));

    // Base 3: 1/3, 2/3, 1/9, 4/9, 7/9 ...
    CHECK(vkm::vkmHalton(1, 3) == doctest::Approx(1.0f / 3.0f));
    CHECK(vkm::vkmHalton(2, 3) == doctest::Approx(2.0f / 3.0f));
    CHECK(vkm::vkmHalton(3, 3) == doctest::Approx(1.0f / 9.0f));
    CHECK(vkm::vkmHalton(4, 3) == doctest::Approx(4.0f / 9.0f));

    // Index 0 is out of the sequence and returns 0; the jitter helper skips it.
    CHECK(vkm::vkmHalton(0, 2) == 0.0f);
}

TEST_CASE("vkmUpscaleJitterPhaseCount - ceil(8 * ratio^2)") {
    CHECK(vkm::vkmUpscaleJitterPhaseCount(1920, 1920) == 8);   // 1x
    CHECK(vkm::vkmUpscaleJitterPhaseCount(1280, 1920) == 18);  // 1.5x
    CHECK(vkm::vkmUpscaleJitterPhaseCount(960, 1920) == 32);   // 2x
    CHECK(vkm::vkmUpscaleJitterPhaseCount(640, 1920) == 72);   // 3x
}

TEST_CASE("vkmUpscaleJitterPixels - offsets stay in [-0.5, 0.5) and cycle by phase count") {
    const uint32_t phaseCount = vkm::vkmUpscaleJitterPhaseCount(960, 1920);
    for (uint64_t frame = 0; frame < 2 * phaseCount; ++frame)
    {
        const glm::vec2 jitter = vkm::vkmUpscaleJitterPixels(frame, phaseCount);
        CHECK(jitter.x >= -0.5f);
        CHECK(jitter.x < 0.5f);
        CHECK(jitter.y >= -0.5f);
        CHECK(jitter.y < 0.5f);
    }

    // The sequence repeats after exactly phaseCount frames and never repeats inside one cycle's
    // first two samples (a stuck jitter would degrade every temporal accumulation).
    const glm::vec2 first = vkm::vkmUpscaleJitterPixels(0, phaseCount);
    const glm::vec2 second = vkm::vkmUpscaleJitterPixels(1, phaseCount);
    const glm::vec2 wrapped = vkm::vkmUpscaleJitterPixels(phaseCount, phaseCount);
    CHECK(first.x == doctest::Approx(wrapped.x));
    CHECK(first.y == doctest::Approx(wrapped.y));
    CHECK((first.x != second.x || first.y != second.y));
}
