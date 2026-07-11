#include <doctest/doctest.h>

#include <vkm/renderer/backend/common/shader_cache.h>
#include <vkm/renderer/backend/common/shader_cache_loader.h>
#include <vkm/renderer/backend/common/shader_cache_util.h>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace
{
// Mirrors vkm-compiler's writeCacheFile, but with individually overridable header
// fields so tests can construct both valid files and deliberately-corrupt ones.
// `bytesToWrite` defaults to content.size(); passing a smaller value simulates a
// file truncated mid-content (contentSize in the header still claims the full size).
bool writeCacheFileRaw(const fs::path& path,
                       uint32_t magic,
                       uint32_t version,
                       vkm::VkmShaderCacheBackend backend,
                       vkm::VkmShaderCacheStage stage,
                       vkm::VkmShaderCacheContentFormat format,
                       const std::string& entryPoint,
                       const std::vector<uint8_t>& content,
                       size_t bytesToWrite)
{
    vkm::VkmShaderCacheHeader header{};
    header.magic = magic;
    header.version = version;
    header.backend = backend;
    header.stage = stage;
    header.contentFormat = format;
    std::strncpy(header.entryPoint, entryPoint.c_str(), sizeof(header.entryPoint) - 1);
    header.contentSize = static_cast<uint64_t>(content.size());

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
    {
        return false;
    }
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    if (bytesToWrite > 0)
    {
        out.write(reinterpret_cast<const char*>(content.data()),
                  static_cast<std::streamsize>(bytesToWrite));
    }
    return static_cast<bool>(out);
}

bool writeValidCacheFile(const fs::path& path,
                         vkm::VkmShaderCacheBackend backend,
                         vkm::VkmShaderCacheStage stage,
                         vkm::VkmShaderCacheContentFormat format,
                         const std::string& entryPoint,
                         const std::vector<uint8_t>& content)
{
    return writeCacheFileRaw(path, vkm::kVkmShaderCacheMagic, vkm::kVkmShaderCacheVersion,
                             backend, stage, format, entryPoint, content, content.size());
}
} // namespace

TEST_CASE("buildShaderCacheFilename - no option matches the legacy format") {
    CHECK(vkm::buildShaderCacheFilename("triangle", "", vkm::VkmShaderCacheStage::Vertex,
                                        vkm::VkmShaderCacheBackend::Vulkan)
          == "triangle.vert.vulkan.vfcache");
    CHECK(vkm::buildShaderCacheFilename("triangle", "", vkm::VkmShaderCacheStage::Fragment,
                                        vkm::VkmShaderCacheBackend::Metal)
          == "triangle.frag.metal.vfcache");
    CHECK(vkm::buildShaderCacheFilename("particles", "", vkm::VkmShaderCacheStage::Compute,
                                        vkm::VkmShaderCacheBackend::WebGPU)
          == "particles.comp.webgpu.vfcache");
}

TEST_CASE("buildShaderCacheFilename - non-empty option inserts the [option] suffix") {
    CHECK(vkm::buildShaderCacheFilename("triangle", "wireframe", vkm::VkmShaderCacheStage::Vertex,
                                        vkm::VkmShaderCacheBackend::Vulkan)
          == "triangle[wireframe].vert.vulkan.vfcache");
    CHECK(vkm::buildShaderCacheFilename("triangle", "default", vkm::VkmShaderCacheStage::Fragment,
                                        vkm::VkmShaderCacheBackend::Metal)
          == "triangle[default].frag.metal.vfcache");
}

TEST_CASE("loadShaderCacheFile - round-trips a valid file") {
    const fs::path path = fs::temp_directory_path() / "vkm_test_valid.vfcache";
    const std::vector<uint8_t> content = {0x01, 0x02, 0x03, 0x04, 0xAA, 0xBB};
    REQUIRE(writeValidCacheFile(path, vkm::VkmShaderCacheBackend::Metal,
                                vkm::VkmShaderCacheStage::Fragment,
                                vkm::VkmShaderCacheContentFormat::Msl, "PSMain", content));

    std::string outError;
    std::optional<vkm::VkmLoadedShaderCache> loaded =
        vkm::loadShaderCacheFile(path.string(), vkm::VkmShaderCacheBackend::Metal, &outError);
    REQUIRE(loaded.has_value());
    CHECK(loaded->stage == vkm::VkmShaderCacheStage::Fragment);
    CHECK(loaded->contentFormat == vkm::VkmShaderCacheContentFormat::Msl);
    CHECK(loaded->entryPoint == "PSMain");
    CHECK(loaded->content == content);

    fs::remove(path);
}

TEST_CASE("loadShaderCacheFile - round-trips empty content") {
    const fs::path path = fs::temp_directory_path() / "vkm_test_empty.vfcache";
    const std::vector<uint8_t> content;
    REQUIRE(writeValidCacheFile(path, vkm::VkmShaderCacheBackend::Vulkan,
                                vkm::VkmShaderCacheStage::Vertex,
                                vkm::VkmShaderCacheContentFormat::SpirV, "main", content));

    std::optional<vkm::VkmLoadedShaderCache> loaded =
        vkm::loadShaderCacheFile(path.string(), vkm::VkmShaderCacheBackend::Vulkan);
    REQUIRE(loaded.has_value());
    CHECK(loaded->entryPoint == "main");
    CHECK(loaded->content.empty());

    fs::remove(path);
}

TEST_CASE("loadShaderCacheFile - fails on bad magic") {
    const fs::path path = fs::temp_directory_path() / "vkm_test_bad_magic.vfcache";
    const std::vector<uint8_t> content = {0x01, 0x02};
    REQUIRE(writeCacheFileRaw(path, 0xDEADBEEFu, vkm::kVkmShaderCacheVersion,
                              vkm::VkmShaderCacheBackend::Vulkan, vkm::VkmShaderCacheStage::Vertex,
                              vkm::VkmShaderCacheContentFormat::SpirV, "main", content, content.size()));

    std::string outError;
    std::optional<vkm::VkmLoadedShaderCache> loaded =
        vkm::loadShaderCacheFile(path.string(), vkm::VkmShaderCacheBackend::Vulkan, &outError);
    CHECK_FALSE(loaded.has_value());
    CHECK(outError.find("magic") != std::string::npos);

    fs::remove(path);
}

TEST_CASE("loadShaderCacheFile - fails on version mismatch") {
    const fs::path path = fs::temp_directory_path() / "vkm_test_bad_version.vfcache";
    const std::vector<uint8_t> content = {0x01, 0x02};
    REQUIRE(writeCacheFileRaw(path, vkm::kVkmShaderCacheMagic, vkm::kVkmShaderCacheVersion + 1u,
                              vkm::VkmShaderCacheBackend::Vulkan, vkm::VkmShaderCacheStage::Vertex,
                              vkm::VkmShaderCacheContentFormat::SpirV, "main", content, content.size()));

    std::string outError;
    std::optional<vkm::VkmLoadedShaderCache> loaded =
        vkm::loadShaderCacheFile(path.string(), vkm::VkmShaderCacheBackend::Vulkan, &outError);
    CHECK_FALSE(loaded.has_value());
    CHECK(outError.find("version") != std::string::npos);

    fs::remove(path);
}

TEST_CASE("loadShaderCacheFile - fails on backend mismatch") {
    const fs::path path = fs::temp_directory_path() / "vkm_test_bad_backend.vfcache";
    const std::vector<uint8_t> content = {0x01, 0x02};
    REQUIRE(writeValidCacheFile(path, vkm::VkmShaderCacheBackend::Vulkan,
                                vkm::VkmShaderCacheStage::Vertex,
                                vkm::VkmShaderCacheContentFormat::SpirV, "main", content));

    std::string outError;
    std::optional<vkm::VkmLoadedShaderCache> loaded =
        vkm::loadShaderCacheFile(path.string(), vkm::VkmShaderCacheBackend::Metal, &outError);
    CHECK_FALSE(loaded.has_value());
    CHECK(outError.find("backend") != std::string::npos);

    fs::remove(path);
}

TEST_CASE("loadShaderCacheFile - fails on truncated content") {
    const fs::path path = fs::temp_directory_path() / "vkm_test_truncated.vfcache";
    const std::vector<uint8_t> content = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06};
    // Header claims 6 content bytes but only 2 are actually written.
    REQUIRE(writeCacheFileRaw(path, vkm::kVkmShaderCacheMagic, vkm::kVkmShaderCacheVersion,
                              vkm::VkmShaderCacheBackend::Vulkan, vkm::VkmShaderCacheStage::Vertex,
                              vkm::VkmShaderCacheContentFormat::SpirV, "main", content, 2));

    std::string outError;
    std::optional<vkm::VkmLoadedShaderCache> loaded =
        vkm::loadShaderCacheFile(path.string(), vkm::VkmShaderCacheBackend::Vulkan, &outError);
    CHECK_FALSE(loaded.has_value());
    CHECK(outError.find("truncated") != std::string::npos);

    fs::remove(path);
}

TEST_CASE("loadShaderCacheFile - fails on nonexistent file") {
    std::string outError;
    std::optional<vkm::VkmLoadedShaderCache> loaded = vkm::loadShaderCacheFile(
        "/no/such/vfcache_file_that_does_not_exist_98765.vfcache",
        vkm::VkmShaderCacheBackend::Vulkan, &outError);
    CHECK_FALSE(loaded.has_value());
    CHECK_FALSE(outError.empty());
}
