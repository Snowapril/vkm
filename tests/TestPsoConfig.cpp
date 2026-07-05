#include <doctest/doctest.h>

#include <vkm/renderer/backend/common/pipeline_state.h>
#include <vkm/renderer/backend/common/pipeline_state_parser.h>

#include <fstream>
#include <sstream>
#include <string>

namespace
{
std::string readFileToString(const std::string& filepath)
{
    std::ifstream file(filepath);
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}
} // namespace

TEST_CASE("VkmPipelineStateDescriptor - parses full JSON with all fields set") {
#if defined(VKM_PLATFORM_WASM)
    MESSAGE("Skipping: RESOURCES_DIR fixtures are not mounted in the Emscripten test binary's virtual filesystem.");
    return;
#endif
    const std::string filepath = std::string(RESOURCES_DIR) + "tests/pso_full.json";
    std::optional<vkm::VkmPipelineStateDescriptor> result = vkm::parsePipelineStateFromFile(filepath);
    REQUIRE(result.has_value());

    const vkm::VkmPipelineStateDescriptor& desc = *result;

    CHECK(desc.name == "triangle_pso");
    CHECK(desc.primitiveTopology == vkm::VkmPrimitiveTopology::TriangleList);

    CHECK(desc.rasterizationState.fillMode == vkm::VkmFillMode::Solid);
    CHECK(desc.rasterizationState.cullMode == vkm::VkmCullMode::Back);
    CHECK(desc.rasterizationState.frontFace == vkm::VkmFrontFace::CounterClockwise);

    CHECK(desc.depthStencilState.depthTestEnable == true);
    CHECK(desc.depthStencilState.depthWriteEnable == true);
    CHECK(desc.depthStencilState.depthCompareOp == vkm::VkmCompareOp::LessOrEqual);
    CHECK(desc.depthStencilState.depthStencilFormat == vkm::VkmFormat::D32_SFLOAT);
    CHECK(desc.depthStencilState.stencilTestEnable == false);
    CHECK(desc.depthStencilState.stencilReference == 0);
    CHECK(desc.depthStencilState.front.compareOp == vkm::VkmCompareOp::Always);
    CHECK(desc.depthStencilState.back.compareOp == vkm::VkmCompareOp::Always);

    REQUIRE(desc.colorAttachments.size() == 1);
    CHECK(desc.colorAttachments[0].format == vkm::VkmFormat::BGRA8_UNORM);
    CHECK(desc.colorAttachments[0].blendEnable == false);

    REQUIRE(desc.vertexShader.has_value());
    CHECK(desc.vertexShader->filepath == "triangle.vert");
    CHECK(desc.vertexShader->entryPoint == "main");
    CHECK(desc.vertexShader->definitions.at("USE_VERTEX_COLOR") == "");
    CHECK(desc.vertexShader->definitions.at("MAX_LIGHTS") == "4");

    REQUIRE(desc.fragmentShader.has_value());
    CHECK(desc.fragmentShader->filepath == "triangle.frag");
}

TEST_CASE("VkmPipelineStateDescriptor - missing fields fall back to documented defaults") {
#if defined(VKM_PLATFORM_WASM)
    MESSAGE("Skipping: RESOURCES_DIR fixtures are not mounted in the Emscripten test binary's virtual filesystem.");
    return;
#endif
    const std::string filepath = std::string(RESOURCES_DIR) + "tests/pso_minimal.json";
    std::optional<vkm::VkmPipelineStateDescriptor> result = vkm::parsePipelineStateFromFile(filepath);
    REQUIRE(result.has_value());

    const vkm::VkmPipelineStateDescriptor& desc = *result;

    CHECK(desc.rasterizationState.cullMode == vkm::VkmCullMode::Back);
    CHECK(desc.rasterizationState.frontFace == vkm::VkmFrontFace::CounterClockwise);
    CHECK(desc.rasterizationState.fillMode == vkm::VkmFillMode::Solid);

    CHECK(desc.depthStencilState.depthTestEnable == true);
    CHECK(desc.depthStencilState.depthWriteEnable == true);
    CHECK(desc.depthStencilState.depthCompareOp == vkm::VkmCompareOp::Less);
    CHECK(desc.depthStencilState.stencilTestEnable == false);

    REQUIRE(desc.colorAttachments.size() == 1);
    CHECK(desc.colorAttachments[0].blendEnable == false);

    REQUIRE(desc.fragmentShader.has_value());
    CHECK(desc.fragmentShader->entryPoint == "main");
}

TEST_CASE("VkmPipelineStateDescriptor - unknown enum string fails to parse") {
#if defined(VKM_PLATFORM_WASM)
    MESSAGE("Skipping: RESOURCES_DIR fixtures are not mounted in the Emscripten test binary's virtual filesystem.");
    return;
#endif
    const std::string filepath = std::string(RESOURCES_DIR) + "tests/pso_bad_enum.json";
    std::string outError;
    std::optional<vkm::VkmPipelineStateDescriptor> result = vkm::parsePipelineStateFromFile(filepath, &outError);
    CHECK_FALSE(result.has_value());
    CHECK(outError.find("cull_mode") != std::string::npos);
}

TEST_CASE("VkmPipelineStateDescriptor - missing required vertex shader fails to parse") {
#if defined(VKM_PLATFORM_WASM)
    MESSAGE("Skipping: RESOURCES_DIR fixtures are not mounted in the Emscripten test binary's virtual filesystem.");
    return;
#endif
    const std::string filepath = std::string(RESOURCES_DIR) + "tests/pso_missing_vertex.json";
    std::optional<vkm::VkmPipelineStateDescriptor> result = vkm::parsePipelineStateFromFile(filepath);
    CHECK_FALSE(result.has_value());
}

TEST_CASE("VkmPipelineStateDescriptor - malformed JSON text fails to parse") {
    std::optional<vkm::VkmPipelineStateDescriptor> result = vkm::parsePipelineStateFromString("{ not valid json");
    CHECK_FALSE(result.has_value());
}

TEST_CASE("VkmPipelineStateDescriptor - raw-string input mode matches file input mode") {
#if defined(VKM_PLATFORM_WASM)
    MESSAGE("Skipping: RESOURCES_DIR fixtures are not mounted in the Emscripten test binary's virtual filesystem.");
    return;
#endif
    const std::string filepath = std::string(RESOURCES_DIR) + "tests/pso_full.json";
    const std::string jsonText = readFileToString(filepath);

    std::optional<vkm::VkmPipelineStateDescriptor> fromString = vkm::parsePipelineStateFromString(jsonText);
    std::optional<vkm::VkmPipelineStateDescriptor> fromFile = vkm::parsePipelineStateFromFile(filepath);

    REQUIRE(fromString.has_value());
    REQUIRE(fromFile.has_value());

    CHECK(fromString->name == fromFile->name);
    CHECK(fromString->rasterizationState.cullMode == fromFile->rasterizationState.cullMode);
    REQUIRE(fromString->colorAttachments.size() == fromFile->colorAttachments.size());
    CHECK(fromString->colorAttachments[0].format == fromFile->colorAttachments[0].format);
    REQUIRE(fromString->vertexShader.has_value());
    REQUIRE(fromFile->vertexShader.has_value());
    CHECK(fromString->vertexShader->filepath == fromFile->vertexShader->filepath);
}

TEST_CASE("VkmPipelineStateDescriptor - multiple color attachment formats parsed in order") {
    const std::string jsonText = R"({
        "color_attachments": [
            { "format": "r8g8b8a8_unorm" },
            { "format": "bgra8_unorm" },
            { "format": "d32_sfloat" }
        ],
        "shaders": {
            "vertex": { "filepath": "triangle.vert" }
        }
    })";

    std::optional<vkm::VkmPipelineStateDescriptor> result = vkm::parsePipelineStateFromString(jsonText);
    REQUIRE(result.has_value());

    REQUIRE(result->colorAttachments.size() == 3);
    CHECK(result->colorAttachments[0].format == vkm::VkmFormat::R8G8B8A8_UNORM);
    CHECK(result->colorAttachments[1].format == vkm::VkmFormat::BGRA8_UNORM);
    CHECK(result->colorAttachments[2].format == vkm::VkmFormat::D32_SFLOAT);
}

TEST_CASE("VkmPipelineStateDescriptor - shader definitions value conversion") {
    SUBCASE("supported value kinds convert as documented") {
        const std::string jsonText = R"({
            "color_attachments": [ { "format": "bgra8_unorm" } ],
            "shaders": {
                "vertex": {
                    "filepath": "triangle.vert",
                    "definitions": { "FLAG_TRUE": true, "FLAG_EMPTY": "", "VAL": 4, "STR": "abc" }
                }
            }
        })";

        std::optional<vkm::VkmPipelineStateDescriptor> result = vkm::parsePipelineStateFromString(jsonText);
        REQUIRE(result.has_value());
        REQUIRE(result->vertexShader.has_value());

        const auto& definitions = result->vertexShader->definitions;
        CHECK(definitions.at("FLAG_TRUE") == "");
        CHECK(definitions.at("FLAG_EMPTY") == "");
        CHECK(definitions.at("VAL") == "4");
        CHECK(definitions.at("STR") == "abc");
    }

    SUBCASE("boolean false definition value fails to parse") {
        const std::string jsonText = R"({
            "color_attachments": [ { "format": "bgra8_unorm" } ],
            "shaders": {
                "vertex": {
                    "filepath": "triangle.vert",
                    "definitions": { "FLAG_FALSE": false }
                }
            }
        })";

        std::optional<vkm::VkmPipelineStateDescriptor> result = vkm::parsePipelineStateFromString(jsonText);
        CHECK_FALSE(result.has_value());
    }
}

TEST_CASE("VkmPipelineStateDescriptor - nonexistent file path fails to parse") {
    std::optional<vkm::VkmPipelineStateDescriptor> result =
        vkm::parsePipelineStateFromFile("/no/such/file_that_does_not_exist_12345.json");
    CHECK_FALSE(result.has_value());
}
