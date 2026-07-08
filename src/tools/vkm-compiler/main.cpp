// Copyright (c) 2025 Snowapril
//
// vkm-compiler: build-time HLSL shader compiler.
//
// Given a PSO renderpass JSON (parsed with the same parser vkmcore uses), it
// compiles every shader stage referenced by that PSO from a single HLSL source
// to the active backend's binary/text form, wrapping each result in a
// VkmShaderCacheHeader:
//   - vulkan: dxc HLSL -> SPIR-V (stored as-is)
//   - metal:  dxc HLSL -> SPIR-V -> spirv-cross -> MSL text
//   - webgpu: dxc HLSL -> SPIR-V -> tint -> WGSL text (requires WGSL support)

#include <cxxopts.hpp>
#include <spirv_msl.hpp>

#include <vkm/renderer/backend/common/pipeline_state_parser.h>
#include <vkm/renderer/backend/common/shader_cache.h>

#include "subprocess.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace vkm;

namespace
{
    struct StageInfo
    {
        VkmShaderCacheStage stage;
        const char* shortName; // file-name component, e.g. "vert"
        const char* profile;   // dxc target profile, e.g. "vs_6_0"
    };

    const char* backendDefault()
    {
#if defined(VKM_USE_VULKAN_API)
        return "vulkan";
#elif defined(VKM_USE_METAL_API)
        return "metal";
#elif defined(VKM_USE_WEBGPU_API)
        return "webgpu";
#else
        return "vulkan";
#endif
    }

    bool backendToEnum(const std::string& name, VkmShaderCacheBackend& out)
    {
        if (name == "vulkan") { out = VkmShaderCacheBackend::Vulkan; return true; }
        if (name == "metal")  { out = VkmShaderCacheBackend::Metal;  return true; }
        if (name == "webgpu") { out = VkmShaderCacheBackend::WebGPU; return true; }
        return false;
    }

    std::vector<uint8_t> readFileBytes(const fs::path& path)
    {
        std::ifstream file(path, std::ios::binary | std::ios::ate);
        if (!file)
        {
            return {};
        }
        const std::streamsize size = file.tellg();
        file.seekg(0, std::ios::beg);
        std::vector<uint8_t> bytes(static_cast<size_t>(size));
        if (size > 0)
        {
            file.read(reinterpret_cast<char*>(bytes.data()), size);
        }
        return bytes;
    }

    bool writeCacheFile(const fs::path& path,
                        VkmShaderCacheBackend backend,
                        const StageInfo& info,
                        VkmShaderCacheContentFormat format,
                        const std::string& entryPoint,
                        const std::vector<uint8_t>& content)
    {
        VkmShaderCacheHeader header{};
        header.magic = kVkmShaderCacheMagic;
        header.version = kVkmShaderCacheVersion;
        header.backend = backend;
        header.stage = info.stage;
        header.contentFormat = format;
        std::strncpy(header.entryPoint, entryPoint.c_str(), sizeof(header.entryPoint) - 1);
        header.contentSize = static_cast<uint64_t>(content.size());

        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            std::cerr << "vkm-compiler: cannot open output file " << path << "\n";
            return false;
        }
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        if (!content.empty())
        {
            out.write(reinterpret_cast<const char*>(content.data()),
                      static_cast<std::streamsize>(content.size()));
        }
        return static_cast<bool>(out);
    }

    // Runs dxc to compile `source` to SPIR-V at `spvOut`. Returns false and
    // prints dxc's captured output on failure (shader errors are build errors).
    bool compileToSpirv(const VkmShaderStageDescriptor& desc,
                        const StageInfo& info,
                        const fs::path& source,
                        const fs::path& spvOut)
    {
        std::vector<std::string> args = {
            "-spirv",
            "-T", info.profile,
            "-E", desc.entryPoint,
        };
        for (const auto& [name, value] : desc.definitions)
        {
            args.push_back("-D");
            args.push_back(value.empty() ? name : name + "=" + value);
        }
        args.push_back("-Fo");
        args.push_back(spvOut.string());
        args.push_back(source.string());

        const SubprocessResult result = runSubprocess(VKM_DXC_EXECUTABLE_PATH, args);
        if (result.exitCode != 0)
        {
            std::cerr << "vkm-compiler: dxc failed for " << source.filename().string()
                      << " (" << info.shortName << ", entry '" << desc.entryPoint
                      << "'):\n" << result.output << "\n";
            return false;
        }
        return true;
    }

    std::vector<uint32_t> toSpirvWords(const std::vector<uint8_t>& bytes)
    {
        std::vector<uint32_t> words(bytes.size() / sizeof(uint32_t));
        if (!words.empty())
        {
            std::memcpy(words.data(), bytes.data(), words.size() * sizeof(uint32_t));
        }
        return words;
    }

    bool compileStage(const VkmShaderStageDescriptor& desc,
                      const StageInfo& info,
                      const std::string& backendName,
                      VkmShaderCacheBackend backend,
                      const fs::path& shaderRoot,
                      const fs::path& outputDir)
    {
        const fs::path source = shaderRoot / desc.filepath;
        if (!fs::exists(source))
        {
            std::cerr << "vkm-compiler: shader source not found: " << source << "\n";
            return false;
        }

        const std::string baseName = source.stem().string();
        const fs::path spvTmp =
            outputDir / (baseName + "." + info.shortName + ".tmp.spv");

        if (!compileToSpirv(desc, info, source, spvTmp))
        {
            return false;
        }

        const std::vector<uint8_t> spirv = readFileBytes(spvTmp);
        if (spirv.empty())
        {
            std::cerr << "vkm-compiler: dxc produced empty SPIR-V for "
                      << source.filename().string() << "\n";
            fs::remove(spvTmp);
            return false;
        }

        std::vector<uint8_t> content;
        VkmShaderCacheContentFormat format = VkmShaderCacheContentFormat::SpirV;

        bool ok = true;
        if (backend == VkmShaderCacheBackend::Vulkan)
        {
            content = spirv;
            format = VkmShaderCacheContentFormat::SpirV;
        }
        else if (backend == VkmShaderCacheBackend::Metal)
        {
            try
            {
                spirv_cross::CompilerMSL compiler(toSpirvWords(spirv));
                const std::string msl = compiler.compile();
                content.assign(msl.begin(), msl.end());
                format = VkmShaderCacheContentFormat::Msl;
            }
            catch (const std::exception& e)
            {
                std::cerr << "vkm-compiler: spirv-cross MSL generation failed for "
                          << source.filename().string() << ": " << e.what() << "\n";
                ok = false;
            }
        }
        else // WebGPU
        {
#if defined(VKM_COMPILER_SUPPORTS_WGSL)
            const fs::path wgslTmp =
                outputDir / (baseName + "." + info.shortName + ".tmp.wgsl");
            const SubprocessResult tintResult = runSubprocess(
                VKM_TINT_EXECUTABLE_PATH,
                {"--format", "wgsl", "--input-format", "spirv",
                 "-o", wgslTmp.string(), spvTmp.string()});
            if (tintResult.exitCode != 0)
            {
                std::cerr << "vkm-compiler: tint failed for "
                          << source.filename().string() << ":\n"
                          << tintResult.output << "\n";
                ok = false;
            }
            else
            {
                content = readFileBytes(wgslTmp);
                format = VkmShaderCacheContentFormat::Wgsl;
            }
            fs::remove(wgslTmp);
#else
            std::cerr << "vkm-compiler: WGSL output requested but this build was "
                         "compiled without WGSL support (VKM_COMPILER_ENABLE_WGSL=OFF)\n";
            ok = false;
#endif
        }

        fs::remove(spvTmp);
        if (!ok)
        {
            return false;
        }

        const fs::path outPath =
            outputDir / (baseName + "." + info.shortName + "." + backendName + ".vfcache");
        if (!writeCacheFile(outPath, backend, info, format, desc.entryPoint, content))
        {
            return false;
        }

        std::cout << "vkm-compiler: wrote " << outPath.filename().string()
                  << " (" << content.size() << " bytes)\n";
        return true;
    }
} // namespace

int main(int argc, char** argv)
{
    cxxopts::Options options("vkm-compiler",
                             "Compile HLSL shaders referenced by a vkm PSO JSON.");
    options.add_options()
        ("pso", "Path to the renderpass/PSO JSON", cxxopts::value<std::string>())
        ("output-dir", "Directory for generated .vfcache files",
         cxxopts::value<std::string>())
        ("backend", "Target backend: vulkan|metal|webgpu",
         cxxopts::value<std::string>()->default_value(backendDefault()))
        ("shader-root", "Root directory for resolving shader filepaths "
                        "(default: directory containing --pso)",
         cxxopts::value<std::string>()->default_value(""))
        ("h,help", "Print usage");

    cxxopts::ParseResult parsed;
    try
    {
        parsed = options.parse(argc, argv);
    }
    catch (const std::exception& e)
    {
        std::cerr << "vkm-compiler: " << e.what() << "\n";
        return 1;
    }

    if (parsed.count("help") || !parsed.count("pso") || !parsed.count("output-dir"))
    {
        std::cout << options.help() << "\n";
        return parsed.count("help") ? 0 : 1;
    }

    const fs::path psoPath = parsed["pso"].as<std::string>();
    const fs::path outputDir = parsed["output-dir"].as<std::string>();
    const std::string backendName = parsed["backend"].as<std::string>();

    VkmShaderCacheBackend backend{};
    if (!backendToEnum(backendName, backend))
    {
        std::cerr << "vkm-compiler: unknown backend '" << backendName << "'\n";
        return 1;
    }

    fs::path shaderRoot = parsed["shader-root"].as<std::string>();
    if (shaderRoot.empty())
    {
        shaderRoot = psoPath.parent_path();
    }

    std::string parseError;
    std::optional<VkmPipelineStateDescriptor> pso =
        parsePipelineStateFromFile(psoPath.string(), &parseError);
    if (!pso.has_value())
    {
        std::cerr << "vkm-compiler: failed to parse PSO " << psoPath
                  << ": " << parseError << "\n";
        return 1;
    }

    std::error_code ec;
    fs::create_directories(outputDir, ec);
    if (ec)
    {
        std::cerr << "vkm-compiler: cannot create output dir " << outputDir
                  << ": " << ec.message() << "\n";
        return 1;
    }

    const StageInfo vertexInfo{VkmShaderCacheStage::Vertex, "vert", "vs_6_0"};
    const StageInfo fragmentInfo{VkmShaderCacheStage::Fragment, "frag", "ps_6_0"};
    const StageInfo computeInfo{VkmShaderCacheStage::Compute, "comp", "cs_6_0"};

    bool allOk = true;
    if (pso->vertexShader.has_value())
    {
        allOk &= compileStage(*pso->vertexShader, vertexInfo, backendName, backend,
                              shaderRoot, outputDir);
    }
    if (pso->fragmentShader.has_value())
    {
        allOk &= compileStage(*pso->fragmentShader, fragmentInfo, backendName, backend,
                              shaderRoot, outputDir);
    }
    if (pso->computeShader.has_value())
    {
        allOk &= compileStage(*pso->computeShader, computeInfo, backendName, backend,
                              shaderRoot, outputDir);
    }

    return allOk ? 0 : 1;
}
