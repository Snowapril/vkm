#include <doctest/doctest.h>

#include <glm/geometric.hpp>

#include <cmath>

// ball_sim.h and sphere_mesh.h are the hand_interaction sample's simulation core, deliberately
// free of every engine type so the whole interaction -- poses in, ball position out -- is
// testable without a driver, a window or a camera.
#include "ball_sim.h"
#include "sphere_mesh.h"

namespace
{
    constexpr float kInvAspect = 0.5625f; // a 16:9 window

    // A pose whose every joint sits at the same place. Enough for the collision tests, which care
    // only about where the proxies land and how fast they move.
    vkm::HandPose uniformPose(const glm::vec2& position, bool valid = true)
    {
        vkm::HandPose pose;
        for (uint32_t i = 0; i < vkm::kHandJointCount; ++i)
        {
            pose._joints[i] = position;
            pose._confidence[i] = 1.0f;
        }
        pose._valid = valid;
        return pose;
    }

    vkm::BallSimParams restParams()
    {
        vkm::BallSimParams params;
        params._gravity = 0.0f;
        params._linearDamping = 0.0f;
        params._boundsMax = glm::vec2(1.0f, kInvAspect);
        return params;
    }
}

TEST_CASE("buildHandColliders - an invalid pose produces no proxies") {
    const vkm::HandPose current = uniformPose(glm::vec2(0.5f, 0.5f), false);
    const vkm::HandPose previous = uniformPose(glm::vec2(0.4f, 0.5f));

    vkm::HandColliders colliders;
    vkm::buildHandColliders(current, previous, 1.0f / 60.0f, kInvAspect, 0.03f, 0.07f, &colliders);

    // A dropped detection has to stop pushing the ball rather than push it from wherever the
    // pose happened to be left.
    CHECK(colliders._count == 0);
}

TEST_CASE("buildHandColliders - a valid pose produces five fingertips and a palm") {
    const vkm::HandPose current = uniformPose(glm::vec2(0.5f, 0.25f));

    vkm::HandColliders colliders;
    vkm::buildHandColliders(current, vkm::HandPose{}, 1.0f / 60.0f, kInvAspect, 0.03f, 0.07f, &colliders);

    REQUIRE(colliders._count == vkm::kHandColliderCount);
    for (uint32_t i = 0; i < 5; ++i)
    {
        CHECK(colliders._colliders[i]._radius == doctest::Approx(0.03f));
    }
    CHECK(colliders._colliders[5]._radius == doctest::Approx(0.07f));

    // Normalized y is scaled into sim space; x passes through unchanged.
    for (uint32_t i = 0; i < colliders._count; ++i)
    {
        CHECK(colliders._colliders[i]._center.x == doctest::Approx(0.5f));
        CHECK(colliders._colliders[i]._center.y == doctest::Approx(0.25f * kInvAspect));
    }
}

TEST_CASE("buildHandColliders - velocity is the finite difference, and zero without a previous pose") {
    const float deltaTime = 1.0f / 60.0f;
    const vkm::HandPose previous = uniformPose(glm::vec2(0.30f, 0.50f));
    const vkm::HandPose current = uniformPose(glm::vec2(0.42f, 0.50f));

    vkm::HandColliders colliders;
    vkm::buildHandColliders(current, previous, deltaTime, kInvAspect, 0.03f, 0.07f, &colliders);

    REQUIRE(colliders._count == vkm::kHandColliderCount);
    for (uint32_t i = 0; i < colliders._count; ++i)
    {
        CHECK(colliders._colliders[i]._velocity.x == doctest::Approx((0.42f - 0.30f) / deltaTime));
        CHECK(colliders._colliders[i]._velocity.y == doctest::Approx(0.0f));
    }

    // The first frame of a detection has nothing to difference against; reporting a velocity
    // there would fling the ball on every re-acquisition.
    vkm::HandColliders firstFrame;
    vkm::buildHandColliders(current, vkm::HandPose{}, deltaTime, kInvAspect, 0.03f, 0.07f, &firstFrame);
    REQUIRE(firstFrame._count == vkm::kHandColliderCount);
    for (uint32_t i = 0; i < firstFrame._count; ++i)
    {
        CHECK(firstFrame._colliders[i]._velocity.x == doctest::Approx(0.0f));
        CHECK(firstFrame._colliders[i]._velocity.y == doctest::Approx(0.0f));
    }
}

TEST_CASE("stepBall - a ball at rest with no gravity and no hand does not move") {
    const vkm::BallSimParams params = restParams();

    vkm::BallState ball;
    ball._position = glm::vec2(0.5f, 0.25f);
    ball._velocity = glm::vec2(0.0f, 0.0f);

    for (int frame = 0; frame < 120; ++frame)
    {
        vkm::stepBall(params, vkm::HandColliders{}, 1.0f / 60.0f, &ball);
    }

    CHECK(ball._position.x == doctest::Approx(0.5f));
    CHECK(ball._position.y == doctest::Approx(0.25f));
    CHECK(glm::length(ball._velocity) == doctest::Approx(0.0f));
}

TEST_CASE("stepBall - a wall reverses the normal velocity and keeps the ball inside the bounds") {
    vkm::BallSimParams params = restParams();
    params._wallRestitution = 0.5f;

    vkm::BallState ball;
    ball._radius = 0.05f;
    ball._position = glm::vec2(0.10f, 0.25f);
    ball._velocity = glm::vec2(-1.0f, 0.0f);

    for (int frame = 0; frame < 60; ++frame)
    {
        vkm::stepBall(params, vkm::HandColliders{}, 1.0f / 60.0f, &ball);
    }

    CHECK(ball._velocity.x > 0.0f);
    CHECK(ball._position.x >= params._boundsMin.x + ball._radius - 1e-4f);
    CHECK(ball._position.y >= params._boundsMin.y + ball._radius - 1e-4f);
    CHECK(ball._position.x <= params._boundsMax.x - ball._radius + 1e-4f);
    CHECK(ball._position.y <= params._boundsMax.y - ball._radius + 1e-4f);
}

TEST_CASE("stepBall - a moving proxy separates the ball and pushes it along the contact normal") {
    vkm::BallSimParams params = restParams();
    params._handRestitution = 0.8f;

    vkm::BallState ball;
    ball._radius = 0.06f;
    ball._position = glm::vec2(0.50f, 0.25f);
    ball._velocity = glm::vec2(0.0f, 0.0f);

    // A fingertip sweeping to the right, already overlapping the ball from the left.
    vkm::HandColliders hand;
    hand._count = 1;
    hand._colliders[0]._center = glm::vec2(0.47f, 0.25f);
    hand._colliders[0]._velocity = glm::vec2(1.2f, 0.0f);
    hand._colliders[0]._radius = 0.04f;

    vkm::stepBall(params, hand, 1.0f / 240.0f, &ball);

    const glm::vec2 delta = ball._position - hand._colliders[0]._center;
    const float minDistance = ball._radius + hand._colliders[0]._radius;
    // The contact has to leave no penetration: a ball still inside the proxy would be resolved
    // again next substep and drift instead of bouncing.
    CHECK(glm::length(delta) >= minDistance - 1e-5f);
    CHECK(ball._velocity.x > 0.0f);
    CHECK(ball._velocity.y == doctest::Approx(0.0f));
}

TEST_CASE("stepBall - a contact never grows the approach speed in the proxy's frame") {
    vkm::BallSimParams params = restParams();
    params._handRestitution = 1.0f; // the most energetic setting the clamp allows

    vkm::BallState ball;
    ball._radius = 0.06f;
    ball._position = glm::vec2(0.50f, 0.25f);
    ball._velocity = glm::vec2(-0.9f, 0.0f); // driving into the proxy

    vkm::HandColliders hand;
    hand._count = 1;
    hand._colliders[0]._center = glm::vec2(0.46f, 0.25f);
    hand._colliders[0]._velocity = glm::vec2(0.3f, 0.0f);
    hand._colliders[0]._radius = 0.04f;

    const glm::vec2 relativeBefore = ball._velocity - hand._colliders[0]._velocity;
    vkm::stepBall(params, hand, 1.0f / 240.0f, &ball);
    const glm::vec2 relativeAfter = ball._velocity - hand._colliders[0]._velocity;

    // The proxy is infinitely massive, so the bounce is only ever a reflection in its frame.
    // In the world frame the ball may well speed up -- that is the hand doing work on it -- but
    // relative to the hand it can only lose speed.
    CHECK(glm::length(relativeAfter) <= glm::length(relativeBefore) + 1e-5f);
}

TEST_CASE("stepBall - substeps make the result independent of the frame rate") {
    vkm::BallSimParams params;
    params._boundsMax = glm::vec2(1.0f, kInvAspect);
    params._substepSeconds = 1.0f / 240.0f;

    vkm::BallState coarse;
    coarse._position = glm::vec2(0.40f, 0.10f);
    coarse._velocity = glm::vec2(0.25f, -0.10f);
    vkm::BallState fine = coarse;

    vkm::stepBall(params, vkm::HandColliders{}, 1.0f / 60.0f, &coarse);
    for (int i = 0; i < 4; ++i)
    {
        vkm::stepBall(params, vkm::HandColliders{}, 1.0f / 240.0f, &fine);
    }

    CHECK(coarse._position.x == doctest::Approx(fine._position.x));
    CHECK(coarse._position.y == doctest::Approx(fine._position.y));
    CHECK(coarse._velocity.x == doctest::Approx(fine._velocity.x));
    CHECK(coarse._velocity.y == doctest::Approx(fine._velocity.y));
}

TEST_CASE("stepBall - an oversized delta time is clamped rather than caught up") {
    vkm::BallSimParams params = restParams();
    params._maxStepSeconds = 0.10f;

    vkm::BallState ball;
    ball._position = glm::vec2(0.50f, 0.25f);
    ball._velocity = glm::vec2(0.20f, 0.0f);

    // A window drag or a breakpoint can hand the simulation an arbitrarily long frame; catching
    // all of it up would teleport the ball across the playfield.
    vkm::stepBall(params, vkm::HandColliders{}, 5.0f, &ball);

    CHECK(ball._position.x == doctest::Approx(0.50f + 0.20f * 0.10f).epsilon(0.01));
}

TEST_CASE("simToWorld - the plane's centre maps to the view axis and its edges to its extents") {
    constexpr float kAspect = 16.0f / 9.0f;
    constexpr float kPlaneDistance = 2.0f;
    const float halfHeight = kPlaneDistance * std::tan(0.90f * 0.5f);
    const float halfWidth = halfHeight * kAspect;

    // Sim space runs [0, 1] in x and [0, 1 / aspect] in y, so its centre is the plane's centre.
    const glm::vec3 centre = vkm::simToWorld(glm::vec2(0.5f, 0.5f / kAspect), halfHeight, kAspect, kPlaneDistance);
    CHECK(centre.x == doctest::Approx(0.0f));
    CHECK(centre.y == doctest::Approx(0.0f));
    CHECK(centre.z == doctest::Approx(-kPlaneDistance));

    const glm::vec3 topLeft = vkm::simToWorld(glm::vec2(0.0f, 0.0f), halfHeight, kAspect, kPlaneDistance);
    CHECK(topLeft.x == doctest::Approx(-halfWidth));
    CHECK(topLeft.y == doctest::Approx(halfHeight));

    const glm::vec3 bottomRight =
        vkm::simToWorld(glm::vec2(1.0f, 1.0f / kAspect), halfHeight, kAspect, kPlaneDistance);
    CHECK(bottomRight.x == doctest::Approx(halfWidth));
    CHECK(bottomRight.y == doctest::Approx(-halfHeight));

    // Both axes share one scale, which is what keeps a simulated circle a rendered sphere rather
    // than an ellipsoid.
    CHECK(vkm::simToWorldScale(halfHeight, kAspect) == doctest::Approx(2.0f * halfWidth));
}

TEST_CASE("buildSphereMesh - produces a closed unit sphere with outward-facing triangles") {
    vkm::SphereMesh mesh;
    vkm::buildSphereMesh(12, 16, &mesh);

    REQUIRE(mesh._vertices.size() == static_cast<size_t>(13) * 17);
    REQUIRE(mesh._indices.size() == static_cast<size_t>(12) * 16 * 6);

    for (const vkm::SphereVertex& vertex : mesh._vertices)
    {
        const glm::vec3 position(vertex.position[0], vertex.position[1], vertex.position[2]);
        const glm::vec3 normal(vertex.normal[0], vertex.normal[1], vertex.normal[2]);
        CHECK(glm::length(position) == doctest::Approx(1.0f));
        CHECK(glm::length(normal) == doctest::Approx(1.0f));
    }

    // Winding: every non-degenerate triangle's geometric normal has to agree with the outward
    // direction, or back-face culling would hide the sphere and show its interior instead.
    size_t checkedTriangles = 0;
    for (size_t i = 0; i < mesh._indices.size(); i += 3)
    {
        const vkm::SphereVertex& a = mesh._vertices[mesh._indices[i + 0]];
        const vkm::SphereVertex& b = mesh._vertices[mesh._indices[i + 1]];
        const vkm::SphereVertex& c = mesh._vertices[mesh._indices[i + 2]];

        const glm::vec3 pa(a.position[0], a.position[1], a.position[2]);
        const glm::vec3 pb(b.position[0], b.position[1], b.position[2]);
        const glm::vec3 pc(c.position[0], c.position[1], c.position[2]);

        const glm::vec3 faceNormal = glm::cross(pb - pa, pc - pa);
        if (glm::length(faceNormal) < 1e-5f)
        {
            continue; // the pole rings collapse to a point, so their triangles have no area
        }

        const glm::vec3 centroid = (pa + pb + pc) / 3.0f;
        CHECK(glm::dot(glm::normalize(faceNormal), glm::normalize(centroid)) > 0.0f);
        ++checkedTriangles;
    }
    CHECK(checkedTriangles > 0);
}
