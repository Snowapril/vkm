// Copyright (c) 2025 Snowapril

#include <vkm/renderer/backend/common/pipeline_state_parser.h>

#include <vkm/base/common.h>
#include <vkm/renderer/backend/common/enum_string_util.h>

#include <nlohmann/json.hpp>

#include <cctype>
#include <fstream>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>

namespace vkm
{
    namespace
    {
        using Json = nlohmann::json;

        const std::unordered_map<std::string_view, VkmFormat> kFormatTable = {
            { "undefined", VkmFormat::Undefined },
            { "r8g8b8a8_unorm", VkmFormat::R8G8B8A8_UNORM },
            { "r8g8b8a8_srgb", VkmFormat::R8G8B8A8_SRGB },
            { "r8g8b8a8_uint", VkmFormat::R8G8B8A8_UINT },
            { "r8g8b8a8_snorm", VkmFormat::R8G8B8A8_SNORM },
            { "r8g8b8a8_sint", VkmFormat::R8G8B8A8_SINT },
            { "r16g16b16a16_unorm", VkmFormat::R16G16B16A16_UNORM },
            { "r16g16b16a16_sfloat", VkmFormat::R16G16B16A16_SFLOAT },
            { "r32g32b32a32_sfloat", VkmFormat::R32G32B32A32_SFLOAT },
            { "d32_sfloat", VkmFormat::D32_SFLOAT },
            { "d24_unorm_s8_uint", VkmFormat::D24_UNORM_S8_UINT },
            { "d32_sfloat_s8_uint", VkmFormat::D32_SFLOAT_S8_UINT },
            { "bgra8_unorm", VkmFormat::BGRA8_UNORM },
            { "bgra8_srgb", VkmFormat::BGRA8_SRGB },
        };

        const std::unordered_map<std::string_view, VkmPrimitiveTopology> kPrimitiveTopologyTable = {
            { "point_list", VkmPrimitiveTopology::PointList },
            { "line_list", VkmPrimitiveTopology::LineList },
            { "line_strip", VkmPrimitiveTopology::LineStrip },
            { "triangle_list", VkmPrimitiveTopology::TriangleList },
            { "triangle_strip", VkmPrimitiveTopology::TriangleStrip },
        };

        const std::unordered_map<std::string_view, VkmFillMode> kFillModeTable = {
            { "solid", VkmFillMode::Solid },
            { "wireframe", VkmFillMode::Wireframe },
            { "point", VkmFillMode::Point },
        };

        const std::unordered_map<std::string_view, VkmCullMode> kCullModeTable = {
            { "none", VkmCullMode::None },
            { "front", VkmCullMode::Front },
            { "back", VkmCullMode::Back },
            { "front_and_back", VkmCullMode::FrontAndBack },
        };

        const std::unordered_map<std::string_view, VkmFrontFace> kFrontFaceTable = {
            { "counter_clockwise", VkmFrontFace::CounterClockwise },
            { "clockwise", VkmFrontFace::Clockwise },
        };

        const std::unordered_map<std::string_view, VkmCompareOp> kCompareOpTable = {
            { "never", VkmCompareOp::Never },
            { "less", VkmCompareOp::Less },
            { "equal", VkmCompareOp::Equal },
            { "less_or_equal", VkmCompareOp::LessOrEqual },
            { "greater", VkmCompareOp::Greater },
            { "not_equal", VkmCompareOp::NotEqual },
            { "greater_or_equal", VkmCompareOp::GreaterOrEqual },
            { "always", VkmCompareOp::Always },
        };

        const std::unordered_map<std::string_view, VkmStencilOp> kStencilOpTable = {
            { "keep", VkmStencilOp::Keep },
            { "zero", VkmStencilOp::Zero },
            { "replace", VkmStencilOp::Replace },
            { "increment_and_clamp", VkmStencilOp::IncrementAndClamp },
            { "decrement_and_clamp", VkmStencilOp::DecrementAndClamp },
            { "invert", VkmStencilOp::Invert },
            { "increment_and_wrap", VkmStencilOp::IncrementAndWrap },
            { "decrement_and_wrap", VkmStencilOp::DecrementAndWrap },
        };

        const std::unordered_map<std::string_view, VkmBlendFactor> kBlendFactorTable = {
            { "zero", VkmBlendFactor::Zero },
            { "one", VkmBlendFactor::One },
            { "src_color", VkmBlendFactor::SrcColor },
            { "one_minus_src_color", VkmBlendFactor::OneMinusSrcColor },
            { "dst_color", VkmBlendFactor::DstColor },
            { "one_minus_dst_color", VkmBlendFactor::OneMinusDstColor },
            { "src_alpha", VkmBlendFactor::SrcAlpha },
            { "one_minus_src_alpha", VkmBlendFactor::OneMinusSrcAlpha },
            { "dst_alpha", VkmBlendFactor::DstAlpha },
            { "one_minus_dst_alpha", VkmBlendFactor::OneMinusDstAlpha },
            { "constant_color", VkmBlendFactor::ConstantColor },
            { "one_minus_constant_color", VkmBlendFactor::OneMinusConstantColor },
            { "constant_alpha", VkmBlendFactor::ConstantAlpha },
            { "one_minus_constant_alpha", VkmBlendFactor::OneMinusConstantAlpha },
            { "src_alpha_saturate", VkmBlendFactor::SrcAlphaSaturate },
        };

        const std::unordered_map<std::string_view, VkmBlendOp> kBlendOpTable = {
            { "add", VkmBlendOp::Add },
            { "subtract", VkmBlendOp::Subtract },
            { "reverse_subtract", VkmBlendOp::ReverseSubtract },
            { "min", VkmBlendOp::Min },
            { "max", VkmBlendOp::Max },
        };

        // Accumulates a parse failure without using C++ exceptions. The Emscripten/WASM
        // build of this project is compiled without -fexceptions (see the EMSCRIPTEN block
        // in the top-level CMakeLists.txt), so a `throw` here would abort the whole module
        // instead of being catchable. Every helper below returns without effect once a
        // failure has already been recorded, so the first (most specific) error wins and
        // callers can keep chaining helper calls without checking after every single one.
        class ParseState
        {
        public:
            bool failed() const { return !_error.empty(); }
            const std::string& error() const { return _error; }
            void fail(const std::string& message)
            {
                if (_error.empty())
                {
                    _error = message;
                }
            }

        private:
            std::string _error;
        };

        // `out` is generic so the same call populates either a plain enum field (base parse)
        // or an overlay's std::optional<EnumT> field (option-overlay parse); `out = *result`
        // compiles for both.
        template <typename EnumT, typename OutT>
        void parseEnumField(ParseState& state, const Json& obj, const char* key,
            const std::string& fieldPath, const std::unordered_map<std::string_view, EnumT>& table, OutT& out)
        {
            if (state.failed() || !obj.contains(key))
            {
                return;
            }
            const Json& value = obj.at(key);
            if (!value.is_string())
            {
                state.fail("Field '" + fieldPath + "' must be a string");
                return;
            }
            const std::string strValue = value.get<std::string>();
            const std::optional<EnumT> result = parseEnumFromString(table, strValue);
            if (!result.has_value())
            {
                state.fail("Unknown value '" + strValue + "' for field '" + fieldPath + "'");
                return;
            }
            out = *result;
        }

        // `out` is generic so the same call populates either a plain bool field or an
        // overlay's std::optional<bool> field.
        template <typename OutT>
        void parseBoolField(ParseState& state, const Json& obj, const char* key, const std::string& fieldPath, OutT& out)
        {
            if (state.failed() || !obj.contains(key))
            {
                return;
            }
            const Json& value = obj.at(key);
            if (!value.is_boolean())
            {
                state.fail("Field '" + fieldPath + "' must be a boolean");
                return;
            }
            out = value.get<bool>();
        }

        // `out` is generic so the same call populates either a plain uint32_t field or an
        // overlay's std::optional<uint32_t> field.
        template <typename OutT>
        void parseUint32Field(ParseState& state, const Json& obj, const char* key, const std::string& fieldPath, OutT& out)
        {
            if (state.failed() || !obj.contains(key))
            {
                return;
            }
            const Json& value = obj.at(key);
            if (!value.is_number_integer() && !value.is_number_unsigned())
            {
                state.fail("Field '" + fieldPath + "' must be an integer");
                return;
            }
            out = value.get<uint32_t>();
        }

        void parseStringField(ParseState& state, const Json& obj, const char* key, const std::string& fieldPath, std::string& out)
        {
            if (state.failed() || !obj.contains(key))
            {
                return;
            }
            const Json& value = obj.at(key);
            if (!value.is_string())
            {
                state.fail("Field '" + fieldPath + "' must be a string");
                return;
            }
            out = value.get<std::string>();
        }

        // Returns the child object at `key`, or nullptr if absent. Fails `state` if present
        // but not a JSON object.
        const Json* getObjectField(ParseState& state, const Json& obj, const char* key, const std::string& fieldPath)
        {
            if (state.failed() || !obj.contains(key))
            {
                return nullptr;
            }
            const Json& value = obj.at(key);
            if (!value.is_object())
            {
                state.fail("Field '" + fieldPath + "' must be an object");
                return nullptr;
            }
            return &value;
        }

        // Converts a single JSON value under "definitions" into the string stored in
        // VkmShaderStageDescriptor::definitions ("" => valueless flag, i.e. `-D NAME`;
        // any other string => `-D NAME=VALUE`).
        std::string parseDefinitionValue(ParseState& state, const std::string& name, const Json& value)
        {
            if (value.is_null())
            {
                return "";
            }
            if (value.is_boolean())
            {
                if (!value.get<bool>())
                {
                    state.fail("Definition '" + name + "' has value false, which is not supported");
                    return "";
                }
                return "";
            }
            if (value.is_string())
            {
                return value.get<std::string>();
            }
            if (value.is_number_integer())
            {
                return std::to_string(value.get<int64_t>());
            }
            if (value.is_number_float())
            {
                return std::to_string(value.get<double>());
            }
            state.fail("Definition '" + name + "' has an unsupported JSON value type");
            return "";
        }

        // Parses a JSON "definitions" object (if present under `key`) into `out`, converting
        // each value via parseDefinitionValue. Shared by shader-stage parsing and the
        // "options" overlay parse.
        void parseDefinitionsMap(ParseState& state, const Json& obj, const char* key,
            const std::string& fieldPath, std::unordered_map<std::string, std::string>& out)
        {
            if (state.failed() || !obj.contains(key))
            {
                return;
            }
            const Json& definitions = obj.at(key);
            if (!definitions.is_object())
            {
                state.fail("Field '" + fieldPath + "' must be an object");
                return;
            }
            for (const auto& item : definitions.items())
            {
                const std::string converted = parseDefinitionValue(state, item.key(), item.value());
                if (state.failed())
                {
                    return;
                }
                out[item.key()] = converted;
            }
        }

        void parseShaderStage(ParseState& state, const Json& obj, const std::string& fieldPrefix, VkmShaderStageDescriptor& out)
        {
            if (state.failed())
            {
                return;
            }
            if (!obj.is_object() || !obj.contains("filepath") || !obj.at("filepath").is_string() ||
                obj.at("filepath").get<std::string>().empty())
            {
                state.fail("Missing or empty required field '" + fieldPrefix + ".filepath'");
                return;
            }
            out.filepath = obj.at("filepath").get<std::string>();

            parseStringField(state, obj, "entry_point", fieldPrefix + ".entry_point", out.entryPoint);
            if (state.failed())
            {
                return;
            }

            parseDefinitionsMap(state, obj, "definitions", fieldPrefix + ".definitions", out.definitions);
        }

        // Templated over the output struct so a single body populates either a plain
        // VkmStencilOpState (base parse) or a VkmStencilOpStateOverlay (option parse) --
        // parseEnumField/parseUint32Field's generic `out` handles both plain and optional fields.
        template <typename StencilOut>
        void parseStencilOpStateFields(ParseState& state, const Json& obj, const std::string& fieldPrefix, StencilOut& out)
        {
            parseEnumField(state, obj, "fail_op", fieldPrefix + ".fail_op", kStencilOpTable, out.failOp);
            parseEnumField(state, obj, "pass_op", fieldPrefix + ".pass_op", kStencilOpTable, out.passOp);
            parseEnumField(state, obj, "depth_fail_op", fieldPrefix + ".depth_fail_op", kStencilOpTable, out.depthFailOp);
            parseEnumField(state, obj, "compare_op", fieldPrefix + ".compare_op", kCompareOpTable, out.compareOp);
            parseUint32Field(state, obj, "compare_mask", fieldPrefix + ".compare_mask", out.compareMask);
            parseUint32Field(state, obj, "write_mask", fieldPrefix + ".write_mask", out.writeMask);
        }

        // Rasterization / depth-stencil-scalar field groups, templated over the output struct
        // so the same body serves both base (plain fields) and option-overlay (optional fields).
        template <typename RasterOut>
        void parseRasterizationStateFields(ParseState& state, const Json& obj, const std::string& fieldPrefix, RasterOut& out)
        {
            parseEnumField(state, obj, "fill_mode", fieldPrefix + ".fill_mode", kFillModeTable, out.fillMode);
            parseEnumField(state, obj, "cull_mode", fieldPrefix + ".cull_mode", kCullModeTable, out.cullMode);
            parseEnumField(state, obj, "front_face", fieldPrefix + ".front_face", kFrontFaceTable, out.frontFace);
        }

        // Scalar depth-stencil fields only; front/back stencil op state is handled by callers
        // (its plain-vs-optional wrapping differs between base and overlay).
        template <typename DepthStencilOut>
        void parseDepthStencilScalarFields(ParseState& state, const Json& obj, const std::string& fieldPrefix, DepthStencilOut& out)
        {
            parseBoolField(state, obj, "depth_test_enable", fieldPrefix + ".depth_test_enable", out.depthTestEnable);
            parseBoolField(state, obj, "depth_write_enable", fieldPrefix + ".depth_write_enable", out.depthWriteEnable);
            parseEnumField(state, obj, "depth_compare_op", fieldPrefix + ".depth_compare_op", kCompareOpTable, out.depthCompareOp);
            parseEnumField(state, obj, "depth_stencil_format", fieldPrefix + ".depth_stencil_format", kFormatTable, out.depthStencilFormat);
            parseBoolField(state, obj, "stencil_test_enable", fieldPrefix + ".stencil_test_enable", out.stencilTestEnable);
            parseUint32Field(state, obj, "stencil_reference", fieldPrefix + ".stencil_reference", out.stencilReference);
        }

        // Parses a "color_attachments" JSON array into `out` (whole-array replace). Shared by
        // the base parse and the option-overlay parse.
        bool parseColorAttachmentsArray(ParseState& state, const Json& colorAttachments,
            const std::string& fieldNamePrefix, std::vector<VkmColorBlendAttachmentState>& out)
        {
            if (!colorAttachments.is_array())
            {
                state.fail("Field '" + fieldNamePrefix + "' must be an array");
                return false;
            }
            out.clear();
            for (size_t i = 0; i < colorAttachments.size(); ++i)
            {
                const Json& attachment = colorAttachments.at(i);
                const std::string fieldPrefix = fieldNamePrefix + "[" + std::to_string(i) + "]";

                if (!attachment.is_object() || !attachment.contains("format"))
                {
                    state.fail("Missing required field '" + fieldPrefix + ".format'");
                    return false;
                }

                VkmColorBlendAttachmentState attachmentState{};
                parseEnumField(state, attachment, "format", fieldPrefix + ".format", kFormatTable, attachmentState.format);
                parseBoolField(state, attachment, "blend_enable", fieldPrefix + ".blend_enable", attachmentState.blendEnable);
                parseEnumField(state, attachment, "src_color_blend_factor", fieldPrefix + ".src_color_blend_factor", kBlendFactorTable, attachmentState.srcColorBlendFactor);
                parseEnumField(state, attachment, "dst_color_blend_factor", fieldPrefix + ".dst_color_blend_factor", kBlendFactorTable, attachmentState.dstColorBlendFactor);
                parseEnumField(state, attachment, "src_alpha_blend_factor", fieldPrefix + ".src_alpha_blend_factor", kBlendFactorTable, attachmentState.srcAlphaBlendFactor);
                parseEnumField(state, attachment, "dst_alpha_blend_factor", fieldPrefix + ".dst_alpha_blend_factor", kBlendFactorTable, attachmentState.dstAlphaBlendFactor);
                parseEnumField(state, attachment, "color_blend_op", fieldPrefix + ".color_blend_op", kBlendOpTable, attachmentState.colorBlendOp);
                parseEnumField(state, attachment, "alpha_blend_op", fieldPrefix + ".alpha_blend_op", kBlendOpTable, attachmentState.alphaBlendOp);

                if (state.failed())
                {
                    return false;
                }
                out.push_back(attachmentState);
            }
            return true;
        }

        // Parses a compact "[type][count][type][count]..." string (e.g. "float3float4float2")
        // into a VkmVertexInputLayoutPart. Each token is a type keyword (float/int/uint)
        // immediately followed by 1-4 decimal digits giving that attribute's component count.
        // Locations are assigned sequentially starting at 0; offsets/stride are computed
        // assuming 4 bytes per component (float/int/uint are all 32-bit types).
        bool parseVertexInputLayoutPart(ParseState& state, const std::string& compact,
            const std::string& fieldPath, VkmVertexInputLayoutPart& out)
        {
            size_t pos = 0;
            uint32_t location = 0;
            uint32_t offset = 0;
            while (pos < compact.size())
            {
                VkmVertexAttributeBaseType baseType;
                if (compact.compare(pos, 5, "float") == 0) { baseType = VkmVertexAttributeBaseType::Float; pos += 5; }
                else if (compact.compare(pos, 4, "uint") == 0) { baseType = VkmVertexAttributeBaseType::UInt; pos += 4; }
                else if (compact.compare(pos, 3, "int") == 0) { baseType = VkmVertexAttributeBaseType::Int; pos += 3; }
                else
                {
                    state.fail("Unrecognized type token in '" + fieldPath + "' at offset " + std::to_string(pos));
                    return false;
                }

                const size_t digitsStart = pos;
                while (pos < compact.size() && std::isdigit(static_cast<unsigned char>(compact[pos])))
                {
                    ++pos;
                }
                if (pos == digitsStart)
                {
                    state.fail("Expected component count after type in '" + fieldPath + "' at offset " + std::to_string(digitsStart));
                    return false;
                }
                // A valid count is always a single digit (1-4); reject longer digit runs
                // before calling std::stoi so an oversized run can never overflow int.
                if (pos - digitsStart > 1)
                {
                    state.fail("Component count must be between 1 and 4 in '" + fieldPath + "'");
                    return false;
                }

                const int count = std::stoi(compact.substr(digitsStart, pos - digitsStart));
                if (count < 1 || count > 4)
                {
                    state.fail("Component count must be between 1 and 4 in '" + fieldPath + "'");
                    return false;
                }

                VkmVertexAttributeDescriptor attribute{};
                attribute.baseType = baseType;
                attribute.componentCount = static_cast<uint32_t>(count);
                attribute.location = location++;
                attribute.offset = offset;
                out.attributes.push_back(attribute);

                offset += static_cast<uint32_t>(count) * 4;
            }
            if (out.attributes.empty())
            {
                state.fail("Field '" + fieldPath + "' must contain at least one attribute");
                return false;
            }
            out.stride = offset;
            return true;
        }

        // Parses an "input_layout" JSON object's per_vertex/per_instance compact strings into
        // `out` (whole-struct replace). Shared by the base parse and the option-overlay parse.
        void parseInputLayout(ParseState& state, const Json& inputLayout, const std::string& fieldPrefix,
            VkmVertexInputLayoutDescriptor& out)
        {
            if (inputLayout.contains("per_vertex"))
            {
                std::string compact;
                parseStringField(state, inputLayout, "per_vertex", fieldPrefix + ".per_vertex", compact);
                VkmVertexInputLayoutPart part{};
                if (!state.failed() && parseVertexInputLayoutPart(state, compact, fieldPrefix + ".per_vertex", part))
                {
                    out.perVertex = part;
                }
            }
            if (!state.failed() && inputLayout.contains("per_instance"))
            {
                std::string compact;
                parseStringField(state, inputLayout, "per_instance", fieldPrefix + ".per_instance", compact);
                VkmVertexInputLayoutPart part{};
                if (!state.failed() && parseVertexInputLayoutPart(state, compact, fieldPrefix + ".per_instance", part))
                {
                    out.perInstance = part;
                }
            }
        }

        // Parses a single "options" entry's body into an overlay. Reuses the same field-group
        // helpers as the base parse, targeting the Overlay struct types (optional fields).
        void parseOptionOverlay(ParseState& state, const Json& obj, const std::string& fieldPrefix,
            VkmPipelineStateOptionOverlay& overlay)
        {
            parseEnumField(state, obj, "primitive_topology", fieldPrefix + ".primitive_topology", kPrimitiveTopologyTable, overlay.primitiveTopology);

            if (const Json* rasterizationState = getObjectField(state, obj, "rasterization_state", fieldPrefix + ".rasterization_state"))
            {
                VkmRasterizationStateOverlay raster{};
                parseRasterizationStateFields(state, *rasterizationState, fieldPrefix + ".rasterization_state", raster);
                overlay.rasterizationState = raster;
            }

            if (const Json* depthStencilState = getObjectField(state, obj, "depth_stencil_state", fieldPrefix + ".depth_stencil_state"))
            {
                VkmDepthStencilStateOverlay depthStencil{};
                parseDepthStencilScalarFields(state, *depthStencilState, fieldPrefix + ".depth_stencil_state", depthStencil);

                if (const Json* front = getObjectField(state, *depthStencilState, "front", fieldPrefix + ".depth_stencil_state.front"))
                {
                    VkmStencilOpStateOverlay stencil{};
                    parseStencilOpStateFields(state, *front, fieldPrefix + ".depth_stencil_state.front", stencil);
                    depthStencil.front = stencil;
                }
                if (const Json* back = getObjectField(state, *depthStencilState, "back", fieldPrefix + ".depth_stencil_state.back"))
                {
                    VkmStencilOpStateOverlay stencil{};
                    parseStencilOpStateFields(state, *back, fieldPrefix + ".depth_stencil_state.back", stencil);
                    depthStencil.back = stencil;
                }
                overlay.depthStencilState = depthStencil;
            }

            if (state.failed())
            {
                return;
            }

            if (obj.contains("color_attachments"))
            {
                std::vector<VkmColorBlendAttachmentState> attachments;
                if (!parseColorAttachmentsArray(state, obj.at("color_attachments"), fieldPrefix + ".color_attachments", attachments))
                {
                    return;
                }
                overlay.colorAttachments = std::move(attachments);
            }

            if (const Json* inputLayout = getObjectField(state, obj, "input_layout", fieldPrefix + ".input_layout"))
            {
                VkmVertexInputLayoutDescriptor layout{};
                parseInputLayout(state, *inputLayout, fieldPrefix + ".input_layout", layout);
                if (state.failed())
                {
                    return;
                }
                overlay.vertexInputLayout = layout;
            }

            parseDefinitionsMap(state, obj, "definitions", fieldPrefix + ".definitions", overlay.definitions);
            if (state.failed())
            {
                return;
            }

            if (const Json* shaders = getObjectField(state, obj, "shaders", fieldPrefix + ".shaders"))
            {
                if (const Json* vertex = getObjectField(state, *shaders, "vertex", fieldPrefix + ".shaders.vertex"))
                {
                    parseDefinitionsMap(state, *vertex, "definitions", fieldPrefix + ".shaders.vertex.definitions", overlay.vertexDefinitions);
                }
                if (const Json* fragment = getObjectField(state, *shaders, "fragment", fieldPrefix + ".shaders.fragment"))
                {
                    parseDefinitionsMap(state, *fragment, "definitions", fieldPrefix + ".shaders.fragment.definitions", overlay.fragmentDefinitions);
                }
                if (const Json* compute = getObjectField(state, *shaders, "compute", fieldPrefix + ".shaders.compute"))
                {
                    parseDefinitionsMap(state, *compute, "definitions", fieldPrefix + ".shaders.compute.definitions", overlay.computeDefinitions);
                }
            }
        }

        bool parseDescriptor(ParseState& state, const Json& root, VkmPipelineStateDescriptor& desc)
        {
            if (!root.is_object())
            {
                state.fail("Top-level JSON value must be an object");
                return false;
            }

            parseStringField(state, root, "name", "name", desc.name);
            parseEnumField(state, root, "primitive_topology", "primitive_topology", kPrimitiveTopologyTable, desc.primitiveTopology);

            if (const Json* rasterizationState = getObjectField(state, root, "rasterization_state", "rasterization_state"))
            {
                parseRasterizationStateFields(state, *rasterizationState, "rasterization_state", desc.rasterizationState);
            }

            if (const Json* depthStencilState = getObjectField(state, root, "depth_stencil_state", "depth_stencil_state"))
            {
                parseDepthStencilScalarFields(state, *depthStencilState, "depth_stencil_state", desc.depthStencilState);

                if (const Json* front = getObjectField(state, *depthStencilState, "front", "depth_stencil_state.front"))
                {
                    parseStencilOpStateFields(state, *front, "depth_stencil_state.front", desc.depthStencilState.front);
                }
                if (const Json* back = getObjectField(state, *depthStencilState, "back", "depth_stencil_state.back"))
                {
                    parseStencilOpStateFields(state, *back, "depth_stencil_state.back", desc.depthStencilState.back);
                }
            }

            if (state.failed())
            {
                return false;
            }

            if (root.contains("color_attachments"))
            {
                desc.colorAttachments.clear();
                if (!parseColorAttachmentsArray(state, root.at("color_attachments"), "color_attachments", desc.colorAttachments))
                {
                    return false;
                }
            }

            if (const Json* inputLayout = getObjectField(state, root, "input_layout", "input_layout"))
            {
                parseInputLayout(state, *inputLayout, "input_layout", desc.vertexInputLayout);
            }

            if (state.failed())
            {
                return false;
            }

            if (!root.contains("shaders") || !root.at("shaders").is_object())
            {
                state.fail("Missing required field 'shaders'");
                return false;
            }
            const Json& shaders = root.at("shaders");

            if (!shaders.contains("vertex"))
            {
                state.fail("Missing required field 'shaders.vertex'");
                return false;
            }
            VkmShaderStageDescriptor vertexShader{};
            parseShaderStage(state, shaders.at("vertex"), "shaders.vertex", vertexShader);
            if (state.failed())
            {
                return false;
            }
            desc.vertexShader = vertexShader;

            if (shaders.contains("fragment"))
            {
                VkmShaderStageDescriptor fragmentShader{};
                parseShaderStage(state, shaders.at("fragment"), "shaders.fragment", fragmentShader);
                if (state.failed())
                {
                    return false;
                }
                desc.fragmentShader = fragmentShader;
            }

            if (shaders.contains("compute"))
            {
                VkmShaderStageDescriptor computeShader{};
                parseShaderStage(state, shaders.at("compute"), "shaders.compute", computeShader);
                if (state.failed())
                {
                    return false;
                }
                desc.computeShader = computeShader;
            }

            if (root.contains("options"))
            {
                const Json& options = root.at("options");
                if (!options.is_object())
                {
                    state.fail("Field 'options' must be an object");
                    return false;
                }
                for (const auto& item : options.items())
                {
                    const std::string& optionName = item.key();
                    const Json& optionBody = item.value();
                    const std::string fieldPrefix = "options." + optionName;
                    if (!optionBody.is_object())
                    {
                        state.fail("Field '" + fieldPrefix + "' must be an object");
                        return false;
                    }
                    VkmPipelineStateOptionOverlay overlay{};
                    parseOptionOverlay(state, optionBody, fieldPrefix, overlay);
                    if (state.failed())
                    {
                        return false;
                    }
                    desc.options[optionName] = std::move(overlay);
                }
            }

            return true;
        }

        void applyStencilOverlay(const VkmStencilOpStateOverlay& overlay, VkmStencilOpState& out)
        {
            if (overlay.failOp.has_value()) out.failOp = *overlay.failOp;
            if (overlay.passOp.has_value()) out.passOp = *overlay.passOp;
            if (overlay.depthFailOp.has_value()) out.depthFailOp = *overlay.depthFailOp;
            if (overlay.compareOp.has_value()) out.compareOp = *overlay.compareOp;
            if (overlay.compareMask.has_value()) out.compareMask = *overlay.compareMask;
            if (overlay.writeMask.has_value()) out.writeMask = *overlay.writeMask;
        }

        // Merges pipeline-wide then per-stage definitions into `stage` if it is present.
        // Per-stage entries win on key collision (applied last).
        void mergeStageDefinitions(std::optional<VkmShaderStageDescriptor>& stage,
            const std::unordered_map<std::string, std::string>& pipelineWide,
            const std::unordered_map<std::string, std::string>& perStage)
        {
            if (!stage.has_value())
            {
                return;
            }
            for (const auto& [key, value] : pipelineWide)
            {
                stage->definitions.insert_or_assign(key, value);
            }
            for (const auto& [key, value] : perStage)
            {
                stage->definitions.insert_or_assign(key, value);
            }
        }
    } // namespace

    std::optional<VkmPipelineStateDescriptor> parsePipelineStateFromString(const std::string& jsonText, std::string* outError)
    {
        const Json root = Json::parse(jsonText, nullptr, /*allow_exceptions=*/false);
        if (root.is_discarded())
        {
            const std::string message = "JSON parse error: malformed JSON text";
            if (outError != nullptr)
            {
                *outError = message;
            }
            VKM_DEBUG_ERROR(message.c_str());
            return std::nullopt;
        }

        ParseState state;
        VkmPipelineStateDescriptor desc{};
        if (!parseDescriptor(state, root, desc))
        {
            if (outError != nullptr)
            {
                *outError = state.error();
            }
            VKM_DEBUG_ERROR(state.error().c_str());
            return std::nullopt;
        }

        return desc;
    }

    std::optional<VkmPipelineStateDescriptor> parsePipelineStateFromFile(const std::string& filepath, std::string* outError)
    {
        std::ifstream file(filepath);
        if (!file.is_open())
        {
            const std::string message = "Failed to open pipeline state file '" + filepath + "'";
            if (outError != nullptr)
            {
                *outError = message;
            }
            VKM_DEBUG_ERROR(message.c_str());
            return std::nullopt;
        }

        std::stringstream buffer;
        buffer << file.rdbuf();

        return parsePipelineStateFromString(buffer.str(), outError);
    }

    std::optional<std::vector<VkmPipelineStateDescriptor>> expandPipelineStateOptions(
        const VkmPipelineStateDescriptor& base, std::string* outError)
    {
        std::vector<VkmPipelineStateDescriptor> variants;

        if (base.options.empty())
        {
            VkmPipelineStateDescriptor single = base;
            single.options.clear();
            variants.push_back(std::move(single));
        }
        else
        {
            if (base.name.empty())
            {
                const std::string message = "Pipeline state with a non-empty 'options' object must have a non-empty 'name'";
                if (outError != nullptr)
                {
                    *outError = message;
                }
                VKM_DEBUG_ERROR(message.c_str());
                return std::nullopt;
            }

            variants.reserve(base.options.size());
            for (const auto& [optionName, overlay] : base.options)
            {
                VkmPipelineStateDescriptor variant = base;
                variant.options.clear();
                variant.optionName = optionName;
                variant.name = base.name + "[" + optionName + "]";

                if (overlay.primitiveTopology.has_value())
                {
                    variant.primitiveTopology = *overlay.primitiveTopology;
                }
                if (overlay.rasterizationState.has_value())
                {
                    const VkmRasterizationStateOverlay& raster = *overlay.rasterizationState;
                    if (raster.fillMode.has_value()) variant.rasterizationState.fillMode = *raster.fillMode;
                    if (raster.cullMode.has_value()) variant.rasterizationState.cullMode = *raster.cullMode;
                    if (raster.frontFace.has_value()) variant.rasterizationState.frontFace = *raster.frontFace;
                }
                if (overlay.depthStencilState.has_value())
                {
                    const VkmDepthStencilStateOverlay& depthStencil = *overlay.depthStencilState;
                    if (depthStencil.depthTestEnable.has_value()) variant.depthStencilState.depthTestEnable = *depthStencil.depthTestEnable;
                    if (depthStencil.depthWriteEnable.has_value()) variant.depthStencilState.depthWriteEnable = *depthStencil.depthWriteEnable;
                    if (depthStencil.depthCompareOp.has_value()) variant.depthStencilState.depthCompareOp = *depthStencil.depthCompareOp;
                    if (depthStencil.depthStencilFormat.has_value()) variant.depthStencilState.depthStencilFormat = *depthStencil.depthStencilFormat;
                    if (depthStencil.stencilTestEnable.has_value()) variant.depthStencilState.stencilTestEnable = *depthStencil.stencilTestEnable;
                    if (depthStencil.stencilReference.has_value()) variant.depthStencilState.stencilReference = *depthStencil.stencilReference;
                    if (depthStencil.front.has_value()) applyStencilOverlay(*depthStencil.front, variant.depthStencilState.front);
                    if (depthStencil.back.has_value()) applyStencilOverlay(*depthStencil.back, variant.depthStencilState.back);
                }
                if (overlay.colorAttachments.has_value())
                {
                    variant.colorAttachments = *overlay.colorAttachments;
                }
                if (overlay.vertexInputLayout.has_value())
                {
                    variant.vertexInputLayout = *overlay.vertexInputLayout;
                }

                mergeStageDefinitions(variant.vertexShader, overlay.definitions, overlay.vertexDefinitions);
                mergeStageDefinitions(variant.fragmentShader, overlay.definitions, overlay.fragmentDefinitions);
                mergeStageDefinitions(variant.computeShader, overlay.definitions, overlay.computeDefinitions);

                variants.push_back(std::move(variant));
            }
        }

        for (const VkmPipelineStateDescriptor& variant : variants)
        {
            if (variant.computeShader.has_value() &&
                (variant.vertexShader.has_value() || variant.fragmentShader.has_value()))
            {
                const std::string message = "Pipeline state variant '" + variant.name +
                    "' has both a compute shader and a graphics (vertex/fragment) shader set";
                if (outError != nullptr)
                {
                    *outError = message;
                }
                VKM_DEBUG_ERROR(message.c_str());
                return std::nullopt;
            }
        }

        return variants;
    }
} // namespace vkm
