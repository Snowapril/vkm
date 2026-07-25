#ifndef TEST_CUBEMAP_TEXTURE_SHARED_HPP
#define TEST_CUBEMAP_TEXTURE_SHARED_HPP

#include <doctest/doctest.h>

#include <vkm/renderer/backend/common/bindless_resource_manager.h>
#include <vkm/renderer/backend/common/command_buffer.h>
#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/pipeline_state_manager.h>
#include <vkm/renderer/backend/common/pipeline_state_object.h>
#include <vkm/renderer/backend/common/render_graph.h>
#include <vkm/renderer/backend/common/render_pass.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/renderer_common.h>
#include <vkm/renderer/backend/common/texture.h>

#include <glm/ext/matrix_clip_space.hpp>
#include <glm/ext/matrix_transform.hpp>
#include <glm/matrix.hpp>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace vkmtest
{
    // Distinct color per cube face, so a face-order or slice-index mistake shows up as the
    // wrong color rather than as a still-plausible image. Verifying all six is the point of
    // this test: uploading one face proves almost nothing about a cubemap.
    inline constexpr std::array<std::array<uint8_t, 4>, vkm::kVkmCubeFaceCount> kFaceColors = {{
        {{255, 0, 0, 255}},     // +X
        {{0, 255, 255, 255}},   // -X
        {{0, 255, 0, 255}},     // +Y
        {{255, 0, 255, 255}},   // -Y
        {{0, 0, 255, 255}},     // +Z
        {{255, 255, 0, 255}},   // -Z
    }};

    inline constexpr uint32_t kCubeFaceExtent = 32;

    /*
    * @brief Backend-agnostic cubemap test body: create a cube texture, upload six faces
    * through the engine's upload path, publish it into the bindless texture array, and read
    * every face back.
    * @details Runs with validation layers on, so the assertions below are only half of what
    * is being tested -- a missing VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT, a wrong Metal
    * arrayLength, or an image left in the wrong layout surfaces as a validation error.
    */
    inline void runCubemapTextureTest(vkm::VkmDriverBase* driver)
    {
        REQUIRE(driver != nullptr);

        if ((driver->getDriverCapabilityFlags() & vkm::VkmDriverCapabilityFlags::TextureUpload) == 0)
        {
            MESSAGE("Skipping: this backend does not implement texture upload");
            return;
        }

        vkm::VkmTextureInfo textureInfo{};
        textureInfo._flags = static_cast<vkm::VkmResourceCreateInfo>(
            static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowShaderRead) |
            static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowTransferDst) |
            static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowTransferSrc));
        textureInfo._extent = glm::uvec3(kCubeFaceExtent, kCubeFaceExtent, 1);
        textureInfo._numMipLevels = 1;
        textureInfo._numArrayLayers = vkm::kVkmCubeFaceCount;
        textureInfo._format = vkm::VkmFormat::R8G8B8A8_UNORM;
        textureInfo._type = vkm::VkmTextureType::Cube;
        textureInfo._debugName = "TestCubemap";

        vkm::VkmTexture* cubemap = driver->newTexture(textureInfo);
        REQUIRE(cubemap != nullptr);
        const vkm::VkmResourceHandle cubemapHandle = cubemap->getHandle();
        CHECK(cubemap->getTextureInfo()._type == vkm::VkmTextureType::Cube);

        // Uploads face `face` in the given color through the given path.
        const auto uploadFace = [&](uint32_t face, const std::array<uint8_t, 4>& color, vkm::VkmTextureUploadMode mode) {
            std::vector<uint8_t> pixels(static_cast<size_t>(kCubeFaceExtent) * kCubeFaceExtent * 4);
            for (size_t texel = 0; texel < pixels.size(); texel += 4)
            {
                std::copy(color.begin(), color.end(), pixels.begin() + texel);
            }
            return driver->uploadToTexture(cubemapHandle, pixels.data(), pixels.size(), 0, face, mode);
        };
        const auto checkFaceReadsBack = [&](uint32_t face, const std::array<uint8_t, 4>& color) {
            const vkm::VkmTextureReadbackResult readback = driver->readbackTexture(cubemapHandle, face);
            REQUIRE(readback.pixels.size() >= 4);
            CHECK(readback.width == kCubeFaceExtent);
            CHECK(readback.height == kCubeFaceExtent);

            CAPTURE(face);
            CHECK(readback.pixels[0] == color[0]);
            CHECK(readback.pixels[1] == color[1]);
            CHECK(readback.pixels[2] == color[2]);
        };

        for (uint32_t face = 0; face < vkm::kVkmCubeFaceCount; ++face)
        {
            CHECK(uploadFace(face, kFaceColors[face], vkm::VkmTextureUploadMode::Auto));
        }

        SUBCASE("every face reads back the color it was uploaded with")
        {
            for (uint32_t face = 0; face < vkm::kVkmCubeFaceCount; ++face)
            {
                checkFaceReadsBack(face, kFaceColors[face]);
            }
        }

        /*
        * The assertion that matters for the host-copy path: whichever route the pixels take,
        * the result must be indistinguishable. The second pass deliberately writes a
        * *different* color per face (the face order reversed) so that a host copy which
        * silently did nothing would leave the staging pass's colors behind and fail here,
        * rather than passing by coincidence.
        */
        SUBCASE("the staging and host-copy paths produce identical results")
        {
            for (uint32_t face = 0; face < vkm::kVkmCubeFaceCount; ++face)
            {
                CHECK(uploadFace(face, kFaceColors[face], vkm::VkmTextureUploadMode::ForceStaging));
            }
            for (uint32_t face = 0; face < vkm::kVkmCubeFaceCount; ++face)
            {
                checkFaceReadsBack(face, kFaceColors[face]);
            }

            const auto reversedColor = [](uint32_t face) {
                return kFaceColors[vkm::kVkmCubeFaceCount - 1 - face];
            };
            for (uint32_t face = 0; face < vkm::kVkmCubeFaceCount; ++face)
            {
                CHECK(uploadFace(face, reversedColor(face), vkm::VkmTextureUploadMode::ForceHostCopy));
            }
            for (uint32_t face = 0; face < vkm::kVkmCubeFaceCount; ++face)
            {
                checkFaceReadsBack(face, reversedColor(face));
            }
        }

        SUBCASE("a plain upload destination is host-writable exactly when the device supports it")
        {
            // Guards against the whole feature silently degrading to staging everywhere: if
            // the device advertises host copy, a plain AllowTransferDst texture like this one
            // must actually get it, not merely be eligible in principle. Asserting against
            // the capability rather than hasUnifiedMemory() keeps this correct on devices
            // that are unified but lack the mechanism (e.g. no VK_EXT_host_image_copy).
            const bool deviceSupportsHostCopy =
                (driver->getDriverCapabilityFlags() & vkm::VkmDriverCapabilityFlags::TextureHostCopy) != 0;
            CAPTURE(deviceSupportsHostCopy);
            CAPTURE(driver->hasUnifiedMemory());
            CHECK(cubemap->isHostWritable() == deviceSupportsHostCopy);
        }

        SUBCASE("the cubemap can be published into the bindless texture array")
        {
            vkm::VkmBindlessResourceManagerBase* bindlessManager = driver->getBindlessResourceManager();
            REQUIRE(bindlessManager != nullptr);

            const uint32_t slot = bindlessManager->registerTexture(cubemapHandle);
            CHECK(slot != UINT32_MAX);
            CHECK(slot < vkm::kVkmBindlessTextureCapacity);
            bindlessManager->unregisterTexture(slot);
        }

        driver->getRenderResourcePool()->releaseResource(cubemapHandle);
    }

    /*
    * @brief Renders the skybox sample's PSO into an offscreen target and asserts which face
    * lands where.
    * @details This is the cross-backend half of the cubemap coverage, and the reason it is
    * shared rather than Metal-only: the vertex shader reconstructs a view ray from NDC
    * coordinates it builds itself, and Vulkan compiles that stage with -fvk-invert-y. The
    * flip moves SV_Position and its interpolants together, so both backends must produce the
    * same image -- if that reasoning is ever wrong, or someone "compensates" for the flip,
    * the sky comes out mirrored and the top/bottom assertions below catch it.
    */
    inline void runSkyboxRenderTest(vkm::VkmDriverBase* driver)
    {
        REQUIRE(driver != nullptr);

        if ((driver->getDriverCapabilityFlags() & vkm::VkmDriverCapabilityFlags::TextureUpload) == 0)
        {
            MESSAGE("Skipping: this backend does not implement texture upload");
            return;
        }

        constexpr uint32_t kSize = 64;

        vkm::VkmBindlessResourceManagerBase* bindlessManager = driver->getBindlessResourceManager();
        REQUIRE(bindlessManager != nullptr);

        vkm::VkmPipelineStateManager manager(driver);
        std::string err;
        REQUIRE_MESSAGE(manager.loadPipelineStatesFromDirectory(TEST_SKYBOX_SAMPLE_DIR, TEST_SKYBOX_SHADER_CACHE_DIR,
                                                                vkm::VkmPipelineStateOrigin::User, &err), err);
        vkm::VkmPipelineStateBase* pso =
            manager.getPipelineState("skybox_pso[default]", vkm::VkmPipelineStateOrigin::User);
        REQUIRE(pso != nullptr);

        // Same six-color cubemap as runCubemapTextureTest.
        vkm::VkmTextureInfo textureInfo{};
        textureInfo._flags = static_cast<vkm::VkmResourceCreateInfo>(
            static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowShaderRead) |
            static_cast<uint32_t>(vkm::VkmResourceCreateInfo::AllowTransferDst));
        textureInfo._extent = glm::uvec3(kCubeFaceExtent, kCubeFaceExtent, 1);
        textureInfo._numMipLevels = 1;
        textureInfo._numArrayLayers = vkm::kVkmCubeFaceCount;
        textureInfo._format = vkm::VkmFormat::R8G8B8A8_UNORM;
        textureInfo._type = vkm::VkmTextureType::Cube;
        textureInfo._debugName = "TestSkyboxCubemap";

        vkm::VkmTexture* cubemap = driver->newTexture(textureInfo);
        REQUIRE(cubemap != nullptr);

        const size_t faceByteSize = static_cast<size_t>(kCubeFaceExtent) * kCubeFaceExtent * 4;
        for (uint32_t face = 0; face < vkm::kVkmCubeFaceCount; ++face)
        {
            std::vector<uint8_t> pixels(faceByteSize);
            for (size_t texel = 0; texel < pixels.size(); texel += 4)
            {
                std::copy(kFaceColors[face].begin(), kFaceColors[face].end(), pixels.begin() + texel);
            }
            REQUIRE(driver->uploadToTexture(cubemap->getHandle(), pixels.data(), pixels.size(), 0, face));
        }

        const uint32_t cubemapSlot = bindlessManager->registerTexture(cubemap->getHandle());
        REQUIRE(cubemapSlot != UINT32_MAX);

        vkm::VkmTextureInfo targetInfo{};
        targetInfo._flags = vkm::VkmResourceCreateInfo::AllowColorAttachment | vkm::VkmResourceCreateInfo::AllowTransferSrc;
        targetInfo._extent = glm::uvec3(kSize, kSize, 1);
        targetInfo._format = vkm::VkmFormat::BGRA8_UNORM;
        targetInfo._numMipLevels = 1;
        targetInfo._numArrayLayers = 1;
        vkm::VkmTexture* offscreen = driver->newTexture(targetInfo);
        REQUIRE(offscreen != nullptr);

        vkm::VkmFrameBufferDescriptor fbDesc{};
        fbDesc._width = kSize;
        fbDesc._height = kSize;
        fbDesc._renderPass._colorAttachmentCount = 1;
        fbDesc._renderPass._colorAttachments[0]._attachmentId = 0;
        fbDesc._renderPass._colorAttachments[0]._loadAction = vkm::VkmLoadAction::Clear;
        fbDesc._renderPass._colorAttachments[0]._storeAction = vkm::VkmStoreAction::Store;
        fbDesc._renderPass._colorAttachments[0]._clearColors[3] = 1.0f;
        fbDesc._colorAttachments[0] = offscreen->getHandle();

        // Mirrors the sample's LookCamera at yaw = pitch = 0: looking straight down +Z, with
        // a right-handed view whose screen-right is -X. Keep in sync with
        // src/samples/skybox/main.cpp.
        const glm::mat4 view = glm::lookAtRH(glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(0.0f, 1.0f, 0.0f));
        // Deliberately much wider than the sample's 60 degrees. A cube face spans +/-45
        // degrees about its axis, so a 60-degree FOV on a square target only ever reaches
        // 30 degrees off-axis and every pixel would land on +Z -- there would be nothing to
        // assert about the other five faces. At 120 degrees the edges sit ~59 degrees out,
        // comfortably past the face boundary.
        const glm::mat4 projection = glm::perspectiveRH_ZO(glm::radians(120.0f), 1.0f, 0.1f, 10.0f);

        struct SkyboxPushConstants
        {
            glm::mat4 _inverseViewRotationProjection;
            uint32_t _cubemapSlot;
        } pushConstants{glm::inverse(projection * glm::mat4(glm::mat3(view))), cubemapSlot};

        vkm::VkmRenderGraph renderGraph(driver, /*frameIndex=*/0);
        auto* subGraph = renderGraph.beginGraphicsSubGraph(fbDesc);
        subGraph->setRenderCallback([pso, pushConstants](vkm::VkmCommandBufferBase* commandBuffer) {
            commandBuffer->bindPipeline(pso);
            commandBuffer->setPushConstants(&pushConstants, sizeof(pushConstants));
            commandBuffer->draw(3, 1, 0, 0);
        });
        renderGraph.compile();
        renderGraph.execute();
        renderGraph.ensureCompleted();

        vkm::VkmTextureReadbackResult readback = driver->readbackTexture(offscreen->getHandle());
        REQUIRE(readback.pixels.size() == static_cast<size_t>(kSize) * kSize * 4);
        REQUIRE(readback.channels == 4);

        // BGRA8, row 0 = top row of the framebuffer (same convention as
        // runClipSpaceOrientationTest).
        const auto pixelAt = [&](uint32_t x, uint32_t y) {
            return &readback.pixels[(static_cast<size_t>(y) * readback.width + x) * readback.channels];
        };
        const auto dominantChannel = [](const uint8_t* bgra) {
            // Returns 'b', 'g' or 'r' -- enough to name a face without exact-color matching,
            // which would be hostage to sampler filtering at the face edges.
            if (bgra[0] >= bgra[1] && bgra[0] >= bgra[2]) return 'b';
            return (bgra[1] >= bgra[2]) ? 'g' : 'r';
        };

        SUBCASE("the center pixel shows the +Z face")
        {
            // Looking down +Z at a 60-degree FOV: the middle of the image is squarely inside
            // the +Z face (blue).
            const uint8_t* center = pixelAt(kSize / 2, kSize / 2);
            CHECK(dominantChannel(center) == 'b');
            CHECK(center[0] > 200); // blue channel, i.e. the +Z face color
            CHECK(center[2] < 100); // and not red
        }

        SUBCASE("left and right edges show +X and -X, not each other")
        {
            // glm::lookAtRH with forward +Z and up +Y puts -X on the right of the screen, so
            // the left edge is the +X face (red) and the right edge the -X face (cyan).
            // Swapping the two faces, or mirroring horizontally, flips this.
            const uint8_t* left = pixelAt(1, kSize / 2);
            const uint8_t* right = pixelAt(kSize - 2, kSize / 2);
            CHECK(dominantChannel(left) == 'r');
            CHECK(left[2] > left[0]); // R > B
            CHECK(right[0] > right[2]); // cyan: B > R
            CHECK(right[1] > right[2]); // cyan: G > R
        }

        SUBCASE("top and bottom show +Y and -Y -- the vertical-flip detector")
        {
            // This is the assertion that a -fvk-invert-y mistake breaks: +Y (green) must be
            // at the top of the image and -Y (magenta) at the bottom, on every backend.
            const uint8_t* top = pixelAt(kSize / 2, 1);
            const uint8_t* bottom = pixelAt(kSize / 2, kSize - 2);
            CHECK(dominantChannel(top) == 'g');
            CHECK(top[1] > top[2]); // green: G > R
            CHECK(top[1] > top[0]); // green: G > B
            CHECK(bottom[2] > bottom[1]); // magenta: R > G
            CHECK(bottom[0] > bottom[1]); // magenta: B > G
        }

        bindlessManager->unregisterTexture(cubemapSlot);

        // Same rationale as runClipSpaceOrientationTest: release before the fixture destroys
        // the driver, or VMA reports unfreed allocations.
        vkm::VkmRenderResourcePool* resourcePool = driver->getRenderResourcePool();
        resourcePool->releaseResource(offscreen->getHandle());
        resourcePool->releaseResource(cubemap->getHandle());
    }
} // namespace vkmtest

#endif // TEST_CUBEMAP_TEXTURE_SHARED_HPP
