#include <doctest/doctest.h>

#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/scene/scene.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

/*
* These records are mirrored by hand in the scene shaders (model_viewer.hlsl and scene_cull.hlsl):
* nothing in the build fails if the two sides drift, so the offsets are asserted here instead.
* They run on every backend and need no GPU.
*/

TEST_CASE("VkmObjectData - matches the shader-side ObjectData layout") {
    CHECK(sizeof(vkm::VkmObjectData) == 176);

    CHECK(offsetof(vkm::VkmObjectData, _worldTransform) == 0);
    CHECK(offsetof(vkm::VkmObjectData, _normalTransform) == 64);
    CHECK(offsetof(vkm::VkmObjectData, _vertexPoolSlot) == 128);
    CHECK(offsetof(vkm::VkmObjectData, _indexPoolSlot) == 132);
    CHECK(offsetof(vkm::VkmObjectData, _vertexWordOffset) == 136);
    CHECK(offsetof(vkm::VkmObjectData, _materialIndex) == 140);
    CHECK(offsetof(vkm::VkmObjectData, _indexOffset) == 144);
    CHECK(offsetof(vkm::VkmObjectData, _indexCount) == 148);
    // The float4 must land on a 16-byte boundary, which is what _pad0 exists for.
    CHECK(offsetof(vkm::VkmObjectData, _boundsCenterRadius) == 160);
    CHECK(offsetof(vkm::VkmObjectData, _boundsCenterRadius) % 16 == 0);
}

/*
* No camera member: the view-projection comes from descriptor set 1 (VkmFrameConstants), so this
* record only carries the per-frame data set 1 does not have.
*/
TEST_CASE("VkmFrameData - matches the shader-side FrameData layout") {
    CHECK(sizeof(vkm::VkmFrameData) == 128);

    CHECK(offsetof(vkm::VkmFrameData, _frustumPlanes) == 0);
    CHECK(offsetof(vkm::VkmFrameData, _lightDirection) == 96);
    CHECK(offsetof(vkm::VkmFrameData, _materialPoolSlot) == 112);
    CHECK(offsetof(vkm::VkmFrameData, _debugMode) == 116);
}

/*
* The shaders read the material pool out of the untyped u32 Buffer array, so they index it with a
* hardcoded stride in words -- VKM_MATERIAL_WORD_STRIDE in vkm_material.hlsli, which is the only
* place that stride now lives. 64 bytes == 16 words, with baseColorFactor first and the texture
* slots last.
*/
TEST_CASE("VkmMaterialData - is 16 u32 words with the base color first and the texture slots last") {
    CHECK(sizeof(vkm::VkmMaterialData) == 64);
    CHECK(sizeof(vkm::VkmMaterialData) % 4 == 0);
    CHECK(sizeof(vkm::VkmMaterialData) / 4 == 16);

    CHECK(offsetof(vkm::VkmMaterialData, _baseColorFactor) == 0);
    CHECK(offsetof(vkm::VkmMaterialData, _emissive) == 16);
    CHECK(offsetof(vkm::VkmMaterialData, _metallicRoughness) == 32);
    CHECK(offsetof(vkm::VkmMaterialData, _textureSlots) == 48);

    // "No texture for this channel" must be distinguishable from slot 0, or every untextured
    // material samples whatever happens to live there.
    const vkm::VkmMaterialData defaults;
    CHECK(defaults._textureSlots.x == vkm::INVALID_VALUE32);
    CHECK(defaults._textureSlots.y == vkm::INVALID_VALUE32);
    CHECK(defaults._textureSlots.z == vkm::INVALID_VALUE32);
    CHECK(defaults._textureSlots.w == vkm::INVALID_VALUE32);
}

/*
* The culling shader writes these records and the draw fetches them as native indirect arguments,
* so the layout has to match VkDrawIndirectCommand / MTLDrawPrimitivesIndirectArguments / WebGPU's
* non-indexed indirect layout exactly. firstInstance carries the object index.
*/
TEST_CASE("VkmDrawIndirectArguments - matches the native indirect argument layout") {
    CHECK(sizeof(vkm::VkmDrawIndirectArguments) == 16);

    CHECK(offsetof(vkm::VkmDrawIndirectArguments, _vertexCount) == 0);
    CHECK(offsetof(vkm::VkmDrawIndirectArguments, _instanceCount) == 4);
    CHECK(offsetof(vkm::VkmDrawIndirectArguments, _firstVertex) == 8);
    CHECK(offsetof(vkm::VkmDrawIndirectArguments, _firstInstance) == 12);

    // An all-zero record must be a no-op draw: that is what culled slots become on the backends
    // with no GPU-side draw count.
    const vkm::VkmDrawIndirectArguments zeroed{};
    CHECK(zeroed._vertexCount == 0);
    CHECK(zeroed._instanceCount == 0);
}

/*
* Must match VkDrawIndexedIndirectCommand / MTLDrawIndexedPrimitivesIndirectArguments / WebGPU's
* indexed indirect layout. Nothing draws with it yet (there is no bound index buffer), so these
* assertions are the only thing keeping the record honest until something does.
*/
TEST_CASE("VkmDrawIndexedIndirectArguments - matches the native indexed indirect layout") {
    CHECK(sizeof(vkm::VkmDrawIndexedIndirectArguments) == 20);

    CHECK(offsetof(vkm::VkmDrawIndexedIndirectArguments, _indexCount) == 0);
    CHECK(offsetof(vkm::VkmDrawIndexedIndirectArguments, _instanceCount) == 4);
    CHECK(offsetof(vkm::VkmDrawIndexedIndirectArguments, _firstIndex) == 8);
    CHECK(offsetof(vkm::VkmDrawIndexedIndirectArguments, _vertexOffset) == 12);
    CHECK(offsetof(vkm::VkmDrawIndexedIndirectArguments, _firstInstance) == 16);

    // _vertexOffset is signed on all three backends -- the one field a mirror written as five
    // uint32s gets wrong, and a negative value would silently become a huge positive one.
    CHECK(std::is_signed_v<decltype(vkm::VkmDrawIndexedIndirectArguments::_vertexOffset)>);
    CHECK(std::is_unsigned_v<decltype(vkm::VkmDrawIndexedIndirectArguments::_indexCount)>);
}

// The stride the backends feed to their native indirect draws comes from here, so it must not be
// able to drift from the records it describes.
TEST_CASE("vkmGetIndirectArgumentStride - agrees with the record it names") {
    CHECK(vkm::vkmGetIndirectArgumentStride(vkm::VkmIndirectArgumentLayout::NonIndexed) ==
          sizeof(vkm::VkmDrawIndirectArguments));
    CHECK(vkm::vkmGetIndirectArgumentStride(vkm::VkmIndirectArgumentLayout::Indexed) ==
          sizeof(vkm::VkmDrawIndexedIndirectArguments));

    // Both strides must stay whole u32 words: the scene addresses its argument regions in words.
    CHECK(vkm::vkmGetIndirectArgumentStride(vkm::VkmIndirectArgumentLayout::NonIndexed) % sizeof(uint32_t) == 0);
    CHECK(vkm::vkmGetIndirectArgumentStride(vkm::VkmIndirectArgumentLayout::Indexed) % sizeof(uint32_t) == 0);
}
