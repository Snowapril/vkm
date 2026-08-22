// Copyright (c) 2026 Snowapril
//
// The MSE / RelMSE comparison utility Phase 6 exists to make later phases quotable with. No GPU:
// the path tracer cannot produce an image without a device, so the metric is exercised from
// hand-built buffers in the accumulator's own layout (rgb summed, a = sample count).

#include <doctest/doctest.h>

#include <vkm/renderer/path_tracer.h>

#include <vector>

TEST_CASE("vkmComputeImageMse / vkmComputeImageRelativeMse - accumulator layout and weighting")
{
    // Two samples accumulated into each pixel, so the normalization by alpha is load-bearing:
    // compared raw, these images differ by a factor of two everywhere.
    const std::vector<float> reference{ 1.0f, 2.0f, 4.0f, 2.0f,
                                        0.0f, 0.0f, 0.0f, 2.0f };
    const std::vector<float> identical{ 2.0f, 4.0f, 8.0f, 4.0f,
                                        0.0f, 0.0f, 0.0f, 4.0f };
    CHECK(vkm::vkmComputeImageMse(reference.data(), identical.data(), 2) == doctest::Approx(0.0f));

    // Pixel 0 is off by 1 in every channel after normalization; pixel 1 matches. Averaged over
    // 2 pixels * 3 channels that is 3/6 = 0.5.
    // One sample, so these ARE the normalized values: (1.5, 2, 3) against the reference's
    // (0.5, 1, 2) is exactly +1 per channel.
    const std::vector<float> offByOne{ 1.5f, 2.0f, 3.0f, 1.0f,
                                       0.0f, 0.0f, 0.0f, 1.0f };
    CHECK(vkm::vkmComputeImageMse(reference.data(), offByOne.data(), 2) == doctest::Approx(0.5f));

    // A pixel nothing was accumulated into is unknown, not black: excluded from both the sum
    // and the divisor, so it cannot flatter or penalise a comparison.
    const std::vector<float> halfUnsampled{ 1.5f, 2.0f, 3.0f, 1.0f,
                                            9.0f, 9.0f, 9.0f, 0.0f };
    CHECK(vkm::vkmComputeImageMse(reference.data(), halfUnsampled.data(), 2) == doctest::Approx(1.0f));

    // RelMSE divides by the reference's own magnitude, so the same absolute error counts for far
    // more where the reference is dim. Pixel 0's reference channels are 0.5, 1 and 2 against an
    // error of 1 each, giving 1/(0.25+eps) + 1/(1+eps) + 1/(4+eps) -- nearly 4 from the dimmest
    // channel alone and a quarter from the brightest, which is the weighting plain MSE lacks.
    // Pixel 0's dim channel is what the epsilon bounds; a black reference channel with no error
    // contributes nothing either way.
    const float relative = vkm::vkmComputeImageRelativeMse(reference.data(), offByOne.data(), 2);
    const float plain = vkm::vkmComputeImageMse(reference.data(), offByOne.data(), 2);
    CHECK(relative > plain);
    // Identical images score zero on both, which is the only value a metric must never get
    // wrong.
    CHECK(vkm::vkmComputeImageRelativeMse(reference.data(), identical.data(), 2) ==
          doctest::Approx(0.0f));
}
