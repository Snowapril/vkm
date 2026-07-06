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

        template <typename EnumT>
        void parseEnumField(ParseState& state, const Json& obj, const char* key,
            const std::string& fieldPath, const std::unordered_map<std::string_view, EnumT>& table, EnumT& out)
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

        void parseBoolField(ParseState& state, const Json& obj, const char* key, const std::string& fieldPath, bool& out)
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

        void parseUint32Field(ParseState& state, const Json& obj, const char* key, const std::string& fieldPath, uint32_t& out)
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

            if (obj.contains("definitions"))
            {
                const Json& definitions = obj.at("definitions");
                if (!definitions.is_object())
                {
                    state.fail("Field '" + fieldPrefix + ".definitions' must be an object");
                    return;
                }
                for (const auto& item : definitions.items())
                {
                    const std::string converted = parseDefinitionValue(state, item.key(), item.value());
                    if (state.failed())
                    {
                        return;
                    }
                    out.definitions[item.key()] = converted;
                }
            }
        }

        void parseStencilOpState(ParseState& state, const Json& obj, const std::string& fieldPrefix, VkmStencilOpState& out)
        {
            parseEnumField(state, obj, "fail_op", fieldPrefix + ".fail_op", kStencilOpTable, out.failOp);
            parseEnumField(state, obj, "pass_op", fieldPrefix + ".pass_op", kStencilOpTable, out.passOp);
            parseEnumField(state, obj, "depth_fail_op", fieldPrefix + ".depth_fail_op", kStencilOpTable, out.depthFailOp);
            parseEnumField(state, obj, "compare_op", fieldPrefix + ".compare_op", kCompareOpTable, out.compareOp);
            parseUint32Field(state, obj, "compare_mask", fieldPrefix + ".compare_mask", out.compareMask);
            parseUint32Field(state, obj, "write_mask", fieldPrefix + ".write_mask", out.writeMask);
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
                parseEnumField(state, *rasterizationState, "fill_mode", "rasterization_state.fill_mode", kFillModeTable, desc.rasterizationState.fillMode);
                parseEnumField(state, *rasterizationState, "cull_mode", "rasterization_state.cull_mode", kCullModeTable, desc.rasterizationState.cullMode);
                parseEnumField(state, *rasterizationState, "front_face", "rasterization_state.front_face", kFrontFaceTable, desc.rasterizationState.frontFace);
            }

            if (const Json* depthStencilState = getObjectField(state, root, "depth_stencil_state", "depth_stencil_state"))
            {
                parseBoolField(state, *depthStencilState, "depth_test_enable", "depth_stencil_state.depth_test_enable", desc.depthStencilState.depthTestEnable);
                parseBoolField(state, *depthStencilState, "depth_write_enable", "depth_stencil_state.depth_write_enable", desc.depthStencilState.depthWriteEnable);
                parseEnumField(state, *depthStencilState, "depth_compare_op", "depth_stencil_state.depth_compare_op", kCompareOpTable, desc.depthStencilState.depthCompareOp);
                parseEnumField(state, *depthStencilState, "depth_stencil_format", "depth_stencil_state.depth_stencil_format", kFormatTable, desc.depthStencilState.depthStencilFormat);
                parseBoolField(state, *depthStencilState, "stencil_test_enable", "depth_stencil_state.stencil_test_enable", desc.depthStencilState.stencilTestEnable);
                parseUint32Field(state, *depthStencilState, "stencil_reference", "depth_stencil_state.stencil_reference", desc.depthStencilState.stencilReference);

                if (const Json* front = getObjectField(state, *depthStencilState, "front", "depth_stencil_state.front"))
                {
                    parseStencilOpState(state, *front, "depth_stencil_state.front", desc.depthStencilState.front);
                }
                if (const Json* back = getObjectField(state, *depthStencilState, "back", "depth_stencil_state.back"))
                {
                    parseStencilOpState(state, *back, "depth_stencil_state.back", desc.depthStencilState.back);
                }
            }

            if (state.failed())
            {
                return false;
            }

            if (root.contains("color_attachments"))
            {
                const Json& colorAttachments = root.at("color_attachments");
                if (!colorAttachments.is_array())
                {
                    state.fail("Field 'color_attachments' must be an array");
                    return false;
                }
                for (size_t i = 0; i < colorAttachments.size(); ++i)
                {
                    const Json& attachment = colorAttachments.at(i);
                    const std::string fieldPrefix = "color_attachments[" + std::to_string(i) + "]";

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
                    desc.colorAttachments.push_back(attachmentState);
                }
            }

            if (const Json* inputLayout = getObjectField(state, root, "input_layout", "input_layout"))
            {
                if (inputLayout->contains("per_vertex"))
                {
                    std::string compact;
                    parseStringField(state, *inputLayout, "per_vertex", "input_layout.per_vertex", compact);
                    VkmVertexInputLayoutPart part{};
                    if (!state.failed() && parseVertexInputLayoutPart(state, compact, "input_layout.per_vertex", part))
                    {
                        desc.vertexInputLayout.perVertex = part;
                    }
                }
                if (!state.failed() && inputLayout->contains("per_instance"))
                {
                    std::string compact;
                    parseStringField(state, *inputLayout, "per_instance", "input_layout.per_instance", compact);
                    VkmVertexInputLayoutPart part{};
                    if (!state.failed() && parseVertexInputLayoutPart(state, compact, "input_layout.per_instance", part))
                    {
                        desc.vertexInputLayout.perInstance = part;
                    }
                }
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

            return true;
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
} // namespace vkm
