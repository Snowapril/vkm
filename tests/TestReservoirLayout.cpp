// Copyright (c) 2025 Snowapril
//
// The reservoir's packing is an ABI in the strongest sense this engine has: a screen's worth is
// millions of records, every ReSTIR pass reads and writes them, and the shader side mirrors these
// numbers by hand. Nothing in the build fails if the two drift, so they are asserted here --
// the same reason TestObjectDataLayout exists.

#include <doctest/doctest.h>

#include <vkm/renderer/restir.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

TEST_CASE("VkmReservoir - the packed record is 32 bytes of eight u32 words")
{
    // Mirrors VKM_RESERVOIR_WORD_STRIDE in vkm_reservoir.hlsli. restir.md section 8.1 budgets
    // ~32 B per reservoir and ~16 MB per slice at half-res 1080p; the second number follows from
    // the first, so it is the first that has to be pinned.
    CHECK(vkm::kVkmReservoirWordStride == 8);
    CHECK(vkm::kVkmReservoirByteStride == 32);

    // 960x540 (half-res 1080p), one slice.
    const uint64_t halfRes1080pSliceBytes = 960ull * 540ull * vkm::kVkmReservoirByteStride;
    CHECK(halfRes1080pSliceBytes < 17ull * 1024ull * 1024ull);
}

TEST_CASE("VkmRestirConstants - matches the shader-side push-constant record")
{
    CHECK(sizeof(vkm::VkmRestirConstants) == 56);
    // Well inside the 128 bytes every device guarantees, which is what the engine's whole
    // push-constant convention is sized against.
    CHECK(sizeof(vkm::VkmRestirConstants) <= 128);

    CHECK(offsetof(vkm::VkmRestirConstants, _width) == 0);
    CHECK(offsetof(vkm::VkmRestirConstants, _height) == 4);
    CHECK(offsetof(vkm::VkmRestirConstants, _sampleIndex) == 8);
    CHECK(offsetof(vkm::VkmRestirConstants, _maxBounces) == 12);
    CHECK(offsetof(vkm::VkmRestirConstants, _environmentR) == 16);
    CHECK(offsetof(vkm::VkmRestirConstants, _outputSlice) == 28);
    CHECK(offsetof(vkm::VkmRestirConstants, _inputSlice) == 32);
    CHECK(offsetof(vkm::VkmRestirConstants, _neighbourCount) == 36);
    CHECK(offsetof(vkm::VkmRestirConstants, _neighbourRadius) == 40);
    CHECK(offsetof(vkm::VkmRestirConstants, _normalThreshold) == 44);
    CHECK(offsetof(vkm::VkmRestirConstants, _depthThreshold) == 48);
}

/*
* Two slices, deliberately not FRAME_COUNT. A pass reads one and writes the other, which is what
* 8.4's spatial reuse needs; the frames-in-flight question is answered by VkmRenderGraph's
* per-slot ensureCompleted() instead. See the comment on kVkmReservoirSliceCount.
*/
TEST_CASE("kVkmReservoirSliceCount - enough for a read slice and a write slice")
{
    CHECK(vkm::kVkmReservoirSliceCount >= 2);
}

/*
* Phase 8.2. The spatial pass reads a *run* of consecutive entries, so what matters is not just
* that the set covers the disk but that any short run of it does -- which is the property R2 has
* and a golden-angle spiral does not.
*/
TEST_CASE("vkmBuildNeighbourOffsets - low-discrepancy points inside the unit disk")
{
    std::vector<float> offsets(vkm::kVkmNeighbourOffsetCount * 2);
    vkm::vkmBuildNeighbourOffsets(offsets.data(), vkm::kVkmNeighbourOffsetCount);

    double meanX = 0.0;
    double meanY = 0.0;
    float largestRadius = 0.0f;
    for (uint32_t i = 0; i < vkm::kVkmNeighbourOffsetCount; ++i)
    {
        const float x = offsets[i * 2 + 0];
        const float y = offsets[i * 2 + 1];
        const float radius = std::sqrt(x * x + y * y);
        // Inside the disk, or the spatial pass's radius would not mean what it says.
        CHECK(radius <= 1.0001f);
        largestRadius = std::max(largestRadius, radius);
        meanX += x;
        meanY += y;
    }
    // Reaches the rim: a distribution bunched at the centre would make every neighbour a near
    // neighbour, which is the failure mode the polar square-to-disk map has.
    CHECK(largestRadius > 0.95f);
    // Centred, so reuse does not drift the sampled region in one direction.
    CHECK(std::abs(meanX / vkm::kVkmNeighbourOffsetCount) < 0.05);
    CHECK(std::abs(meanY / vkm::kVkmNeighbourOffsetCount) < 0.05);

    // Any run of four -- what the pass actually takes -- must spread rather than cluster.
    for (uint32_t base = 0; base < vkm::kVkmNeighbourOffsetCount; ++base)
    {
        float smallestSeparation = 4.0f;
        for (uint32_t a = 0; a < 4; ++a)
        {
            for (uint32_t b = a + 1; b < 4; ++b)
            {
                const uint32_t ia = (base + a) % vkm::kVkmNeighbourOffsetCount;
                const uint32_t ib = (base + b) % vkm::kVkmNeighbourOffsetCount;
                const float dx = offsets[ia * 2 + 0] - offsets[ib * 2 + 0];
                const float dy = offsets[ia * 2 + 1] - offsets[ib * 2 + 1];
                smallestSeparation = std::min(smallestSeparation, std::sqrt(dx * dx + dy * dy));
            }
        }
        if (smallestSeparation <= 0.1f)
        {
            CHECK_MESSAGE(smallestSeparation > 0.1f,
                          "run of four starting at " << base << " clusters within "
                                                     << smallestSeparation);
            break;
        }
    }
}
