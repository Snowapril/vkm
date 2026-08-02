// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/frame_constants.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/shader_cache.h>

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace vkm
{
    enum class VkmPrimitiveTopology : uint8_t
    {
        PointList = 0,
        LineList = 1,
        LineStrip = 2,
        TriangleList = 3,
        TriangleStrip = 4,
    };

    // NOTE: Point is representable in Vulkan (VK_POLYGON_MODE_POINT, requires
    // fillModeNonSolid) only. Metal's MTLTriangleFillMode has no point mode, and
    // WebGPU's WGPUPrimitiveState has no polygon-mode concept at all. Parsing
    // accepts all values; backend pipeline-creation code (out of scope here)
    // must reject/approximate unsupported combinations.
    enum class VkmFillMode : uint8_t
    {
        Solid = 0,
        Wireframe = 1,
        Point = 2,
    };

    // NOTE: FrontAndBack has no Metal (MTLCullMode) or WebGPU (WGPUCullMode)
    // equivalent — both only support None/Front/Back.
    enum class VkmCullMode : uint8_t
    {
        None = 0,
        Front = 1,
        Back = 2,
        FrontAndBack = 3,
    };

    enum class VkmFrontFace : uint8_t
    {
        CounterClockwise = 0,
        Clockwise = 1,
    };

    // VkmCompareOp is defined in renderer_common.h (already included above) --
    // shared between pipeline depth/stencil state and sampler compare-op.

    enum class VkmStencilOp : uint8_t
    {
        Keep = 0,
        Zero = 1,
        Replace = 2,
        IncrementAndClamp = 3,
        DecrementAndClamp = 4,
        Invert = 5,
        IncrementAndWrap = 6,
        DecrementAndWrap = 7,
    };

    enum class VkmBlendFactor : uint8_t
    {
        Zero = 0,
        One = 1,
        SrcColor = 2,
        OneMinusSrcColor = 3,
        DstColor = 4,
        OneMinusDstColor = 5,
        SrcAlpha = 6,
        OneMinusSrcAlpha = 7,
        DstAlpha = 8,
        OneMinusDstAlpha = 9,
        ConstantColor = 10,
        OneMinusConstantColor = 11,
        ConstantAlpha = 12,
        OneMinusConstantAlpha = 13,
        SrcAlphaSaturate = 14,
    };

    enum class VkmBlendOp : uint8_t
    {
        Add = 0,
        Subtract = 1,
        ReverseSubtract = 2,
        Min = 3,
        Max = 4,
    };

    // Two-sided stencil operation state (front-face / back-face).
    struct VkmStencilOpState
    {
        VkmStencilOp failOp = VkmStencilOp::Keep;
        VkmStencilOp passOp = VkmStencilOp::Keep;
        VkmStencilOp depthFailOp = VkmStencilOp::Keep;
        VkmCompareOp compareOp = VkmCompareOp::Always;
        uint32_t compareMask = 0xFFFFFFFFu;
        uint32_t writeMask = 0xFFFFFFFFu;
    };

    struct VkmDepthStencilStateDescriptor
    {
        // Depth
        bool depthTestEnable = true;
        bool depthWriteEnable = true;
        VkmCompareOp depthCompareOp = VkmCompareOp::Less;
        VkmFormat depthStencilFormat = VkmFormat::Undefined; // Undefined => no depth/stencil attachment

        // Stencil
        bool stencilTestEnable = false;
        VkmStencilOpState front{};
        VkmStencilOpState back{};
        uint32_t stencilReference = 0;
    };

    struct VkmRasterizationStateDescriptor
    {
        VkmFillMode fillMode = VkmFillMode::Solid;
        VkmCullMode cullMode = VkmCullMode::Back;
        VkmFrontFace frontFace = VkmFrontFace::CounterClockwise;
    };

    // Per-color-attachment format + blend state (mirrors VkPipelineColorBlendAttachmentState,
    // plus the attachment's own format, since both are 1:1 with a color attachment slot).
    struct VkmColorBlendAttachmentState
    {
        VkmFormat format = VkmFormat::Undefined;

        bool blendEnable = false;
        VkmBlendFactor srcColorBlendFactor = VkmBlendFactor::One;
        VkmBlendFactor dstColorBlendFactor = VkmBlendFactor::Zero;
        VkmBlendOp colorBlendOp = VkmBlendOp::Add;
        VkmBlendFactor srcAlphaBlendFactor = VkmBlendFactor::One;
        VkmBlendFactor dstAlphaBlendFactor = VkmBlendFactor::Zero;
        VkmBlendOp alphaBlendOp = VkmBlendOp::Add;
    };

    // A single shader stage: filepath + entry point + preprocessor definitions.
    // `definitions`: key = macro name, value = macro value ("" => valueless flag,
    // i.e. `-D NAME` with no `=VALUE`; any other string => `-D NAME=VALUE`).
    struct VkmShaderStageDescriptor
    {
        std::string filepath;
        std::string entryPoint = "main";
        std::unordered_map<std::string, std::string> definitions;

        // Stage uses inline ray tracing (an HLSL `RayQuery<>` against a bound acceleration
        // structure). Compile-time consequences only, applied by vkm-compiler: shader model 6.5
        // instead of 6.0, and a Vulkan 1.2 SPIR-V target env so the module is SPIR-V 1.5 rather
        // than 1.0. Compute stages only -- the engine has no ray-tracing pipeline stages, and
        // inline queries are the one form spirv-cross can translate to MSL.
        //
        // Nothing here gates *loading* such a PSO on the device actually supporting ray tracing;
        // there is no acceleration-structure resource or capability flag yet.
        bool requiresRayQuery = false;
    };

    /*
    * @brief One resource kind a PSO can declare at descriptor set 2 or set 3.
    *
    * Sets 2 (per-pass) and 3 (per-draw) are the PSO-declared sets (see common/frame_constants.h).
    * Unlike set 0, their contents are declared by each PSO rather than being engine-global, and
    * unlike set 0 the bindings are fixed rather than runtime-sized arrays. That is what makes them
    * work on WebGPU: WGSL has no runtime-sized arrays, which is the whole reason the bindless
    * texture path is Vulkan/Metal only (VkmDriverCapabilityFlags::TextureUpload), so a shader there
    * can sample nothing at all through set 0. A fixed binding is an ordinary WGSL bind-group entry.
    *
    * The register space in the comments below is the set the declaration belongs to -- space2 for
    * a per-pass binding, space3 for a per-draw one. Nothing else about the kinds differs.
    */
    enum class VkmTableResourceType : uint8_t
    {
        // Texture2D at register(t#, spaceN). Read-only; pairs with a Sampler binding.
        SampledTexture = 0,
        // SamplerState at register(s#, spaceN). Separate from the texture so one sampler can
        // serve several, matching HLSL/SPIR-V rather than Metal's combined view.
        Sampler = 1,
        /*
        * RWStructuredBuffer at register(u#, spaceN).
        *
        * Compute-visible only, for the same reason set 0's two read-write singletons are
        * (bindless_resource_manager.h): WebGPU forbids writable storage in the vertex stage, and
        * a buffer used as writable storage inside a render pass's usage scope may not also be
        * fetched as an indirect argument in that scope.
        */
        StorageBuffer = 2,
        // ConstantBuffer at register(b#, spaceN): constants that outgrow the 128-byte vertex-only
        // push-constant range, and that the fragment stage can actually read.
        UniformBuffer = 3,
    };

    /*
    * @brief One entry in a PSO's set-2 or set-3 declaration.
    *
    * Shader visibility is derived from `type` rather than declared, so a PSO cannot accidentally
    * ask for a combination a backend rejects -- see the StorageBuffer note above.
    */
    struct VkmTableResourceBinding
    {
        uint32_t binding = 0;
        VkmTableResourceType type = VkmTableResourceType::SampledTexture;

        inline bool operator==(const VkmTableResourceBinding& other) const noexcept
        {
            return binding == other.binding && type == other.type;
        }
        inline bool operator!=(const VkmTableResourceBinding& other) const noexcept
        {
            return !(*this == other);
        }
    };

    enum class VkmVertexAttributeBaseType : uint8_t
    {
        Float = 0,
        Int = 1,
        UInt = 2,
    };

    // One decoded token from a compact "[type][count]..." layout string
    // (e.g. "float3float4float2" decodes to three of these).
    struct VkmVertexAttributeDescriptor
    {
        VkmVertexAttributeBaseType baseType = VkmVertexAttributeBaseType::Float;
        uint32_t componentCount = 1; // 1..4
        uint32_t location = 0; // sequential within this part, starting at 0
        uint32_t offset = 0; // byte offset within this part's stride; auto-computed (4 bytes/component)
    };

    struct VkmVertexInputLayoutPart
    {
        std::vector<VkmVertexAttributeDescriptor> attributes;
        uint32_t stride = 0; // auto-computed: sum of componentCount*4 bytes across attributes
    };

    // Both parts independently optional — a pipeline may have neither, either, or both.
    struct VkmVertexInputLayoutDescriptor
    {
        std::optional<VkmVertexInputLayoutPart> perVertex;
        std::optional<VkmVertexInputLayoutPart> perInstance;
    };

    // Field-level partial overrides -- unset optional = inherit from the base descriptor.
    struct VkmRasterizationStateOverlay
    {
        std::optional<VkmFillMode> fillMode;
        std::optional<VkmCullMode> cullMode;
        std::optional<VkmFrontFace> frontFace;
    };

    struct VkmStencilOpStateOverlay
    {
        std::optional<VkmStencilOp> failOp;
        std::optional<VkmStencilOp> passOp;
        std::optional<VkmStencilOp> depthFailOp;
        std::optional<VkmCompareOp> compareOp;
        std::optional<uint32_t> compareMask;
        std::optional<uint32_t> writeMask;
    };

    struct VkmDepthStencilStateOverlay
    {
        std::optional<bool> depthTestEnable;
        std::optional<bool> depthWriteEnable;
        std::optional<VkmCompareOp> depthCompareOp;
        std::optional<VkmFormat> depthStencilFormat;
        std::optional<bool> stencilTestEnable;
        std::optional<VkmStencilOpStateOverlay> front; // whole-struct replace when present
        std::optional<VkmStencilOpStateOverlay> back;
        std::optional<uint32_t> stencilReference;
    };

    // One named variant overlay parsed from a pipeline JSON node's "options" object.
    // Applying this to a copy of the owning (base) VkmPipelineStateDescriptor and merging
    // shader-stage definitions produces one fully-resolved variant -- see
    // expandPipelineStateOptions() in pipeline_state_parser.h.
    struct VkmPipelineStateOptionOverlay
    {
        std::optional<VkmPrimitiveTopology> primitiveTopology;
        std::optional<VkmRasterizationStateOverlay> rasterizationState;
        std::optional<VkmDepthStencilStateOverlay> depthStencilState;
        std::optional<std::vector<VkmColorBlendAttachmentState>> colorAttachments; // whole-array replace
        std::optional<VkmVertexInputLayoutDescriptor> vertexInputLayout;           // whole-struct replace

        std::unordered_map<std::string, std::string> definitions;         // merged into every present stage
        std::unordered_map<std::string, std::string> vertexDefinitions;   // merged after `definitions`, vertex only
        std::unordered_map<std::string, std::string> fragmentDefinitions;
        std::unordered_map<std::string, std::string> computeDefinitions;

        // Optional JSON "backends" allowlist: when non-empty, this option variant only
        // exists on the listed backends -- expandPipelineStateOptions() drops it for
        // other backends, and vkm-compiler generates no caches for it (e.g. a wireframe
        // variant on WebGPU, which has no polygon-mode concept). Empty = all backends.
        std::vector<VkmShaderCacheBackend> backends;
    };

    // Top-level, backend-agnostic pipeline state descriptor. Fully populated by
    // the parser (see pipeline_state_parser.h) — every field has a documented
    // default that applies when the corresponding JSON field is omitted.
    struct VkmPipelineStateDescriptor
    {
        std::string name; // optional, for debugging/logging only

        VkmPrimitiveTopology primitiveTopology = VkmPrimitiveTopology::TriangleList;
        VkmRasterizationStateDescriptor rasterizationState{};
        VkmDepthStencilStateDescriptor depthStencilState{};

        // One entry per color attachment, in the order given in the JSON
        // `color_attachments` array. May be empty (e.g. depth-only pipelines).
        std::vector<VkmColorBlendAttachmentState> colorAttachments;

        // Vertex/instance buffer layout for this pipeline. Both perVertex and
        // perInstance are independently optional (e.g. a fullscreen-quad shader
        // with no vertex buffer at all would leave both unset).
        VkmVertexInputLayoutDescriptor vertexInputLayout;

        /*
        * @brief This pipeline's descriptor set 2 (per-pass) declaration, in the order given in the
        * JSON `per_pass_resources` array. Empty for a pipeline that reaches only sets 0 and 1,
        * which is every pipeline that predates set 2.
        *
        * Sets 0 and 1 are engine-global and identical for every pipeline, which is what let bind
        * sites stay free of per-PSO knowledge. Sets 2 and 3 are the genuinely per-PSO ones, so a
        * pipeline declaring either is no longer layout-compatible with one that does not, and
        * whatever binds its resources has to know this list.
        */
        std::vector<VkmTableResourceBinding> perPassResources;

        /*
        * @brief This pipeline's descriptor set 3 (per-draw) declaration, from the JSON
        * `per_draw_resources` array. Same shape and same rules as perPassResources; only the set
        * index and the intended bind frequency differ.
        *
        * A PSO may declare this *without* declaring set 2 -- a G-buffer pass wants per-material
        * textures and no per-pass resources at all. Backends must therefore keep set 3 at set index
        * 3 rather than at "the next free slot", which means inserting an empty placeholder layout
        * for set 2 (see VkmPipelineStateVulkan::createInner and its WebGPU counterpart).
        */
        std::vector<VkmTableResourceBinding> perDrawResources;

        // The declaration for one set kind, so shared code selects instead of branching.
        inline const std::vector<VkmTableResourceBinding>& resourcesFor(VkmResourceSetKind kind) const
        {
            return (kind == VkmResourceSetKind::PerPass) ? perPassResources : perDrawResources;
        }

        // vertexShader is the only field the parser treats as required.
        std::optional<VkmShaderStageDescriptor> vertexShader;
        std::optional<VkmShaderStageDescriptor> fragmentShader;
        std::optional<VkmShaderStageDescriptor> computeShader;

        // "" for a descriptor with no "options" node, or for the base/unsuffixed descriptor.
        // Set to the option's name (e.g. "wireframe") on descriptors produced by
        // expandPipelineStateOptions(). Shader-cache filenames incorporate this (see
        // shader_cache_util.h) so variants sharing one HLSL source/stage/backend don't collide.
        std::string optionName;

        // Parsed straight from the JSON "options" object; empty if absent. Only ever populated
        // on the descriptor returned directly by parsePipelineStateFromFile/FromString -- always
        // empty on the per-variant descriptors returned by expandPipelineStateOptions(), since
        // expansion fully resolves and clears this.
        std::unordered_map<std::string, VkmPipelineStateOptionOverlay> options;
    };
} // namespace vkm
