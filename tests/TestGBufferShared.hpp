#ifndef TEST_GBUFFER_SHARED_HPP
#define TEST_GBUFFER_SHARED_HPP

#include <doctest/doctest.h>

#include <vkm/renderer/backend/common/driver.h>
#include <vkm/renderer/backend/common/render_resource_pool.h>
#include <vkm/renderer/backend/common/render_resource_pool.hpp>
#include <vkm/renderer/backend/common/texture.h>
#include <vkm/renderer/gbuffer.h>

#include <set>

/*
* Backend-agnostic cover for VkmGBuffer's resource management: allocation, the history flip, and
* resize. No shader is involved -- what is being checked is that the handles a consumer reads are
* the ones it should be reading, which is where a temporal pass silently reads its own output
* instead of last frame's if the flip is wrong.
*/

namespace vkmtest
{
    inline void runGBufferTest(vkm::VkmDriverBase* driver)
    {
        vkm::VkmGBuffer gbuffer;
        REQUIRE(gbuffer.initialize(driver, glm::uvec2(64, 32)));
        CHECK(gbuffer.isValid());
        CHECK(gbuffer.getExtent() == glm::uvec2(64, 32));

        SUBCASE("every target and both depth copies are distinct live textures")
        {
            // Eight textures: three targets plus depth, twice over. A duplicate here would mean a
            // pass writing one channel silently clobbers another.
            std::set<uint64_t> ids;
            const auto collect = [&](vkm::VkmResourceHandle handle) {
                REQUIRE(handle.isValid());
                CHECK(driver->getRenderResourcePool()->getResource<vkm::VkmTexture>(handle) != nullptr);
                ids.insert(handle.id);
            };
            for (uint32_t i = 0; i < vkm::VkmGBuffer::kTargetCount; ++i)
            {
                const auto target = static_cast<vkm::VkmGBuffer::Target>(i);
                collect(gbuffer.getTexture(target));
                collect(gbuffer.getPrevTexture(target));
            }
            collect(gbuffer.getDepthTexture());
            collect(gbuffer.getPrevDepthTexture());
            CHECK(ids.size() == (vkm::VkmGBuffer::kTargetCount + 1) * 2);
        }

        SUBCASE("advanceFrame swaps current and history, and swaps back")
        {
            const vkm::VkmResourceHandle firstNormal = gbuffer.getTexture(vkm::VkmGBuffer::Target::Normal);
            const vkm::VkmResourceHandle firstPrevNormal = gbuffer.getPrevTexture(vkm::VkmGBuffer::Target::Normal);
            const vkm::VkmResourceHandle firstDepth = gbuffer.getDepthTexture();
            REQUIRE(firstNormal != firstPrevNormal);

            gbuffer.advanceFrame();
            // What was written this frame must be what the next frame reads as history -- getting
            // this backwards makes a temporal pass read its own output.
            CHECK(gbuffer.getPrevTexture(vkm::VkmGBuffer::Target::Normal) == firstNormal);
            CHECK(gbuffer.getTexture(vkm::VkmGBuffer::Target::Normal) == firstPrevNormal);
            CHECK(gbuffer.getPrevDepthTexture() == firstDepth);

            gbuffer.advanceFrame();
            CHECK(gbuffer.getTexture(vkm::VkmGBuffer::Target::Normal) == firstNormal);
            CHECK(gbuffer.getPrevDepthTexture() != firstDepth);
        }

        SUBCASE("the framebuffer descriptor binds every target plus depth, all cleared")
        {
            const vkm::VkmFrameBufferDescriptor desc = gbuffer.makeFrameBufferDescriptor();
            CHECK(desc._width == 64);
            CHECK(desc._height == 32);
            REQUIRE(desc._renderPass._colorAttachmentCount == vkm::VkmGBuffer::kTargetCount);
            for (uint32_t i = 0; i < vkm::VkmGBuffer::kTargetCount; ++i)
            {
                CHECK(desc._renderPass._colorAttachments[i]._loadAction == vkm::VkmLoadAction::Clear);
                CHECK(desc._renderPass._colorAttachments[i]._storeAction == vkm::VkmStoreAction::Store);
                CHECK(desc._colorAttachments[i] ==
                      gbuffer.getTexture(static_cast<vkm::VkmGBuffer::Target>(i)));
            }
            REQUIRE(desc._depthStencilAttachment.has_value());
            CHECK(desc._depthStencilAttachment.value() == gbuffer.getDepthTexture());
            REQUIRE(desc._renderPass._depthStencilAttachment.has_value());
            CHECK(desc._renderPass._depthStencilAttachment.value()._loadAction == vkm::VkmLoadAction::Clear);
        }

        SUBCASE("resize to the same extent keeps the existing textures")
        {
            const vkm::VkmResourceHandle before = gbuffer.getTexture(vkm::VkmGBuffer::Target::Normal);
            REQUIRE(gbuffer.resize(glm::uvec2(64, 32)));
            CHECK(gbuffer.getTexture(vkm::VkmGBuffer::Target::Normal) == before);
        }

        SUBCASE("resize to a new extent reallocates")
        {
            const vkm::VkmResourceHandle before = gbuffer.getTexture(vkm::VkmGBuffer::Target::Normal);
            REQUIRE(gbuffer.resize(glm::uvec2(128, 64)));
            CHECK(gbuffer.getExtent() == glm::uvec2(128, 64));
            CHECK(gbuffer.getTexture(vkm::VkmGBuffer::Target::Normal) != before);
        }

        SUBCASE("a zero extent is rejected rather than allocating nothing")
        {
            CHECK_FALSE(gbuffer.resize(glm::uvec2(0, 32)));
            // The previous extent survives a rejected resize.
            CHECK(gbuffer.getExtent() == glm::uvec2(64, 32));
        }

        gbuffer.destroy();
        CHECK_FALSE(gbuffer.isValid());
    }
}

#endif // TEST_GBUFFER_SHARED_HPP
