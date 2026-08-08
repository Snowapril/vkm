// Copyright (c) 2025 Snowapril
//
// vkm-compiler: build-time HLSL shader compiler.
//
// Given a PSO renderpass JSON (parsed with the same parser vkmcore uses), it
// compiles every shader stage referenced by that PSO from a single HLSL source
// to the active backend's binary/text form, wrapping each result in a
// VkmShaderCacheHeader:
//   - vulkan: dxc HLSL -> SPIR-V (stored as-is)
//   - metal:  dxc HLSL -> SPIR-V -> spirv-cross -> MSL -> `metal` -> metallib
//             (falls back to storing MSL text when no Metal toolchain is available)
//   - webgpu: dxc HLSL -> SPIR-V -> tint -> WGSL text (requires WGSL support)
//
// If the PSO JSON declares an "options" node, it is expanded via
// expandPipelineStateOptions() into one variant per option; every variant's
// present stages are compiled with that variant's merged definitions and its
// .vfcache files are named with the option suffix (see buildShaderCacheFilename).

#include <cxxopts.hpp>
#include <spirv_msl.hpp>

#include <vkm/renderer/backend/common/bindless_resource_manager.h>
#include <vkm/renderer/backend/common/frame_constants.h>
#include <vkm/renderer/backend/common/pipeline_state_parser.h>
#include <vkm/renderer/backend/common/shader_cache.h>
#include <vkm/renderer/backend/common/shader_cache_util.h>

#include <vkm/base/subprocess.h>

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
        // Profile used instead when the stage sets VkmShaderStageDescriptor::requiresRayQuery.
        // `RayQuery<>` is a shader-model-6.5 feature, and the baseline profile stays at 6.0 so
        // one shader opting into ray tracing does not raise the requirement for every shader.
        // Null for stages that cannot carry a ray query (everything but compute).
        const char* rayQueryProfile;
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
                        const uint32_t (&threadGroupSize)[3],
                        const std::vector<uint8_t>& content)
    {
        VkmShaderCacheHeader header{};
        header.magic = kVkmShaderCacheMagic;
        header.version = kVkmShaderCacheVersion;
        header.backend = backend;
        header.stage = info.stage;
        header.contentFormat = format;
        std::strncpy(header.entryPoint, entryPoint.c_str(), sizeof(header.entryPoint) - 1);
        header.threadGroupSize[0] = threadGroupSize[0];
        header.threadGroupSize[1] = threadGroupSize[1];
        header.threadGroupSize[2] = threadGroupSize[2];
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

    const char* backendDefine(VkmShaderCacheBackend backend)
    {
        switch (backend)
        {
            case VkmShaderCacheBackend::Vulkan: return "VKM_BACKEND_VULKAN";
            case VkmShaderCacheBackend::Metal:  return "VKM_BACKEND_METAL";
            case VkmShaderCacheBackend::WebGPU: return "VKM_BACKEND_WEBGPU";
        }
        return "VKM_BACKEND_VULKAN";
    }

    // Runs dxc to compile `source` to SPIR-V at `spvOut`. Returns false and
    // prints dxc's captured output on failure (shader errors are build errors).
    bool compileToSpirv(const VkmShaderStageDescriptor& desc,
                        const StageInfo& info,
                        VkmShaderCacheBackend backend,
                        const fs::path& source,
                        const fs::path& includeDir,
                        const fs::path& spvOut)
    {
        if (desc.requiresRayQuery && info.rayQueryProfile == nullptr)
        {
            std::cerr << "vkm-compiler: " << source.filename().string() << " (" << info.shortName
                      << ") sets ray_query, but only the compute stage can carry an inline ray"
                         " query\n";
            return false;
        }

        std::vector<std::string> args = {
            "-spirv",
            "-T", desc.requiresRayQuery ? info.rayQueryProfile : info.profile,
            "-E", desc.entryPoint,
            "-D", backendDefine(backend),
            "-D", "VKM_BINDLESS_BUFFER_CAPACITY=" + std::to_string(kVkmBindlessBufferCapacity),
        };
        if (desc.requiresRayQuery)
        {
            // Raises the emitted module from SPIR-V 1.0, dxc's default, to 1.5 -- the version
            // Vulkan 1.2 mandates and the one the ray-tracing extensions are written against. No
            // driver exposing rayQuery predates Vulkan 1.2, so emitting 1.0 buys nothing.
            //
            // No -fspv-extension: that flag is a whitelist, and naming SPV_KHR_ray_query alone
            // makes dxc reject the engine's bindless unsized descriptor arrays. dxc emits both
            // extensions on its own.
            //
            // Both targets take this path: the Metal target compiles its own SPIR-V and then feeds
            // spirv-cross, which lowers OpRayQuery* to metal::raytracing intersection queries.
            args.push_back("-fspv-target-env=vulkan1.2");
        }
        if (!includeDir.empty())
        {
            // Where the engine's shared .hlsli headers (include/vkm/shaders) are found.
            // Without this dxc only resolves #include relative to the including file.
            args.push_back("-I");
            args.push_back(includeDir.string());
        }
        if (backend == VkmShaderCacheBackend::Vulkan && info.stage == VkmShaderCacheStage::Vertex)
        {
            // Negate SV_Position.y so the engine's +Y-up clip space (the HLSL/D3D convention,
            // native to Metal and WebGPU) renders right-side-up on Vulkan's +Y-down NDC. This
            // is the engine's single Y-flip site for Vulkan; the command buffer uses a plain
            // positive-height viewport. The flip reverses screen-space winding, which
            // toVkFrontFace() in vulkan_pipeline_state.cpp compensates for. Vulkan only: the
            // Metal/WebGPU targets compile their own SPIR-V and are already +Y up. DXC rejects
            // -fvk-invert-y on non-position-writing stages, so it is gated to the vertex stage
            // (the engine has no DS/GS/mesh shaders that also write SV_Position).
            args.push_back("-fvk-invert-y");
        }
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

    /*
    * @brief Reads a compute shader's declared [numthreads(x, y, z)] out of the compiled SPIR-V.
    *
    * Taking it from the module rather than from the PSO JSON keeps the shader the single source
    * of truth: Metal must be told the threadgroup size at dispatch time, and a hand-maintained
    * second copy that drifts from the shader would dispatch the wrong thread count silently.
    * Returns false if the module declares no local size, which is a build error for a compute
    * stage rather than something to paper over with a default.
    */
    bool readComputeThreadGroupSize(const std::vector<uint8_t>& spirv,
                                    const std::string& entryPoint,
                                    uint32_t (&out)[3])
    {
        try
        {
            const spirv_cross::Compiler reflect(toSpirvWords(spirv));
            const spirv_cross::SPIREntryPoint& entry =
                reflect.get_entry_point(entryPoint, spv::ExecutionModelGLCompute);
            if (entry.workgroup_size.x == 0)
            {
                return false;
            }
            out[0] = entry.workgroup_size.x;
            out[1] = entry.workgroup_size.y;
            out[2] = entry.workgroup_size.z;
            return true;
        }
        catch (const std::exception&)
        {
            return false;
        }
    }

#if defined(VKM_METAL_TOOLS_AVAILABLE)
    // Compiles spirv-cross's MSL text into a metallib binary via the Metal toolchain
    // (`xcrun -sdk macosx metal ... -o out.metallib`). The metallib is what gets stored
    // in the .vfcache (VkmShaderCacheContentFormat::MetalLib) and loaded at runtime with
    // -[MTLDevice newLibraryWithData:], so captured .gputrace frames replay without a
    // source recompile.
    //
    // When `emitMsl` is set, the intermediate `.metal` source is kept next to the
    // .vfcache and `-frecord-sources` embeds it in the metallib so Xcode's shader
    // debugger shows the MSL directly inside a capture. Otherwise the temp source is
    // removed and no source is recorded (leaner release caches). On success `metallibOut`
    // holds the binary; on failure returns false with a message in `*outError`.
    bool compileMslToMetallib(const std::string& msl, const std::string& baseName,
        const std::string& optionName, const StageInfo& info, const fs::path& outputDir,
        bool emitMsl, std::vector<uint8_t>& metallibOut, std::string* outError)
    {
        const auto fail = [&](const std::string& message) {
            if (outError != nullptr) { *outError = message; }
            return false;
        };

        // Persisted .metal mirrors the .vfcache naming (incl. the [option] suffix) so each
        // variant stays distinct; the temp path is per-stem and removed immediately.
        const std::string emitStem =
            baseName + (optionName.empty() ? "" : "[" + optionName + "]");
        const fs::path metalSource = emitMsl
            ? outputDir / (emitStem + "." + info.shortName + ".metal")
            : outputDir / (baseName + "." + info.shortName + ".tmp.metal");
        {
            std::ofstream src(metalSource, std::ios::binary | std::ios::trunc);
            if (!src)
            {
                return fail("cannot write intermediate MSL source " + metalSource.string());
            }
            src.write(msl.data(), static_cast<std::streamsize>(msl.size()));
            if (!src)
            {
                return fail("failed writing intermediate MSL source " + metalSource.string());
            }
        }

        const fs::path metallibTmp =
            outputDir / (baseName + "." + info.shortName + ".tmp.metallib");

        std::vector<std::string> metalArgs = {"-sdk", "macosx", "metal"};
        if (emitMsl)
        {
            metalArgs.push_back("-frecord-sources");
            metalArgs.push_back("-gline-tables-only");
        }
        metalArgs.push_back("-o");
        metalArgs.push_back(metallibTmp.string());
        metalArgs.push_back(metalSource.string());

        const SubprocessResult result = runSubprocess(VKM_XCRUN_EXECUTABLE_PATH, metalArgs);
        if (result.exitCode != 0)
        {
            if (!emitMsl) { fs::remove(metalSource); }
            return fail("metal failed for " + metalSource.filename().string() + ":\n" + result.output);
        }

        metallibOut = readFileBytes(metallibTmp);
        fs::remove(metallibTmp);
        if (!emitMsl) { fs::remove(metalSource); }
        if (metallibOut.empty())
        {
            return fail("metal produced empty metallib for " + metalSource.filename().string());
        }
        return true;
    }
#endif // VKM_METAL_TOOLS_AVAILABLE

    bool compileStage(const VkmShaderStageDescriptor& desc,
                      const StageInfo& info,
                      VkmShaderCacheBackend backend,
                      const std::string& optionName,
                      const std::vector<VkmTableResourceBinding>& perPassResources,
                      const std::vector<VkmTableResourceBinding>& perDrawResources,
                      const fs::path& shaderRoot,
                      const fs::path& includeDir,
                      const fs::path& outputDir,
                      bool emitMsl)
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

        if (!compileToSpirv(desc, info, backend, source, includeDir, spvTmp))
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

        uint32_t threadGroupSize[3] = {};
        if (info.stage == VkmShaderCacheStage::Compute &&
            !readComputeThreadGroupSize(spirv, desc.entryPoint, threadGroupSize))
        {
            std::cerr << "vkm-compiler: could not read [numthreads(...)] from "
                      << source.filename().string() << " (entry '" << desc.entryPoint
                      << "'); Metal needs it at dispatch time\n";
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

                // The engine-global bindless set 0 (unsized descriptor arrays) requires
                // Tier-2 argument buffers; the resulting MSL binding layout must match the
                // runtime convention in common/bindless_resource_manager.h exactly (see
                // VkmBindlessResourceManagerMetal).
                spirv_cross::CompilerMSL::Options mslOptions;
                mslOptions.platform = spirv_cross::CompilerMSL::Options::macOS;
                mslOptions.set_msl_version(3, 0);
                mslOptions.argument_buffers = true;
                mslOptions.argument_buffers_tier =
                    spirv_cross::CompilerMSL::Options::ArgumentBuffersTier::Tier2;
                // [[id(N)]] is only an argument-index attribute -- the Metal compiler lays
                // the argument-buffer struct out sequentially, so byte offsets only equal
                // id*8 (the runtime's flat-table model) when padding members fill every id
                // gap. pad_argument_buffer_resources synthesizes those members from the
                // add_msl_resource_binding entries below (e.g. the texture array at ids
                // 0..4095 when a shader doesn't declare it).
                mslOptions.pad_argument_buffer_resources = true;
                mslOptions.force_active_argument_buffer_resources = true;
                compiler.set_msl_options(mslOptions);

                spv::ExecutionModel executionModel = spv::ExecutionModelVertex;
                switch (info.stage)
                {
                    case VkmShaderCacheStage::Vertex:   executionModel = spv::ExecutionModelVertex; break;
                    case VkmShaderCacheStage::Fragment: executionModel = spv::ExecutionModelFragment; break;
                    case VkmShaderCacheStage::Compute:  executionModel = spv::ExecutionModelGLCompute; break;
                }

                const auto addSetZeroBinding = [&](uint32_t binding, uint32_t count,
                                                   spirv_cross::SPIRType::BaseType baseType,
                                                   uint32_t mslBuffer, uint32_t mslTexture) {
                    spirv_cross::MSLResourceBinding resourceBinding;
                    resourceBinding.stage = executionModel;
                    resourceBinding.basetype = baseType;
                    resourceBinding.desc_set = 0;
                    resourceBinding.binding = binding;
                    resourceBinding.count = count;
                    resourceBinding.msl_buffer = mslBuffer;
                    resourceBinding.msl_texture = mslTexture;
                    compiler.add_msl_resource_binding(resourceBinding);
                };
                addSetZeroBinding(0, kVkmBindlessTextureCapacity,
                                  spirv_cross::SPIRType::Image,
                                  0, kVkmMetalBindlessTextureIdBase);
                addSetZeroBinding(1, kVkmBindlessBufferCapacity,
                                  spirv_cross::SPIRType::Float,
                                  kVkmMetalBindlessBufferIdBase, 0);
                addSetZeroBinding(2, kVkmBindlessIndexBufferCapacity,
                                  spirv_cross::SPIRType::Float,
                                  kVkmMetalBindlessIndexBufferIdBase, 0);
                // The single engine sampler, pinned right after the three arrays. Samplers
                // are keyed by msl_sampler (not msl_texture), and this entry must exist even
                // for shaders that never declare the sampler: pad_argument_buffer_resources
                // walks every registered binding to synthesize the padding members, so an
                // unregistered one would shift the whole argument-buffer layout.
                {
                    spirv_cross::MSLResourceBinding resourceBinding;
                    resourceBinding.stage = executionModel;
                    resourceBinding.basetype = spirv_cross::SPIRType::Sampler;
                    resourceBinding.desc_set = 0;
                    resourceBinding.binding = kVkmBindlessSamplerBinding;
                    resourceBinding.count = 1;
                    resourceBinding.msl_sampler = kVkmMetalBindlessSamplerId;
                    compiler.add_msl_resource_binding(resourceBinding);
                }

                // The single-descriptor singleton buffers follow the sampler, one id each (see
                // VkmBindlessSingletonBuffer). Registered even in shaders that don't declare them:
                // with padding enabled the walk needs a basetype for every set-0 binding it may
                // step over.
                for (uint32_t i = 0; i < static_cast<uint32_t>(VkmBindlessSingletonBuffer::Count); ++i)
                {
                    addSetZeroBinding(kVkmBindlessFirstSingletonBinding + i, 1,
                                      spirv_cross::SPIRType::Float,
                                      kVkmMetalBindlessSingletonIdBase + i, 0);
                }

                /*
                * The scene acceleration structure, after the singletons. Ray query on the Metal
                * path needs this entry; without it spirv-cross throws "Argument buffer resource
                * base type could not be determined".
                * Declared as a scalar, not as SPIRType::AccelerationStructure. The basetype
                * registered here selects only which index category the id belongs to and which
                * padding member is synthesized when a shader steps over it, and the switch that
                * does both accepts scalars, Image, Sampler and SampledImage only -- an
                * AccelerationStructure basetype throws "Unexpected argument buffer resource base
                * type" for every shader, not just ray ones. A scalar routes to msl_buffer, the
                * category an acceleration structure genuinely uses, spirv-cross emitting it as
                * `[[buffer(index)]]`, and its padding is one 8-byte member, the same width its
                * argument-buffer entry occupies.
                * Registered unconditionally, like every other set-0 binding: a shader that never
                * declares it still has to agree with the runtime about the whole argument-buffer
                * layout, and an unregistered binding would shift every id after it.
                */
                addSetZeroBinding(kVkmBindlessAccelerationStructureBinding, 1,
                                  spirv_cross::SPIRType::Float,
                                  kVkmMetalBindlessAccelerationStructureId, 0);

                // Pin the set-0 argument buffer itself ([[buffer(0)]]/[[buffer(1)]] are
                // reserved for the vertex-stream buffers). With padding enabled, every
                // registered binding needs a valid basetype (UInt = buffer category); the
                // lookup entry this inserts at set-0 index 2 is inert because the padding
                // walk jumps from id 0 (texture array, count 4096) straight to 4096 and
                // never queries index 2.
                {
                    spirv_cross::MSLResourceBinding resourceBinding;
                    resourceBinding.stage = executionModel;
                    resourceBinding.basetype = spirv_cross::SPIRType::UInt;
                    resourceBinding.desc_set = 0;
                    resourceBinding.binding = spirv_cross::kArgumentBufferBinding;
                    resourceBinding.msl_buffer = kVkmMetalBindlessArgumentBufferIndex;
                    compiler.add_msl_resource_binding(resourceBinding);
                }
                // Set 1 (per-frame camera constants, see common/frame_constants.h) is an
                // ordinary constant buffer, not a resource array, so it is declared *discrete*:
                // spirv-cross then emits it as a plain `constant VkmFrameConstants&` buffer
                // argument instead of wrapping it in a second Tier-2 argument buffer, which is
                // what lets the runtime bind it with a single setAddress: (see
                // VkmFrameConstantManagerMetal). Being discrete also keeps it out of the set-0
                // padding walk that pad_argument_buffer_resources drives.
                compiler.add_discrete_descriptor_set(kVkmFrameConstantSetIndex);
                {
                    spirv_cross::MSLResourceBinding resourceBinding;
                    resourceBinding.stage = executionModel;
                    resourceBinding.basetype = spirv_cross::SPIRType::Float;
                    resourceBinding.desc_set = kVkmFrameConstantSetIndex;
                    resourceBinding.binding = kVkmFrameConstantBinding;
                    resourceBinding.count = 1;
                    resourceBinding.msl_buffer = kVkmMetalFrameConstantBufferIndex;
                    compiler.add_msl_resource_binding(resourceBinding);
                }

                // Sets 2 (per-pass) and 3 (per-draw) are declared discrete for the same reason
                // set 1 is: it keeps them out of the pad_argument_buffer_resources walk, which
                // needs a registered base type for every id it steps over and so cannot cope with
                // the sparse binding indices a PSO may declare. Each declared binding therefore
                // becomes an ordinary MSL argument at a pinned index, mirroring what
                // VkmResourceTableMetal sets on the argument table at bind time -- the two index
                // maps must agree exactly, which is why both read the same vkmMetalTable*IndexBase
                // helpers.
                //
                // Only bindings this PSO declared are pinned; a shader is free to use a subset.
                const auto pinResourceTable = [&](VkmResourceSetKind kind,
                                                  const std::vector<VkmTableResourceBinding>& declaration)
                {
                    if (declaration.empty())
                    {
                        return;
                    }
                    const uint32_t setIndex = vkmResourceSetIndex(kind);
                    compiler.add_discrete_descriptor_set(setIndex);
                    VkmMetalTableIndexAssigner indexAssigner(kind);
                    for (const VkmTableResourceBinding& resource : declaration)
                    {
                        const uint32_t metalIndex = indexAssigner.next(resource.type);
                        spirv_cross::MSLResourceBinding resourceBinding;
                        resourceBinding.stage = executionModel;
                        resourceBinding.desc_set = setIndex;
                        resourceBinding.binding = resource.binding;
                        resourceBinding.count = 1;
                        switch (resource.type)
                        {
                            case VkmTableResourceType::SampledTexture:
                                resourceBinding.basetype = spirv_cross::SPIRType::Image;
                                resourceBinding.msl_texture = metalIndex;
                                break;
                            case VkmTableResourceType::Sampler:
                                resourceBinding.basetype = spirv_cross::SPIRType::Sampler;
                                resourceBinding.msl_sampler = metalIndex;
                                break;
                            case VkmTableResourceType::StorageBuffer:
                                resourceBinding.basetype = spirv_cross::SPIRType::UInt;
                                resourceBinding.msl_buffer = metalIndex;
                                break;
                            case VkmTableResourceType::UniformBuffer:
                                resourceBinding.basetype = spirv_cross::SPIRType::Float;
                                resourceBinding.msl_buffer = metalIndex;
                                break;
                        }
                        compiler.add_msl_resource_binding(resourceBinding);
                    }
                };
                pinResourceTable(VkmResourceSetKind::PerPass, perPassResources);
                pinResourceTable(VkmResourceSetKind::PerDraw, perDrawResources);

                // Pin the push-constant block (kPushConstDescSet is its own namespace,
                // never involved in set-0 padding).
                {
                    spirv_cross::MSLResourceBinding resourceBinding;
                    resourceBinding.stage = executionModel;
                    resourceBinding.basetype = spirv_cross::SPIRType::UInt;
                    resourceBinding.desc_set = spirv_cross::kPushConstDescSet;
                    resourceBinding.binding = spirv_cross::kPushConstBinding;
                    resourceBinding.msl_buffer = kVkmMetalPushConstantBufferIndex;
                    compiler.add_msl_resource_binding(resourceBinding);
                }

                const std::string msl = compiler.compile();
#if defined(VKM_METAL_TOOLS_AVAILABLE)
                std::string metallibError;
                if (compileMslToMetallib(msl, baseName, optionName, info, outputDir, emitMsl,
                                         content, &metallibError))
                {
                    format = VkmShaderCacheContentFormat::MetalLib;
                }
                else
                {
                    std::cerr << "vkm-compiler: " << metallibError << "\n";
                    ok = false;
                }
#else
                // No Metal toolchain: fall back to storing MSL source text, which the
                // runtime compiles on load (works, but does not serialize into captures).
                content.assign(msl.begin(), msl.end());
                format = VkmShaderCacheContentFormat::Msl;
#endif // VKM_METAL_TOOLS_AVAILABLE
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
            outputDir / buildShaderCacheFilename(baseName, optionName, info.stage, backend);
        if (!writeCacheFile(outPath, backend, info, format, desc.entryPoint, threadGroupSize, content))
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
        ("include-dir", "Additional #include search directory passed to dxc "
                        "(the engine's shared .hlsli headers)",
         cxxopts::value<std::string>()->default_value(""))
        ("emit-msl", "Metal only: keep the intermediate .metal source beside each "
                     ".vfcache and embed it in the metallib (-frecord-sources) so Xcode "
                     "GPU captures show shader source",
         cxxopts::value<bool>()->default_value("false"))
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
    const bool emitMsl = parsed["emit-msl"].as<bool>();

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

    const fs::path includeDir = parsed["include-dir"].as<std::string>();

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

    std::string expandError;
    std::optional<std::vector<VkmPipelineStateDescriptor>> variants =
        expandPipelineStateOptions(*pso, backend, &expandError);
    if (!variants.has_value())
    {
        std::cerr << "vkm-compiler: failed to expand PSO options for " << psoPath
                  << ": " << expandError << "\n";
        return 1;
    }

    const StageInfo vertexInfo{VkmShaderCacheStage::Vertex, "vert", "vs_6_0", nullptr};
    const StageInfo fragmentInfo{VkmShaderCacheStage::Fragment, "frag", "ps_6_0", nullptr};
    const StageInfo computeInfo{VkmShaderCacheStage::Compute, "comp", "cs_6_0", "cs_6_5"};

    bool allOk = true;
    for (const VkmPipelineStateDescriptor& variant : *variants)
    {
        if (variant.vertexShader.has_value())
        {
            allOk &= compileStage(*variant.vertexShader, vertexInfo, backend,
                                  variant.optionName, variant.perPassResources, variant.perDrawResources,
                                  shaderRoot, includeDir, outputDir, emitMsl);
        }
        if (variant.fragmentShader.has_value())
        {
            allOk &= compileStage(*variant.fragmentShader, fragmentInfo, backend,
                                  variant.optionName, variant.perPassResources, variant.perDrawResources,
                                  shaderRoot, includeDir, outputDir, emitMsl);
        }
        if (variant.computeShader.has_value())
        {
            allOk &= compileStage(*variant.computeShader, computeInfo, backend,
                                  variant.optionName, variant.perPassResources, variant.perDrawResources,
                                  shaderRoot, includeDir, outputDir, emitMsl);
        }
    }

    return allOk ? 0 : 1;
}
