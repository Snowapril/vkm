// Copyright (c) 2025 Snowapril
//
// The reservoir's packing is an ABI in the strongest sense this engine has: a screen's worth is
// millions of records, every ReSTIR pass reads and writes them, and the shader side mirrors these
// numbers by hand. Nothing in the build fails if the two drift, so they are asserted here --
// the same reason TestObjectDataLayout exists.

#include <doctest/doctest.h>

#include <vkm/renderer/restir.h>

#include <cstddef>

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
    CHECK(sizeof(vkm::VkmRestirConstants) == 40);
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
