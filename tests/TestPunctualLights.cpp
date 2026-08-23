// Copyright (c) 2026 Snowapril
//
// glTF's punctual attenuation, asserted against its closed forms.
//
// These are CPU mirrors of vkm_punctual_lights.hlsli's two functions. A shading test cannot
// localise a wrong falloff -- a squared-instead-of-linear range window or a missing cone term
// both just make the image "a bit off" -- so the exact ratios are pinned here, and the GPU gate
// in TestGBufferRenderShared checks that the shader agrees with them.

#include <doctest/doctest.h>

#include <vkm/renderer/deferred_lighting.h>
#include <vkm/renderer/scene/light_table.h>

#include <algorithm>
#include <cmath>

namespace
{
    // Mirrors vkmPunctualDistanceAttenuation.
    float distanceAttenuation(float distance, float range)
    {
        const float inverseSquare = 1.0f / std::max(distance * distance, 1e-8f);
        if (range <= 0.0f)
        {
            return inverseSquare;
        }
        const float ratio = distance / range;
        const float ratio4 = ratio * ratio * ratio * ratio;
        const float window = std::clamp(1.0f - ratio4, 0.0f, 1.0f);
        return window * window * inverseSquare;
    }

    // Mirrors vkmPunctualSpotAttenuation.
    float spotAttenuation(float cosAngle, float cosInner, float cosOuter)
    {
        const float denominator = cosInner - cosOuter;
        if (denominator <= 1e-6f)
        {
            return cosAngle > cosOuter ? 1.0f : 0.0f;
        }
        return std::clamp((cosAngle - cosOuter) / denominator, 0.0f, 1.0f);
    }
} // namespace

TEST_CASE("punctual attenuation - an unranged light is pure inverse square") {
    // The ratio is the assertion, not the absolute value: doubling the distance must quarter the
    // irradiance. A linear or unsquared falloff passes neither.
    CHECK(distanceAttenuation(1.0f, 0.0f) / distanceAttenuation(2.0f, 0.0f) == doctest::Approx(4.0f));
    CHECK(distanceAttenuation(3.0f, 0.0f) / distanceAttenuation(6.0f, 0.0f) == doctest::Approx(4.0f));
    CHECK(distanceAttenuation(2.0f, 0.0f) == doctest::Approx(0.25f));
}

TEST_CASE("punctual attenuation - glTF's range window closes exactly at the range") {
    constexpr float kRange = 10.0f;

    // Exactly zero at the range, not merely small: a light with a range must be bounded, which is
    // the whole reason the window exists.
    CHECK(distanceAttenuation(kRange, kRange) == doctest::Approx(0.0f));
    CHECK(distanceAttenuation(kRange * 1.5f, kRange) == doctest::Approx(0.0f));

    // Halfway out, the window is (1 - (1/2)^4)^2 = (15/16)^2, on top of 1/d^2.
    const float expected = (15.0f / 16.0f) * (15.0f / 16.0f) / 25.0f;
    CHECK(distanceAttenuation(5.0f, kRange) == doctest::Approx(expected));

    // Near the light the window is ~1, so the ranged and unranged forms agree.
    CHECK(distanceAttenuation(0.1f, kRange) == doctest::Approx(distanceAttenuation(0.1f, 0.0f)).epsilon(1e-4));
}

TEST_CASE("punctual attenuation - the spot cone is full inside, zero outside, half at the midpoint") {
    const float cosInner = std::cos(0.3f);
    const float cosOuter = std::cos(0.6f);

    CHECK(spotAttenuation(std::cos(0.0f), cosInner, cosOuter) == doctest::Approx(1.0f));
    CHECK(spotAttenuation(cosInner, cosInner, cosOuter) == doctest::Approx(1.0f));
    CHECK(spotAttenuation(cosOuter, cosInner, cosOuter) == doctest::Approx(0.0f));
    CHECK(spotAttenuation(std::cos(1.0f), cosInner, cosOuter) == doctest::Approx(0.0f));

    // glTF interpolates in cosine, not in angle, so the midpoint is the cosine midpoint.
    CHECK(spotAttenuation(0.5f * (cosInner + cosOuter), cosInner, cosOuter) == doctest::Approx(0.5f));
}

TEST_CASE("punctual attenuation - equal cone angles are a hard edge, not a division by zero") {
    const float cosAngle = std::cos(0.4f);
    const float cosEqual = std::cos(0.5f);

    const float inside = spotAttenuation(cosAngle, cosEqual, cosEqual);
    const float outside = spotAttenuation(std::cos(0.6f), cosEqual, cosEqual);
    CHECK(std::isfinite(inside));
    CHECK(std::isfinite(outside));
    CHECK(inside == doctest::Approx(1.0f));
    CHECK(outside == doctest::Approx(0.0f));
}

TEST_CASE("VkmDeferredLightConstants - layout matches the shader's mirror") {
    // The shader reads this as a uniform block, so a size drift is a silent miscompare rather
    // than an error.
    CHECK(sizeof(vkm::VkmPunctualLight) == 64);
    CHECK(sizeof(vkm::VkmDeferredLightConstants) == 16 + 64 * vkm::kVkmMaxPunctualLights);

    const vkm::VkmDeferredLightConstants constants{};
    CHECK(constants._lightCount.x == 0u);
    // A non-spot must be inside its own cone from every direction, or a point light would shade
    // as a zero-width spot.
    CHECK(constants._lights[0]._cosOuter == doctest::Approx(-1.0f));
    CHECK(constants._lights[0]._shadowTile == -1);
}
