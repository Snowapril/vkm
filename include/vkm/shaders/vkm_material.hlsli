// Copyright (c) 2025 Snowapril
//
// The one place that knows how a material's textures reach a shader, so no drawing shader has to.
// Same role vkm_bindless.hlsli plays for the resource arrays: a shader writes VKM_MATERIAL_DECLARE()
// once, then calls vkmLoadMaterial() and vkmSample*(), and contains no VKM_BACKEND_* test.
//
// The split it hides:
//
//   Vulkan / Metal  a bindless Texture2D array at set 0 binding 0, indexed by a slot the material
//                   record carries. One indirect draw covers many materials, and a compute or
//                   ray-tracing shader can evaluate a material at an arbitrary hit point -- which
//                   is why this, and not a per-draw table, is the native path.
//
//   WebGPU          neither. WGSL has no array-of-handle type (Tint rejects `Texture2D t[4096]`
//                   outright) and maxSampledTexturesPerShaderStage defaults to 16, so
//                   registerTexture is a hard error there. The sampling functions return 1 and the
//                   material's factor passes through unchanged -- which is honest, and matches what
//                   VkmScene uploads on that backend: nothing. Descriptor set 3 carries a
//                   per-material table there instead; when that lands it replaces this branch and
//                   no drawing shader changes.
//
// Everything is emitted by one macro because HLSL resolves names top-down and the arrays these
// functions read are themselves declared by macros in the shader -- a plain function here would
// reference identifiers that do not exist yet. VKM_BINDLESS_VERTEX_PULLING has the same shape for
// the same reason. Place VKM_MATERIAL_DECLARE() after it.
//
// Only ONE texture type may be declared at set 0 binding 0 per shader (see
// bindless_resource_manager.h): Texture2D here, TextureCube in the skybox. A shader that includes
// this header must therefore not also declare VKM_BINDLESS_TEXTURE_CUBE_ARRAY -- spirv-cross would
// alias the two and emit structurally wrong MSL.

#ifndef VKM_MATERIAL_HLSLI
#define VKM_MATERIAL_HLSLI

#include "vkm_bindless.hlsli"

// Mirrors vkm::VkmMaterialData (include/vkm/renderer/scene/scene.h), 64 bytes = 16 words.
#define VKM_MATERIAL_WORD_STRIDE 16

// A slot of this value means the material has no texture for that channel, so its factor stands
// alone. Mirrors INVALID_VALUE32.
#define VKM_MATERIAL_NO_TEXTURE 0xFFFFFFFFu

struct VkmMaterial
{
    float4 baseColorFactor;
    float3 emissiveFactor;
    float  metallic;
    float  roughness;
    uint   baseColorSlot;
    uint   metallicRoughnessSlot;
    uint   normalSlot;
    uint   emissiveSlot;
};

// Reads one material record out of the pool, which is an untyped u32 word array (it shares the
// bindless Buffer array with the geometry pools) -- so this is a word unpack, the same shape
// VKM_LOAD_VERTEX has. Identical on every backend.
#define VKM_MATERIAL_LOADER()                                                                       \
    VkmMaterial vkmLoadMaterial(uint materialPoolSlot, uint materialIndex)                          \
    {                                                                                               \
        const uint base = materialIndex * VKM_MATERIAL_WORD_STRIDE;                                 \
        VkmMaterial material;                                                                       \
        material.baseColorFactor = float4(asfloat(VKM_LOAD_VERTEX(materialPoolSlot, base + 0)),     \
                                          asfloat(VKM_LOAD_VERTEX(materialPoolSlot, base + 1)),     \
                                          asfloat(VKM_LOAD_VERTEX(materialPoolSlot, base + 2)),     \
                                          asfloat(VKM_LOAD_VERTEX(materialPoolSlot, base + 3)));    \
        material.emissiveFactor = float3(asfloat(VKM_LOAD_VERTEX(materialPoolSlot, base + 4)),      \
                                         asfloat(VKM_LOAD_VERTEX(materialPoolSlot, base + 5)),      \
                                         asfloat(VKM_LOAD_VERTEX(materialPoolSlot, base + 6)));     \
        material.metallic  = asfloat(VKM_LOAD_VERTEX(materialPoolSlot, base + 8));                  \
        material.roughness = asfloat(VKM_LOAD_VERTEX(materialPoolSlot, base + 9));                  \
        material.baseColorSlot         = VKM_LOAD_VERTEX(materialPoolSlot, base + 12);              \
        material.metallicRoughnessSlot = VKM_LOAD_VERTEX(materialPoolSlot, base + 13);              \
        material.normalSlot            = VKM_LOAD_VERTEX(materialPoolSlot, base + 14);              \
        material.emissiveSlot          = VKM_LOAD_VERTEX(materialPoolSlot, base + 15);              \
        return material;                                                                            \
    }

// glTF multiplies factor by texture, so an absent texture sampling to 1 leaves the factor exactly.
// glTF also packs occlusion/roughness/metallic into the image's r/g/b: roughness is g, metallic b.
#define VKM_MATERIAL_SAMPLERS()                                                                     \
    float4 vkmSampleBaseColor(VkmMaterial material, float2 uv)                                      \
    {                                                                                               \
        return material.baseColorFactor * vkmSampleMaterialTexture(material.baseColorSlot, uv);     \
    }                                                                                               \
    float2 vkmSampleMetallicRoughness(VkmMaterial material, float2 uv)                              \
    {                                                                                               \
        const float4 s = vkmSampleMaterialTexture(material.metallicRoughnessSlot, uv);              \
        return float2(material.metallic * s.b, material.roughness * s.g);                           \
    }

#if defined(VKM_BACKEND_WEBGPU)

// No texture array exists on this backend, so every channel is its factor alone.
#define VKM_MATERIAL_DECLARE()                                                                      \
    float4 vkmSampleMaterialTexture(uint slot, float2 uv)                                           \
    {                                                                                               \
        return float4(1.0, 1.0, 1.0, 1.0);                                                          \
    }                                                                                               \
    VKM_MATERIAL_LOADER()                                                                           \
    VKM_MATERIAL_SAMPLERS()

#else

/*
* NonUniformResourceIndex is required, not decorative: one indirect draw covers many materials, so
* the slot is divergent across a wave, and this is exactly the case Vulkan's descriptor-indexing
* non-uniform rule exists for. Without it a wave whose lanes hit different materials may sample one
* lane's texture for all of them.
*/
#define VKM_MATERIAL_DECLARE()                                                                      \
    VKM_BINDLESS_TEXTURE_2D_ARRAY(g_VkmMaterialTextures);                                           \
    VKM_BINDLESS_SAMPLER(g_VkmMaterialSampler);                                                     \
    float4 vkmSampleMaterialTexture(uint slot, float2 uv)                                           \
    {                                                                                               \
        if (slot == VKM_MATERIAL_NO_TEXTURE)                                                        \
        {                                                                                           \
            return float4(1.0, 1.0, 1.0, 1.0);                                                      \
        }                                                                                           \
        return g_VkmMaterialTextures[NonUniformResourceIndex(slot)].Sample(g_VkmMaterialSampler, uv); \
    }                                                                                               \
    VKM_MATERIAL_LOADER()                                                                           \
    VKM_MATERIAL_SAMPLERS()

#endif

#endif // VKM_MATERIAL_HLSLI
