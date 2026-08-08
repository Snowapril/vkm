#include "UnitTestUtils.hpp"

#include <vkm/renderer/backend/common/pipeline_state.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/pipeline_state_parser.h>
#include <vkm/renderer/backend/common/shader_cache.h>
#include <vkm/renderer/backend/common/shader_cache_util.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
    // ---------------------------------------------------------------------
    // Pure-logic test: driver is never dereferenced when `directory` doesn't
    // exist, so a null driver pointer is safe here.
    // ---------------------------------------------------------------------
    TEST_CASE("VkmPipelineStateManager - nonexistent directory is a no-op")
    {
        vkm::VkmPipelineStateManager manager(nullptr);
        std::string outError;
        const bool result = manager.loadPipelineStatesFromDirectory(
            "/no/such/pipeline_dir_that_does_not_exist_13579", "/no/such/shader_cache_dir", vkm::VkmPipelineStateOrigin::Engine, &outError);
        CHECK(result);
        CHECK(outError.empty());
        CHECK(manager.getPipelineState("anything") == nullptr);
    }
} // namespace

// ---------------------------------------------------------------------------
// Real-driver tests (reuses the fixture pattern from TestEngineSetup.cpp).
// Hand-crafts minimal, valid SPIR-V modules directly (no dxc/glslang dependency
// needed) so real VkPipeline objects can be created and validated end-to-end.
// ---------------------------------------------------------------------------
#ifdef VKM_USE_VULKAN_API

#include <GLFW/glfw3.h>
#include <vkm/renderer/backend/vulkan/vulkan_driver.h>
#include <vkm/renderer/engine.h>

namespace
{
    struct VulkanDriverFixture
    {
        std::unique_ptr<vkm::VkmDriverVulkan> driver;
        vkm::VkmInitResult initResult;
        VulkanDriverFixture()
        {
            glfwInit();
            vkm::VkmEngineLaunchOptions opts{ .enableValidationLayer = true };
            driver = std::unique_ptr<vkm::VkmDriverVulkan>(new vkm::VkmDriverVulkan());
            initResult = driver->initialize(&opts);
        }
        ~VulkanDriverFixture()
        {
            driver.reset();
            glfwTerminate();
        }
    };

    // Encodes `s` as a NUL-terminated, word-padded SPIR-V literal string.
    std::vector<uint32_t> encodeSpirvString(const std::string& s)
    {
        const size_t totalBytes = ((s.size() + 1 + 3) / 4) * 4;
        std::vector<uint32_t> out(totalBytes / 4, 0u);
        for (size_t i = 0; i < s.size(); ++i)
        {
            out[i / 4] |= static_cast<uint32_t>(static_cast<uint8_t>(s[i])) << (8 * (i % 4));
        }
        return out;
    }

    void emitSpirv(std::vector<uint32_t>& out, uint16_t opcode, const std::vector<uint32_t>& operands)
    {
        const uint16_t wordCount = static_cast<uint16_t>(1 + operands.size());
        out.push_back((static_cast<uint32_t>(wordCount) << 16) | opcode);
        out.insert(out.end(), operands.begin(), operands.end());
    }

    // Builds a minimal, spec-valid SPIR-V vertex shader: no inputs, writes a constant
    // vec4 to the gl_Position builtin. Real enough to pass validation-layer SPIR-V
    // validation and real VkPipeline creation, without needing dxc/glslang as a test
    // dependency.
    std::vector<uint32_t> buildMinimalVertexSpirv()
    {
        constexpr uint32_t idVoid = 1, idFuncType = 2, idFloat = 3, idV4Float = 4, idPtrOutV4 = 5,
            idGlPosition = 6, idFloat0 = 7, idFloat1 = 8, idConstVec = 9, idMain = 10, idLabel = 11;
        constexpr uint32_t kBound = idLabel + 1;

        std::vector<uint32_t> code = { 0x07230203u, 0x00010000u, 0u, kBound, 0u };

        emitSpirv(code, 17, { 1 }); // OpCapability Shader
        emitSpirv(code, 14, { 0, 1 }); // OpMemoryModel Logical GLSL450
        {
            std::vector<uint32_t> ep = { 0, idMain }; // ExecutionModel Vertex, %main
            const std::vector<uint32_t> name = encodeSpirvString("main");
            ep.insert(ep.end(), name.begin(), name.end());
            ep.push_back(idGlPosition);
            emitSpirv(code, 15, ep); // OpEntryPoint
        }
        emitSpirv(code, 71, { idGlPosition, 11, 0 }); // OpDecorate BuiltIn Position
        emitSpirv(code, 19, { idVoid }); // OpTypeVoid
        emitSpirv(code, 33, { idFuncType, idVoid }); // OpTypeFunction
        emitSpirv(code, 22, { idFloat, 32 }); // OpTypeFloat
        emitSpirv(code, 23, { idV4Float, idFloat, 4 }); // OpTypeVector
        emitSpirv(code, 32, { idPtrOutV4, 3, idV4Float }); // OpTypePointer Output
        emitSpirv(code, 59, { idPtrOutV4, idGlPosition, 3 }); // OpVariable Output
        emitSpirv(code, 43, { idFloat, idFloat0, 0x00000000u }); // OpConstant 0.0
        emitSpirv(code, 43, { idFloat, idFloat1, 0x3F800000u }); // OpConstant 1.0
        emitSpirv(code, 44, { idV4Float, idConstVec, idFloat0, idFloat0, idFloat0, idFloat1 }); // OpConstantComposite
        emitSpirv(code, 54, { idVoid, idMain, 0, idFuncType }); // OpFunction
        emitSpirv(code, 248, { idLabel }); // OpLabel
        emitSpirv(code, 62, { idGlPosition, idConstVec }); // OpStore
        emitSpirv(code, 253, {}); // OpReturn
        emitSpirv(code, 56, {}); // OpFunctionEnd
        return code;
    }

    // Minimal, spec-valid SPIR-V fragment shader: writes a constant vec4 to a single
    // Location-0 output (matching a one-color-attachment pipeline).
    std::vector<uint32_t> buildMinimalFragmentSpirv()
    {
        constexpr uint32_t idVoid = 1, idFuncType = 2, idFloat = 3, idV4Float = 4, idPtrOutV4 = 5,
            idFragColor = 6, idFloat1 = 7, idConstVec = 8, idMain = 9, idLabel = 10;
        constexpr uint32_t kBound = idLabel + 1;

        std::vector<uint32_t> code = { 0x07230203u, 0x00010000u, 0u, kBound, 0u };

        emitSpirv(code, 17, { 1 }); // OpCapability Shader
        emitSpirv(code, 14, { 0, 1 }); // OpMemoryModel Logical GLSL450
        {
            std::vector<uint32_t> ep = { 4, idMain }; // ExecutionModel Fragment, %main
            const std::vector<uint32_t> name = encodeSpirvString("main");
            ep.insert(ep.end(), name.begin(), name.end());
            ep.push_back(idFragColor);
            emitSpirv(code, 15, ep); // OpEntryPoint
        }
        emitSpirv(code, 16, { idMain, 7 }); // OpExecutionMode OriginUpperLeft
        emitSpirv(code, 71, { idFragColor, 30, 0 }); // OpDecorate Location 0
        emitSpirv(code, 19, { idVoid }); // OpTypeVoid
        emitSpirv(code, 33, { idFuncType, idVoid }); // OpTypeFunction
        emitSpirv(code, 22, { idFloat, 32 }); // OpTypeFloat
        emitSpirv(code, 23, { idV4Float, idFloat, 4 }); // OpTypeVector
        emitSpirv(code, 32, { idPtrOutV4, 3, idV4Float }); // OpTypePointer Output
        emitSpirv(code, 59, { idPtrOutV4, idFragColor, 3 }); // OpVariable Output
        emitSpirv(code, 43, { idFloat, idFloat1, 0x3F800000u }); // OpConstant 1.0
        emitSpirv(code, 44, { idV4Float, idConstVec, idFloat1, idFloat1, idFloat1, idFloat1 }); // OpConstantComposite
        emitSpirv(code, 54, { idVoid, idMain, 0, idFuncType }); // OpFunction
        emitSpirv(code, 248, { idLabel }); // OpLabel
        emitSpirv(code, 62, { idFragColor, idConstVec }); // OpStore
        emitSpirv(code, 253, {}); // OpReturn
        emitSpirv(code, 56, {}); // OpFunctionEnd
        return code;
    }

    void writeVfcache(const fs::path& path, vkm::VkmShaderCacheStage stage, const std::vector<uint32_t>& spirvWords)
    {
        vkm::VkmShaderCacheHeader header{};
        header.magic = vkm::kVkmShaderCacheMagic;
        header.version = vkm::kVkmShaderCacheVersion;
        header.backend = vkm::VkmShaderCacheBackend::Vulkan;
        header.stage = stage;
        header.contentFormat = vkm::VkmShaderCacheContentFormat::SpirV;
        std::strncpy(header.entryPoint, "main", sizeof(header.entryPoint) - 1);
        header.contentSize = static_cast<uint64_t>(spirvWords.size() * sizeof(uint32_t));

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        REQUIRE(out.is_open());
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(reinterpret_cast<const char*>(spirvWords.data()), static_cast<std::streamsize>(header.contentSize));
    }

    // Writes both stages' .vfcache files for one (stem, optionName) pair into `dir`.
    void writeStageCacheFiles(const fs::path& dir, const std::string& stem, const std::string& optionName)
    {
        const fs::path vertPath = dir / vkm::buildShaderCacheFilename(stem, optionName, vkm::VkmShaderCacheStage::Vertex, vkm::VkmShaderCacheBackend::Vulkan);
        const fs::path fragPath = dir / vkm::buildShaderCacheFilename(stem, optionName, vkm::VkmShaderCacheStage::Fragment, vkm::VkmShaderCacheBackend::Vulkan);
        writeVfcache(vertPath, vkm::VkmShaderCacheStage::Vertex, buildMinimalVertexSpirv());
        writeVfcache(fragPath, vkm::VkmShaderCacheStage::Fragment, buildMinimalFragmentSpirv());
    }
} // namespace

TEST_CASE("VkmPipelineStateManager - loadPipelineStatesFromDirectory creates distinct option variants")
{
    VulkanDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    const fs::path tempDir = fs::temp_directory_path() / "vkm_test_pso_manager_dir";
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    fs::create_directories(tempDir, ec);
    REQUIRE_FALSE(ec);

    fs::copy_file(std::string(RESOURCES_DIR) + "tests/pso_with_options.json",
        tempDir / "pso_with_options.json", fs::copy_options::overwrite_existing, ec);
    REQUIRE_FALSE(ec);

    writeStageCacheFiles(tempDir, "triangle", "default");
    writeStageCacheFiles(tempDir, "triangle", "wireframe");

    vkm::VkmPipelineStateManager manager(f.driver.get());
    std::string outError;
    const bool result = manager.loadPipelineStatesFromDirectory(tempDir.string(), tempDir.string(), vkm::VkmPipelineStateOrigin::User, &outError);
    REQUIRE_MESSAGE(result, outError);

    vkm::VkmPipelineStateBase* defaultPso = manager.getPipelineState("triangle_pso[default]", vkm::VkmPipelineStateOrigin::User);
    vkm::VkmPipelineStateBase* wireframePso = manager.getPipelineState("triangle_pso[wireframe]", vkm::VkmPipelineStateOrigin::User);
    REQUIRE(defaultPso != nullptr);
    REQUIRE(wireframePso != nullptr);
    CHECK(defaultPso != wireframePso);
    CHECK(defaultPso->getName() == "triangle_pso[default]");
    CHECK(wireframePso->getName() == "triangle_pso[wireframe]");

    fs::remove_all(tempDir, ec);
}

TEST_CASE("VkmPipelineStateManager - Engine and User origins are isolated")
{
    VulkanDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    const fs::path tempDir = fs::temp_directory_path() / "vkm_test_pso_manager_origins";
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    fs::create_directories(tempDir, ec);
    REQUIRE_FALSE(ec);

    writeStageCacheFiles(tempDir, "shared", "");

    vkm::VkmPipelineStateDescriptor engineDesc{};
    engineDesc.name = "engine_pso";
    engineDesc.colorAttachments.push_back(vkm::VkmColorBlendAttachmentState{});
    engineDesc.colorAttachments[0].format = vkm::VkmFormat::BGRA8_UNORM;
    engineDesc.vertexShader = vkm::VkmShaderStageDescriptor{};
    engineDesc.vertexShader->filepath = "shared.vert";
    engineDesc.fragmentShader = vkm::VkmShaderStageDescriptor{};
    engineDesc.fragmentShader->filepath = "shared.frag";

    vkm::VkmPipelineStateDescriptor userDesc = engineDesc;
    userDesc.name = "user_pso";

    vkm::VkmPipelineStateManager manager(f.driver.get());
    std::string engineError, userError;
    REQUIRE_MESSAGE(manager.loadPipelineState(engineDesc, tempDir.string(), vkm::VkmPipelineStateOrigin::Engine, &engineError), engineError);
    REQUIRE_MESSAGE(manager.loadPipelineState(userDesc, tempDir.string(), vkm::VkmPipelineStateOrigin::User, &userError), userError);

    vkm::VkmPipelineStateBase* enginePso = manager.getPipelineState("engine_pso", vkm::VkmPipelineStateOrigin::Engine);
    vkm::VkmPipelineStateBase* userPso = manager.getPipelineState("user_pso", vkm::VkmPipelineStateOrigin::User);
    REQUIRE(enginePso != nullptr);
    REQUIRE(userPso != nullptr);
    CHECK(enginePso != userPso);

    // Each name only exists in its own origin's map.
    CHECK(manager.getPipelineState("engine_pso", vkm::VkmPipelineStateOrigin::User) == nullptr);
    CHECK(manager.getPipelineState("user_pso", vkm::VkmPipelineStateOrigin::Engine) == nullptr);

    // Single-arg overload checks Engine first, then User.
    CHECK(manager.getPipelineState("engine_pso") == enginePso);
    CHECK(manager.getPipelineState("user_pso") == userPso);

    fs::remove_all(tempDir, ec);
}

TEST_CASE("VkmPipelineStateManager - a later variant's collision leaves no earlier variant registered")
{
    VulkanDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    const fs::path tempDir = fs::temp_directory_path() / "vkm_test_pso_manager_partial_load";
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    fs::create_directories(tempDir, ec);
    REQUIRE_FALSE(ec);

    std::string parseError;
    std::optional<vkm::VkmPipelineStateDescriptor> desc = vkm::parsePipelineStateFromFile(
        std::string(RESOURCES_DIR) + "tests/pso_with_options.json", &parseError);
    REQUIRE_MESSAGE(desc.has_value(), parseError);

    writeStageCacheFiles(tempDir, "triangle", "default");
    writeStageCacheFiles(tempDir, "triangle", "wireframe");

    vkm::VkmPipelineStateManager manager(f.driver.get());

    // Pre-register the *later*-expanded variant's name ("triangle_pso[wireframe]") under
    // the opposite origin, so that loading `desc` under User expands "default" first
    // (inserted successfully pre-fix) and only then collides on "wireframe".
    vkm::VkmPipelineStateDescriptor collidingDesc{};
    collidingDesc.name = "triangle_pso[wireframe]";
    collidingDesc.colorAttachments.push_back(vkm::VkmColorBlendAttachmentState{});
    collidingDesc.colorAttachments[0].format = vkm::VkmFormat::BGRA8_UNORM;
    collidingDesc.vertexShader = vkm::VkmShaderStageDescriptor{};
    collidingDesc.vertexShader->filepath = "triangle.vert";
    collidingDesc.fragmentShader = vkm::VkmShaderStageDescriptor{};
    collidingDesc.fragmentShader->filepath = "triangle.frag";
    writeStageCacheFiles(tempDir, "triangle", "");
    std::string engineError;
    REQUIRE_MESSAGE(manager.loadPipelineState(collidingDesc, tempDir.string(), vkm::VkmPipelineStateOrigin::Engine, &engineError), engineError);

    std::string userError;
    const bool result = manager.loadPipelineState(*desc, tempDir.string(), vkm::VkmPipelineStateOrigin::User, &userError);
    CHECK_FALSE(result);
    CHECK_FALSE(userError.empty());

    // Neither variant must have been registered under User: the collision on the later
    // "wireframe" variant must not leave the earlier "default" variant partially loaded.
    CHECK(manager.getPipelineState("triangle_pso[default]", vkm::VkmPipelineStateOrigin::User) == nullptr);
    CHECK(manager.getPipelineState("triangle_pso[wireframe]", vkm::VkmPipelineStateOrigin::User) == nullptr);

    // The pre-registered Engine entry must be untouched.
    CHECK(manager.getPipelineState("triangle_pso[wireframe]", vkm::VkmPipelineStateOrigin::Engine) != nullptr);

    fs::remove_all(tempDir, ec);
}

TEST_CASE("VkmPipelineStateManager - loading a name that collides with the other origin fails")
{
    VulkanDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    const fs::path tempDir = fs::temp_directory_path() / "vkm_test_pso_manager_collision";
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    fs::create_directories(tempDir, ec);
    REQUIRE_FALSE(ec);

    writeStageCacheFiles(tempDir, "shared", "");

    vkm::VkmPipelineStateDescriptor desc{};
    desc.name = "shared_pso";
    desc.colorAttachments.push_back(vkm::VkmColorBlendAttachmentState{});
    desc.colorAttachments[0].format = vkm::VkmFormat::BGRA8_UNORM;
    desc.vertexShader = vkm::VkmShaderStageDescriptor{};
    desc.vertexShader->filepath = "shared.vert";
    desc.fragmentShader = vkm::VkmShaderStageDescriptor{};
    desc.fragmentShader->filepath = "shared.frag";

    vkm::VkmPipelineStateManager manager(f.driver.get());
    std::string engineError;
    REQUIRE_MESSAGE(manager.loadPipelineState(desc, tempDir.string(), vkm::VkmPipelineStateOrigin::Engine, &engineError), engineError);

    // Same name, opposite origin -- must fail because it already exists under Engine.
    std::string userError;
    const bool result = manager.loadPipelineState(desc, tempDir.string(), vkm::VkmPipelineStateOrigin::User, &userError);
    CHECK_FALSE(result);
    CHECK_FALSE(userError.empty());

    // The failed load must not have registered anything under User, and the existing
    // Engine entry must be untouched.
    CHECK(manager.getPipelineState("shared_pso", vkm::VkmPipelineStateOrigin::User) == nullptr);
    CHECK(manager.getPipelineState("shared_pso", vkm::VkmPipelineStateOrigin::Engine) != nullptr);

    fs::remove_all(tempDir, ec);
}

namespace
{
    // A PSO json equivalent to resources/tests/pso_with_options.json, with the default
    // option's fill mode and the option set parameterized so a reload test can rewrite it.
    std::string makeReloadablePsoJson(const std::string& defaultFillMode, bool includeWireframeOption)
    {
        std::string json =
            "{\n"
            "    \"name\": \"triangle_pso\",\n"
            "    \"rasterization_state\": { \"fill_mode\": \"" + defaultFillMode + "\", \"cull_mode\": \"back\" },\n"
            "    \"color_attachments\": [ { \"format\": \"bgra8_unorm\" } ],\n"
            "    \"shaders\": {\n"
            "        \"vertex\":   { \"filepath\": \"triangle.vert\" },\n"
            "        \"fragment\": { \"filepath\": \"triangle.frag\" }\n"
            "    },\n"
            "    \"options\": {\n"
            "        \"default\": {}";
        if (includeWireframeOption)
        {
            json +=
                ",\n"
                "        \"wireframe\": { \"rasterization_state\": { \"fill_mode\": \"wireframe\" } }";
        }
        json +=
            "\n    }\n"
            "}\n";
        return json;
    }

    void writeTextFile(const fs::path& path, const std::string& contents)
    {
        std::ofstream out(path, std::ios::trunc);
        REQUIRE(out.is_open());
        out << contents;
    }
} // namespace

TEST_CASE("VkmPipelineStateManager - reloading a source applies json edits in place")
{
    VulkanDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    const fs::path tempDir = fs::temp_directory_path() / "vkm_test_pso_reload";
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    fs::create_directories(tempDir, ec);
    REQUIRE_FALSE(ec);

    const fs::path jsonPath = tempDir / "reloadable.json";
    writeTextFile(jsonPath, makeReloadablePsoJson("solid", /*includeWireframeOption=*/true));
    writeStageCacheFiles(tempDir, "triangle", "default");
    writeStageCacheFiles(tempDir, "triangle", "wireframe");

    vkm::VkmPipelineStateManager manager(f.driver.get());
    std::string outError;
    REQUIRE_MESSAGE(manager.loadPipelineStatesFromDirectory(
        tempDir.string(), tempDir.string(), vkm::VkmPipelineStateOrigin::User, &outError), outError);

    REQUIRE(manager.getSources().size() == 1);
    CHECK(manager.getSources()[0].jsonPath == jsonPath.string());
    CHECK(manager.getSources()[0].shaderCacheDir == tempDir.string());
    CHECK(manager.getSources()[0].variantNames.size() == 2);
    CHECK_FALSE(manager.getSources()[0].stale);

    vkm::VkmPipelineStateBase* pso = manager.getPipelineState("triangle_pso[default]", vkm::VkmPipelineStateOrigin::User);
    REQUIRE(pso != nullptr);
    CHECK(pso->getDescriptor().rasterizationState.cullMode == vkm::VkmCullMode::Back);

    // Edit the render state and reload without a shader recompile (the .vfcache files on
    // disk are already what this descriptor needs).
    writeTextFile(jsonPath, makeReloadablePsoJson("solid", /*includeWireframeOption=*/true)
        .replace(makeReloadablePsoJson("solid", true).find("\"cull_mode\": \"back\""),
                 std::string("\"cull_mode\": \"back\"").size(), "\"cull_mode\": \"none\""));

    std::string reloadError;
    REQUIRE_MESSAGE(manager.reloadSource(0, /*recompileShaders=*/false, &reloadError), reloadError);

    // The pointer must survive: samples and render-graph callbacks cache it with no
    // invalidation hook.
    CHECK(manager.getPipelineState("triangle_pso[default]", vkm::VkmPipelineStateOrigin::User) == pso);
    CHECK(pso->getDescriptor().rasterizationState.cullMode == vkm::VkmCullMode::None);

    fs::remove_all(tempDir, ec);
}

TEST_CASE("VkmPipelineStateManager - a failed reload rolls back to the working pipeline")
{
    VulkanDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    const fs::path tempDir = fs::temp_directory_path() / "vkm_test_pso_reload_rollback";
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    fs::create_directories(tempDir, ec);
    REQUIRE_FALSE(ec);

    const fs::path jsonPath = tempDir / "reloadable.json";
    writeTextFile(jsonPath, makeReloadablePsoJson("solid", /*includeWireframeOption=*/false));
    writeStageCacheFiles(tempDir, "triangle", "default");

    vkm::VkmPipelineStateManager manager(f.driver.get());
    std::string outError;
    REQUIRE_MESSAGE(manager.loadPipelineStatesFromDirectory(
        tempDir.string(), tempDir.string(), vkm::VkmPipelineStateOrigin::User, &outError), outError);

    vkm::VkmPipelineStateBase* pso = manager.getPipelineState("triangle_pso[default]", vkm::VkmPipelineStateOrigin::User);
    REQUIRE(pso != nullptr);

    // An unparseable json must leave everything untouched -- nothing is destroyed until
    // every fallible step has passed.
    writeTextFile(jsonPath, "{ this is not valid json");
    std::string reloadError;
    CHECK_FALSE(manager.reloadSource(0, /*recompileShaders=*/false, &reloadError));
    CHECK_FALSE(reloadError.empty());
    CHECK(manager.getPipelineState("triangle_pso[default]", vkm::VkmPipelineStateOrigin::User) == pso);
    CHECK(pso->getDescriptor().rasterizationState.fillMode == vkm::VkmFillMode::Solid);

    // A parseable json whose shaders have no .vfcache on disk gets past parse/expand and
    // fails inside createInner -- the rollback must leave the pipeline usable.
    writeTextFile(jsonPath, makeReloadablePsoJson("solid", /*includeWireframeOption=*/false)
        .replace(makeReloadablePsoJson("solid", false).find("triangle.vert"),
                 std::string("triangle.vert").size(), "missing.vert"));
    reloadError.clear();
    CHECK_FALSE(manager.reloadSource(0, /*recompileShaders=*/false, &reloadError));
    CHECK_FALSE(reloadError.empty());
    CHECK(manager.getPipelineState("triangle_pso[default]", vkm::VkmPipelineStateOrigin::User) == pso);
    CHECK(pso->getDescriptor().vertexShader->filepath == "triangle.vert");

    // Restoring a good json must succeed, proving the rolled-back object is still live.
    writeTextFile(jsonPath, makeReloadablePsoJson("wireframe", /*includeWireframeOption=*/false));
    reloadError.clear();
    REQUIRE_MESSAGE(manager.reloadSource(0, /*recompileShaders=*/false, &reloadError), reloadError);
    CHECK(pso->getDescriptor().rasterizationState.fillMode == vkm::VkmFillMode::Wireframe);

    fs::remove_all(tempDir, ec);
}

TEST_CASE("VkmPipelineStateManager - reload adds new variants and keeps removed ones alive")
{
    VulkanDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    const fs::path tempDir = fs::temp_directory_path() / "vkm_test_pso_reload_variants";
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    fs::create_directories(tempDir, ec);
    REQUIRE_FALSE(ec);

    const fs::path jsonPath = tempDir / "reloadable.json";
    writeTextFile(jsonPath, makeReloadablePsoJson("solid", /*includeWireframeOption=*/false));
    writeStageCacheFiles(tempDir, "triangle", "default");
    writeStageCacheFiles(tempDir, "triangle", "wireframe");

    vkm::VkmPipelineStateManager manager(f.driver.get());
    std::string outError;
    REQUIRE_MESSAGE(manager.loadPipelineStatesFromDirectory(
        tempDir.string(), tempDir.string(), vkm::VkmPipelineStateOrigin::User, &outError), outError);
    CHECK(manager.getPipelineState("triangle_pso[wireframe]", vkm::VkmPipelineStateOrigin::User) == nullptr);

    vkm::VkmPipelineStateBase* defaultPso = manager.getPipelineState("triangle_pso[default]", vkm::VkmPipelineStateOrigin::User);
    REQUIRE(defaultPso != nullptr);

    // Adding an option registers the new variant without disturbing the existing one.
    writeTextFile(jsonPath, makeReloadablePsoJson("solid", /*includeWireframeOption=*/true));
    std::string reloadError;
    REQUIRE_MESSAGE(manager.reloadSource(0, /*recompileShaders=*/false, &reloadError), reloadError);
    CHECK(manager.getPipelineState("triangle_pso[default]", vkm::VkmPipelineStateOrigin::User) == defaultPso);
    vkm::VkmPipelineStateBase* wireframePso = manager.getPipelineState("triangle_pso[wireframe]", vkm::VkmPipelineStateOrigin::User);
    REQUIRE(wireframePso != nullptr);
    CHECK(manager.getSources()[0].variantNames.size() == 2);

    // Removing it again keeps the pipeline registered -- callers hold raw pointers to it --
    // and reports that as a warning.
    writeTextFile(jsonPath, makeReloadablePsoJson("solid", /*includeWireframeOption=*/false));
    reloadError.clear();
    REQUIRE_MESSAGE(manager.reloadSource(0, /*recompileShaders=*/false, &reloadError), reloadError);
    CHECK(manager.getPipelineState("triangle_pso[wireframe]", vkm::VkmPipelineStateOrigin::User) == wireframePso);
    CHECK(reloadError.find("wireframe") != std::string::npos);

    fs::remove_all(tempDir, ec);
}

TEST_CASE("VkmPipelineStateManager - staleness tracks the json and its shader sources")
{
    VulkanDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    const fs::path tempDir = fs::temp_directory_path() / "vkm_test_pso_staleness";
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    fs::create_directories(tempDir, ec);
    REQUIRE_FALSE(ec);

    const fs::path jsonPath = tempDir / "reloadable.json";
    writeTextFile(jsonPath, makeReloadablePsoJson("solid", /*includeWireframeOption=*/false));
    // The shader sources the json names must exist to be watched at all.
    writeTextFile(tempDir / "triangle.vert", "// placeholder\n");
    writeTextFile(tempDir / "triangle.frag", "// placeholder\n");
    writeStageCacheFiles(tempDir, "triangle", "default");

    vkm::VkmPipelineStateManager manager(f.driver.get());
    std::string outError;
    REQUIRE_MESSAGE(manager.loadPipelineStatesFromDirectory(
        tempDir.string(), tempDir.string(), vkm::VkmPipelineStateOrigin::User, &outError), outError);

    REQUIRE(manager.getSources().size() == 1);
    // json + both shader sources (plus any shared *.hlsli the engine ships).
    CHECK(manager.getSources()[0].watchedFiles.size() >= 3);
    CHECK_FALSE(manager.refreshStaleness());
    CHECK_FALSE(manager.getSources()[0].stale);

    // Touching a shader source, not the json, must still mark the source stale.
    fs::last_write_time(tempDir / "triangle.frag",
        fs::last_write_time(tempDir / "triangle.frag") + std::chrono::seconds(10), ec);
    REQUIRE_FALSE(ec);
    CHECK(manager.refreshStaleness());
    CHECK(manager.getSources()[0].stale);

    // Reloading re-stamps the watched files and clears the flag.
    std::string reloadError;
    REQUIRE_MESSAGE(manager.reloadSource(0, /*recompileShaders=*/false, &reloadError), reloadError);
    CHECK_FALSE(manager.getSources()[0].stale);
    CHECK_FALSE(manager.refreshStaleness());

    fs::remove_all(tempDir, ec);
}

TEST_CASE("VkmPipelineStateManager - a descriptor-registered pipeline reports as non-reloadable")
{
    VulkanDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    const fs::path tempDir = fs::temp_directory_path() / "vkm_test_pso_no_json";
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    fs::create_directories(tempDir, ec);
    REQUIRE_FALSE(ec);
    writeStageCacheFiles(tempDir, "shared", "");

    vkm::VkmPipelineStateDescriptor desc{};
    desc.name = "descriptor_only_pso";
    desc.colorAttachments.push_back({});
    desc.colorAttachments[0].format = vkm::VkmFormat::BGRA8_UNORM;
    desc.vertexShader = vkm::VkmShaderStageDescriptor{};
    desc.vertexShader->filepath = "shared.vert";
    desc.fragmentShader = vkm::VkmShaderStageDescriptor{};
    desc.fragmentShader->filepath = "shared.frag";

    vkm::VkmPipelineStateManager manager(f.driver.get());
    std::string outError;
    REQUIRE_MESSAGE(manager.loadPipelineState(desc, tempDir.string(), vkm::VkmPipelineStateOrigin::User, &outError), outError);

    REQUIRE(manager.getSources().size() == 1);
    CHECK(manager.getSources()[0].jsonPath.empty());
    CHECK(manager.getSources()[0].watchedFiles.empty());
    CHECK(manager.findSourceIndexOfPipelineState("descriptor_only_pso") == 0);
    CHECK(manager.findSourceIndexOfPipelineState("no_such_pso") == std::string::npos);

    std::string reloadError;
    CHECK_FALSE(manager.reloadSource(0, /*recompileShaders=*/false, &reloadError));
    CHECK(reloadError.find("cannot be reloaded") != std::string::npos);

    fs::remove_all(tempDir, ec);
}

TEST_CASE("VkmPipelineStateManager - reloading with recompile runs vkm-compiler on the real shader")
{
    VulkanDriverFixture f;
    VKM_REQUIRE_DEVICE(f.initResult);

    // Only a build that baked in a vkm-compiler path can recompile at run time (an installed
    // or Emscripten build cannot); there is nothing to test otherwise.
    if (!vkm::VkmPipelineStateManager::isShaderRecompilationAvailable())
    {
        MESSAGE("Skipping: this build has no vkm-compiler path baked in");
        return;
    }

    const fs::path tempDir = fs::temp_directory_path() / "vkm_test_pso_recompile";
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    fs::create_directories(tempDir, ec);
    REQUIRE_FALSE(ec);

    // A real HLSL source (the triangle sample's), with an explicit color format -- the
    // sample's own "swapchain" sentinel has nothing to resolve against on a headless driver.
    fs::copy_file(fs::path(TEST_TRIANGLE_SAMPLE_DIR) / "triangle.hlsl",
        tempDir / "triangle.hlsl", fs::copy_options::overwrite_existing, ec);
    REQUIRE_FALSE(ec);

    const fs::path jsonPath = tempDir / "recompile.json";
    writeTextFile(jsonPath,
        "{\n"
        "    \"name\": \"recompile_pso\",\n"
        "    \"color_attachments\": [ { \"format\": \"bgra8_unorm\" } ],\n"
        "    \"shaders\": {\n"
        "        \"vertex\":   { \"filepath\": \"triangle.hlsl\", \"entry_point\": \"VSMain\" },\n"
        "        \"fragment\": { \"filepath\": \"triangle.hlsl\", \"entry_point\": \"PSMain\" }\n"
        "    }\n"
        "}\n");

    // Placeholder caches so the initial load succeeds; the reload replaces them with what
    // vkm-compiler actually produces from triangle.hlsl.
    writeStageCacheFiles(tempDir, "triangle", "");
    const fs::path vertCache = tempDir / vkm::buildShaderCacheFilename(
        "triangle", "", vkm::VkmShaderCacheStage::Vertex, vkm::VkmShaderCacheBackend::Vulkan);
    const uintmax_t placeholderSize = fs::file_size(vertCache, ec);
    REQUIRE_FALSE(ec);

    vkm::VkmPipelineStateManager manager(f.driver.get());
    std::string outError;
    REQUIRE_MESSAGE(manager.loadPipelineStatesFromDirectory(
        tempDir.string(), tempDir.string(), vkm::VkmPipelineStateOrigin::User, &outError), outError);
    vkm::VkmPipelineStateBase* pso = manager.getPipelineState("recompile_pso", vkm::VkmPipelineStateOrigin::User);
    REQUIRE(pso != nullptr);

    std::string reloadError;
    REQUIRE_MESSAGE(manager.reloadSource(0, /*recompileShaders=*/true, &reloadError), reloadError);

    // The compiler really ran: the cache is not the hand-written placeholder any more, and the
    // pipeline was rebuilt in place from it.
    CHECK(fs::file_size(vertCache, ec) != placeholderSize);
    CHECK(manager.getPipelineState("recompile_pso", vkm::VkmPipelineStateOrigin::User) == pso);

    // A shader that does not compile must fail the reload with the compiler's own output,
    // and must leave the working pipeline in place.
    writeTextFile(tempDir / "triangle.hlsl", "this is not valid HLSL\n");
    reloadError.clear();
    CHECK_FALSE(manager.reloadSource(0, /*recompileShaders=*/true, &reloadError));
    CHECK(reloadError.find("vkm-compiler failed") != std::string::npos);
    CHECK(manager.getPipelineState("recompile_pso", vkm::VkmPipelineStateOrigin::User) == pso);

    fs::remove_all(tempDir, ec);
}

#endif // VKM_USE_VULKAN_API
