// Copyright (c) 2026 Snowapril
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
#define VKM_MATERIAL_WORD_STRIDE 20

// A slot of this value means the material has no texture for that channel, so its factor stands
// alone. Mirrors INVALID_VALUE32.
#define VKM_MATERIAL_NO_TEXTURE 0xFFFFFFFFu

// Levels a feedback entry can report, and the "nothing sampled this" value the buffer is cleared
// to. Mirrors kVkmTextureFeedbackMaxLevel / kVkmTextureFeedbackUnused in texture_streamer.h.
#define VKM_TEXTURE_FEEDBACK_MAX_LEVEL 15u
#define VKM_TEXTURE_FEEDBACK_UNUSED    0xFFFFFFFFu

/*
* Only pixels on this stride vote, which is what makes the feedback write cheap enough to leave on
* unconditionally rather than gate behind a permutation.
*
* Every pixel voting would serialise a great many atomics onto the handful of slots a large surface
* covers, and buy nothing: the answer wanted is the *minimum* level over a surface, and a surface
* thin enough to fall entirely between votes is one whose texture detail cannot matter. 4 gives one
* vote per 16 pixels.
*/
#define VKM_TEXTURE_FEEDBACK_PIXEL_STRIDE 4u

struct VkmMaterial
{
    float4 baseColorFactor;
    float3 emissiveFactor;
    // glTF alphaMode MASK's cutoff; 0 means the material is drawn without an alpha test.
    float  alphaCutoff;
    float  metallic;
    float  roughness;
    uint   baseColorSlot;
    uint   metallicRoughnessSlot;
    uint   normalSlot;
    uint   emissiveSlot;
    /*
    * Finest mip level each channel's texture actually has memory for, in that texture's own level
    * numbering, one component per slot in the same order. Zero unless the texture is sparse: a
    * rebuilt texture is physically only as large as what it holds, so its level 0 is always
    * backed, while a sparse one keeps its full extent and the levels streamed off the front read
    * as blank. Passed to the sample as its min-LOD clamp, which is what keeps them unread.
    */
    float4 minLod;
};

/*
* The base-colour texture's streamed mip range: words 10 and 11, which vkmLoadMaterial does not
* read. x is the level the resident texture starts at, y how many levels its full chain has.
*
* Separate from VkmMaterial, and loaded by its own function, because only the streaming debug view
* wants it: folding two more word loads into vkmLoadMaterial would charge every shader that
* evaluates a material -- including the ray-tracing kernels, which do it per hit -- for a
* visualisation none of them draws.
*/
#define VKM_MATERIAL_STREAMING_LOADER()                                                             \
    float2 vkmLoadMaterialStreamingMip(uint materialPoolSlot, uint materialIndex)                   \
    {                                                                                               \
        const uint base = materialIndex * VKM_MATERIAL_WORD_STRIDE;                                 \
        return float2(asfloat(VKM_LOAD_VERTEX(materialPoolSlot, base + 10)),                        \
                      asfloat(VKM_LOAD_VERTEX(materialPoolSlot, base + 11)));                       \
    }

/*
* Green where the whole chain is resident through cool blue to red at the coarsest level, so a
* surface that has streamed out reads as hot. A material with no streamed texture reports a chain
* of one level and comes back green, which is what "nothing to stream" should look like.
*/
float3 vkmStreamingMipHeatColor(float2 streamingMip)
{
    const float lastLevel = max(streamingMip.y - 1.0, 1.0);
    const float t = saturate(streamingMip.x / lastLevel);
    return float3(t, 1.0 - t, 0.5 * saturate(1.0 - abs(t * 2.0 - 1.0)));
}

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
        material.alphaCutoff = asfloat(VKM_LOAD_VERTEX(materialPoolSlot, base + 7));         \
        material.metallic  = asfloat(VKM_LOAD_VERTEX(materialPoolSlot, base + 8));                  \
        material.roughness = asfloat(VKM_LOAD_VERTEX(materialPoolSlot, base + 9));                  \
        material.baseColorSlot         = VKM_LOAD_VERTEX(materialPoolSlot, base + 12);              \
        material.metallicRoughnessSlot = VKM_LOAD_VERTEX(materialPoolSlot, base + 13);              \
        material.normalSlot            = VKM_LOAD_VERTEX(materialPoolSlot, base + 14);              \
        material.emissiveSlot          = VKM_LOAD_VERTEX(materialPoolSlot, base + 15);              \
        material.minLod = float4(VKM_LOAD_VERTEX(materialPoolSlot, base + 16),                      \
                                 VKM_LOAD_VERTEX(materialPoolSlot, base + 17),                      \
                                 VKM_LOAD_VERTEX(materialPoolSlot, base + 18),                      \
                                 VKM_LOAD_VERTEX(materialPoolSlot, base + 19));                     \
        return material;                                                                            \
    }

// glTF multiplies factor by texture, so an absent texture sampling to 1 leaves the factor exactly.
// glTF also packs occlusion/roughness/metallic into the image's r/g/b: roughness is g, metallic b.
#define VKM_MATERIAL_SAMPLERS()                                                                     \
    float4 vkmSampleBaseColor(VkmMaterial material, float2 uv)                                      \
    {                                                                                               \
        return material.baseColorFactor *                                                           \
               vkmSampleMaterialTexture(material.baseColorSlot, uv, material.minLod.x);              \
    }                                                                                               \
    float2 vkmSampleMetallicRoughness(VkmMaterial material, float2 uv)                              \
    {                                                                                               \
        const float4 s =                                                                            \
            vkmSampleMaterialTexture(material.metallicRoughnessSlot, uv, material.minLod.y);        \
        return float2(material.metallic * s.b, material.roughness * s.g);                           \
    }                                                                                               \
    float3 vkmSampleEmissive(VkmMaterial material, float2 uv)                                       \
    {                                                                                               \
        return material.emissiveFactor *                                                            \
               vkmSampleMaterialTexture(material.emissiveSlot, uv, material.minLod.w).rgb;           \
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
    VKM_MATERIAL_STREAMING_LOADER()                                                                 \
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
* The min-LOD clamp is compiled in only where a texture can actually be sparse.
*
* A clamped Sample lowers to SPIR-V's MinLod image operand, whose capability requires
* VkPhysicalDeviceFeatures::shaderResourceMinLod -- and a module declaring a capability the device
* lacks fails vkCreateShaderModule outright, not just where the clamp is used. Real GPUs offer it;
* lavapipe, which is what the Vulkan CI runs on, does not. So an unconditional clamp costs every
* device that cannot do it the entire G-buffer pass, in exchange for a value that is always zero
* wherever nothing is sparse.
*
* Sparse residency is Metal-only today, so Vulkan and WebGPU always clamp to zero and the operand is
* dead there. Whoever implements vkQueueBindSparse owns this decision again, and will have a device
* in hand to answer it with.
*/
#if defined(VKM_BACKEND_METAL)
#define VKM_SAMPLE_MATERIAL_TEXTURE_CLAMPED(slot, uv, minLod)                                       \
    return g_VkmMaterialTextures[NonUniformResourceIndex(slot)]                                     \
        .Sample(g_VkmMaterialSampler, uv, int2(0, 0), minLod);
#else
#define VKM_SAMPLE_MATERIAL_TEXTURE_CLAMPED(slot, uv, minLod)                                       \
    return g_VkmMaterialTextures[NonUniformResourceIndex(slot)].Sample(g_VkmMaterialSampler, uv);
#endif

/*
* NonUniformResourceIndex is required, not decorative: one indirect draw covers many materials, so
* the slot is divergent across a wave, and this is exactly the case Vulkan's descriptor-indexing
* non-uniform rule exists for. Without it a wave whose lanes hit different materials may sample one
* lane's texture for all of them.
*/
#define VKM_MATERIAL_DECLARE()                                                                      \
    VKM_BINDLESS_TEXTURE_2D_ARRAY(g_VkmMaterialTextures);                                           \
    VKM_BINDLESS_SAMPLER(g_VkmMaterialSampler);                                                     \
    float4 vkmSampleMaterialTexture(uint slot, float2 uv, float minLod)                             \
    {                                                                                               \
        if (slot == VKM_MATERIAL_NO_TEXTURE)                                                        \
        {                                                                                           \
            return float4(1.0, 1.0, 1.0, 1.0);                                                      \
        }                                                                                           \
        VKM_SAMPLE_MATERIAL_TEXTURE_CLAMPED(slot, uv, minLod)                                       \
    }                                                                                               \
    VKM_MATERIAL_LOADER()                                                                           \
    VKM_MATERIAL_STREAMING_LOADER()                                                                 \
    VKM_MATERIAL_SAMPLERS()

/*
* Reports the mip level this pixel wanted, for texture streaming to read back.
*
* Deliberately NOT part of VKM_MATERIAL_DECLARE(): eight shaders expand that macro, and only the
* pass that decides what is on screen should vote. The probe capture in particular must not --
* probes look in every direction, so letting them vote would drag every texture to full resolution
* and defeat streaming entirely.
*
* The level is relative to the texture actually sampled, which for a streamed texture is already
* reduced. The CPU adds that slot's resident base mip to recover a chain-absolute level; doing it
* that way keeps the reading correct even while a rebuild is in flight, and needs nothing extra in
* the material record.
*
* InterlockedMin because the finest level any pixel needed is the one that matters -- a plain store
* would let an arbitrary distant pixel win and leave the surface under-resolved.
*
* Place after VKM_MATERIAL_DECLARE() and VKM_BINDLESS_TEXTURE_FEEDBACK(); it reads both.
*/
#define VKM_MATERIAL_FEEDBACK_RECORDER(feedbackBuffer)                                              \
    void vkmRecordTextureFeedback(uint slot, float2 uv)                                             \
    {                                                                                               \
        if (slot == VKM_MATERIAL_NO_TEXTURE)                                                        \
        {                                                                                           \
            return;                                                                                 \
        }                                                                                           \
        const float lod = g_VkmMaterialTextures[NonUniformResourceIndex(slot)]                      \
                              .CalculateLevelOfDetail(g_VkmMaterialSampler, uv);                    \
        uint previous;                                                                              \
        InterlockedMin(feedbackBuffer[slot],                                                        \
                       (uint)clamp(round(lod), 0.0, (float)VKM_TEXTURE_FEEDBACK_MAX_LEVEL),         \
                       previous);                                                                   \
    }

#endif

#endif // VKM_MATERIAL_HLSLI
