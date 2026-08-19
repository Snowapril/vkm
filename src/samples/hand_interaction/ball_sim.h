// Copyright (c) 2025 Snowapril
//
// The interactive part of the sample, kept free of every engine type so it can be unit-tested
// headlessly (tests/TestHandInteraction.cpp).
//
// Everything here lives in "sim space": a 2D space measured in units of the window's *width*,
// so x runs [0, 1] and y runs [0, invAspect] where invAspect = height / width, y pointing down.
// Measuring both axes in the same unit is what keeps a circle a circle -- a plain [0,1]^2 space
// would squash one axis and turn the ball into an ellipse. It also leaves the simulation
// independent of the pixel resolution: only invAspect changes when the window is resized.

#pragma once

#include "hand_pose.h"

#include <glm/common.hpp>
#include <glm/geometric.hpp>
#include <glm/vec2.hpp>
#include <glm/vec3.hpp>

#include <cmath>
#include <cstdint>
#include <iterator>

namespace vkm
{
    // Five fingertips plus one palm centroid.
    inline constexpr uint32_t kHandColliderCount = 6;

    /*
    * @brief One immovable, possibly moving sphere the ball can bounce off, in sim space.
    * @details Immovable in the sense that the ball never pushes back: a hand has effectively
    * infinite mass next to the ball, so a collision only rewrites the ball's velocity.
    */
    struct HandCollider
    {
        glm::vec2 _center{ 0.0f, 0.0f };
        glm::vec2 _velocity{ 0.0f, 0.0f }; // sim units per second
        float _radius = 0.0f;
    };

    /*
    * @brief The proxies built from one hand pose. _count is 0 when no hand was detected.
    */
    struct HandColliders
    {
        HandCollider _colliders[kHandColliderCount]{};
        uint32_t _count = 0;
    };

    /*
    * @brief The ball's state, including its own substep accumulator.
    * @details The accumulator lives here rather than in the caller so that stepBall() is a pure
    * function of (params, hand, deltaTime, state): advancing 1/60 s in one call and in four
    * 1/240 s calls produce bit-identical results, which is what makes the simulation
    * frame-rate independent and the tests deterministic.
    */
    struct BallState
    {
        glm::vec2 _position{ 0.5f, 0.2f };
        glm::vec2 _velocity{ 0.0f, 0.0f };
        float _radius = 0.07f;
        float _stepAccumulator = 0.0f;
    };

    /*
    * @brief Tunables for one simulation. All rates are per second, all lengths in sim units.
    * @details _wallRestitution and _handRestitution are the fraction of normal speed returned by
    * a bounce and must stay within [0, 1]; stepBall() clamps them, since a value above 1 would
    * let the ball gain energy every frame until it escapes the bounds. _boundsMax.y should track
    * the window's height/width so the playfield fills the window exactly.
    */
    struct BallSimParams
    {
        float _gravity = 0.30f;         // +y is down, so a positive value falls
        float _linearDamping = 0.35f;
        float _wallRestitution = 0.72f;
        float _handRestitution = 0.85f;
        glm::vec2 _boundsMin{ 0.0f, 0.0f };
        glm::vec2 _boundsMax{ 1.0f, 0.5625f };
        float _substepSeconds = 1.0f / 240.0f;
        // A window drag or a breakpoint can hand us an arbitrarily long deltaTime; without a
        // ceiling the substep loop would try to catch up all of it in one call.
        float _maxStepSeconds = 0.25f;
    };

    /*
    * @brief Turns a hand pose into the six collision proxies, differencing against the previous
    * pose for their velocities.
    * @details Produces nothing (_count = 0) unless `current` is valid, so a dropped detection
    * simply stops pushing the ball instead of snapping it somewhere. Velocities are zero when
    * `previous` is invalid or `deltaTime` is not positive -- the first frame of a detection has
    * nothing to difference against, and reporting a velocity there would fling the ball.
    * @param current This frame's pose, in normalized top-left-origin image coordinates.
    * @param previous Last frame's pose, for the finite difference.
    * @param deltaTime Seconds between the two poses.
    * @param invAspect Window height divided by width, mapping pose y into sim space.
    * @param fingertipRadius Proxy radius for the five fingertips, in sim units.
    * @param palmRadius Proxy radius for the palm centroid, in sim units.
    * @param outColliders Receives the proxies.
    */
    inline void buildHandColliders(const HandPose& current, const HandPose& previous,
                                   float deltaTime, float invAspect,
                                   float fingertipRadius, float palmRadius,
                                   HandColliders* outColliders)
    {
        outColliders->_count = 0;
        if (!current._valid)
        {
            return;
        }

        const bool hasVelocity = previous._valid && deltaTime > 0.0f;
        const float inverseDeltaTime = hasVelocity ? (1.0f / deltaTime) : 0.0f;

        auto toSim = [invAspect](const glm::vec2& normalized) {
            return glm::vec2(normalized.x, normalized.y * invAspect);
        };

        for (const HandJoint joint : kHandFingertips)
        {
            const size_t index = static_cast<size_t>(joint);
            HandCollider& collider = outColliders->_colliders[outColliders->_count++];
            collider._center = toSim(current._joints[index]);
            collider._velocity = hasVelocity
                ? (collider._center - toSim(previous._joints[index])) * inverseDeltaTime
                : glm::vec2(0.0f, 0.0f);
            collider._radius = fingertipRadius;
        }

        glm::vec2 palmNow(0.0f, 0.0f);
        glm::vec2 palmBefore(0.0f, 0.0f);
        for (const HandJoint joint : kHandPalmJoints)
        {
            const size_t index = static_cast<size_t>(joint);
            palmNow += toSim(current._joints[index]);
            palmBefore += toSim(previous._joints[index]);
        }
        const float palmJointCount = static_cast<float>(std::size(kHandPalmJoints));
        palmNow /= palmJointCount;
        palmBefore /= palmJointCount;

        HandCollider& palm = outColliders->_colliders[outColliders->_count++];
        palm._center = palmNow;
        palm._velocity = hasVelocity ? (palmNow - palmBefore) * inverseDeltaTime : glm::vec2(0.0f, 0.0f);
        palm._radius = palmRadius;
    }

    namespace detail
    {
        /*
        * @brief Resolves one ball-versus-proxy overlap: separate, then reflect.
        * @details The proxy is treated as infinitely massive, so the bounce is computed in its
        * frame: the normal component of the *relative* velocity is reversed and scaled by
        * `restitution`. That is what lets a sweeping hand accelerate the ball in the world frame
        * while the relative approach speed can only ever shrink.
        */
        inline void resolveHandContact(const HandCollider& collider, float restitution, BallState* ball)
        {
            const glm::vec2 delta = ball->_position - collider._center;
            const float minDistance = ball->_radius + collider._radius;
            const float distanceSquared = glm::dot(delta, delta);
            if (distanceSquared >= minDistance * minDistance)
            {
                return;
            }

            // A ball sitting exactly on the proxy's center has no defined normal; push it
            // straight up so the contact still resolves instead of producing a NaN.
            const float distance = std::sqrt(distanceSquared);
            const glm::vec2 normal = distance > 1e-6f ? (delta / distance) : glm::vec2(0.0f, -1.0f);

            ball->_position = collider._center + normal * minDistance;

            const float approachSpeed = glm::dot(ball->_velocity - collider._velocity, normal);
            if (approachSpeed < 0.0f)
            {
                ball->_velocity -= (1.0f + restitution) * approachSpeed * normal;
            }
        }

        /*
        * @brief Keeps the ball inside the bounds on one axis, reflecting the velocity it hit with.
        */
        inline void resolveWallContact(float minBound, float maxBound, float restitution,
                                       float* position, float* velocity, float radius)
        {
            if (*position < minBound + radius)
            {
                *position = minBound + radius;
                if (*velocity < 0.0f)
                {
                    *velocity = -*velocity * restitution;
                }
            }
            else if (*position > maxBound - radius)
            {
                *position = maxBound - radius;
                if (*velocity > 0.0f)
                {
                    *velocity = -*velocity * restitution;
                }
            }
        }

        inline void integrateBall(const BallSimParams& params, const HandColliders& hand,
                                  float deltaTime, BallState* ball)
        {
            const float wallRestitution = glm::clamp(params._wallRestitution, 0.0f, 1.0f);
            const float handRestitution = glm::clamp(params._handRestitution, 0.0f, 1.0f);

            ball->_velocity.y += params._gravity * deltaTime;
            // Exponential decay rather than a subtraction: it cannot overshoot into a negative
            // velocity however large deltaTime or the damping coefficient get.
            ball->_velocity *= std::exp(-glm::max(params._linearDamping, 0.0f) * deltaTime);
            ball->_position += ball->_velocity * deltaTime;

            for (uint32_t i = 0; i < hand._count; ++i)
            {
                resolveHandContact(hand._colliders[i], handRestitution, ball);
            }

            resolveWallContact(params._boundsMin.x, params._boundsMax.x, wallRestitution,
                               &ball->_position.x, &ball->_velocity.x, ball->_radius);
            resolveWallContact(params._boundsMin.y, params._boundsMax.y, wallRestitution,
                               &ball->_position.y, &ball->_velocity.y, ball->_radius);
        }
    } // namespace detail

    /*
    * @brief Advances the ball by `deltaTime`, in fixed substeps of params._substepSeconds.
    * @details Time left over after the last whole substep is carried in BallState, so the
    * simulation runs identically at any frame rate. A deltaTime beyond params._maxStepSeconds is
    * clamped rather than caught up.
    * @param params Tunables; restitutions are clamped to [0, 1] here.
    * @param hand This frame's proxies, empty when no hand is detected.
    * @param deltaTime Seconds since the last call.
    * @param ball Advanced in place.
    */
    inline void stepBall(const BallSimParams& params, const HandColliders& hand,
                         float deltaTime, BallState* ball)
    {
        const float substep = params._substepSeconds;
        if (substep <= 0.0f)
        {
            return;
        }

        ball->_stepAccumulator += glm::clamp(deltaTime, 0.0f, params._maxStepSeconds);
        while (ball->_stepAccumulator >= substep)
        {
            ball->_stepAccumulator -= substep;
            detail::integrateBall(params, hand, substep, ball);
        }
    }

    /*
    * @brief Where a sim-space point sits in world space, on the interaction plane.
    * @details The camera sits at the origin looking down -Z with the plane at z =
    * -planeDistance, so the plane's half-height is planeDistance * tan(fovY / 2) and one sim
    * unit spans the plane's full width. Both axes therefore share a single scale, which is why a
    * ball of radius r in sim space draws as a sphere of radius r * scale rather than an
    * ellipsoid.
    * @param sim Point in sim space, y down.
    * @param halfHeight Half the plane's world height, planeDistance * tan(fovY / 2).
    * @param aspect Window width divided by height.
    * @param planeDistance Distance from the camera to the plane, positive.
    */
    inline glm::vec3 simToWorld(const glm::vec2& sim, float halfHeight, float aspect, float planeDistance)
    {
        const float worldPerSimUnit = 2.0f * halfHeight * aspect;
        return glm::vec3((sim.x - 0.5f) * worldPerSimUnit,
                         halfHeight - sim.y * worldPerSimUnit,
                         -planeDistance);
    }

    // World length of one sim unit, for turning a sim-space radius into a sphere scale.
    inline float simToWorldScale(float halfHeight, float aspect)
    {
        return 2.0f * halfHeight * aspect;
    }
} // namespace vkm
