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
//   WebGPU          descriptor set 3 (per-draw), one table per material, bound before each draw.
//                   WGSL has no array-of-handle type (Tint rejects `Texture2D t[4096]` outright)
//                   and maxSampledTexturesPerShaderStage defaults to 16, so there is no bindless
//                   array to index: VkmScene uploads the pixels there but registers no slot, and
//                   it splits draw batches per material so a per-draw table has somewhere to be
//                   bound. The material record's slot fields are unused on this branch.
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
    }                                                                                               \
    float3 vkmSampleEmissive(VkmMaterial material, float2 uv)                                       \
    {                                                                                               \
        return material.emissiveFactor * vkmSampleMaterialTexture(material.emissiveSlot, uv).rgb;   \
    }

#if defined(VKM_BACKEND_WEBGPU)

/*
* Set 3 holds this draw's material directly, so there is nothing to index: the two textures are
* bindings 0 and 1, the sampler is binding 2, and every draw of this batch shares them. A channel
* the material has no texture for still needs a binding -- an unbound entry in a declared group is
* a WebGPU validation error, not a silently absent one -- so the table binds a 1x1 white
* placeholder there, which samples to 1 and leaves the factor exactly, matching the other branch.
*
* Sample, not SampleLevel: material textures carry a full mip chain, so the implicit LOD is the
* whole point -- pinning it to 0 would upload the chain and then never read it. WGSL restricts an
* implicit-LOD sample to uniform control flow, which both callers satisfy: neither the G-buffer nor
* the probe-capture pixel shader has an early return or a branch around this. (The fullscreen
* passes that *do* branch -- deferred lighting, gi_composite -- keep SampleLevel for that reason,
* and their inputs are single-mip anyway.)
*
* The declaration order must match the PSO's per_draw_resources array and the table the runtime
* builds; VkmGiMaterialTables is the one that fills it.
*/
#define VKM_MATERIAL_DECLARE()                                                                      \
    [[vk::binding(0, 3)]] Texture2D    g_VkmMaterialBaseColor         : register(t0, space3);       \
    [[vk::binding(1, 3)]] Texture2D    g_VkmMaterialMetallicRoughness : register(t1, space3);       \
    [[vk::binding(2, 3)]] SamplerState g_VkmMaterialSampler           : register(s0, space3);       \
    [[vk::binding(3, 3)]] Texture2D    g_VkmMaterialEmissive          : register(t2, space3);       \
    VKM_MATERIAL_LOADER()                                                                           \
    float4 vkmSampleBaseColor(VkmMaterial material, float2 uv)                                      \
    {                                                                                               \
        return material.baseColorFactor *                                                           \
               g_VkmMaterialBaseColor.Sample(g_VkmMaterialSampler, uv);                             \
    }                                                                                               \
    float2 vkmSampleMetallicRoughness(VkmMaterial material, float2 uv)                              \
    {                                                                                               \
        const float4 s = g_VkmMaterialMetallicRoughness.Sample(g_VkmMaterialSampler, uv);           \
        return float2(material.metallic * s.b, material.roughness * s.g);                           \
    }                                                                                               \
    float3 vkmSampleEmissive(VkmMaterial material, float2 uv)                                       \
    {                                                                                               \
        return material.emissiveFactor * g_VkmMaterialEmissive.Sample(g_VkmMaterialSampler, uv).rgb; \
    }

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
