// Copyright (c) 2025 Snowapril

#pragma once

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/renderer_common.h>

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

    enum class VkmCompareOp : uint8_t
    {
        Never = 0,
        Less = 1,
        Equal = 2,
        LessOrEqual = 3,
        Greater = 4,
        NotEqual = 5,
        GreaterOrEqual = 6,
        Always = 7,
    };

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

        // vertexShader is the only field the parser treats as required.
        std::optional<VkmShaderStageDescriptor> vertexShader;
        std::optional<VkmShaderStageDescriptor> fragmentShader;
        std::optional<VkmShaderStageDescriptor> computeShader;
    };
} // namespace vkm
