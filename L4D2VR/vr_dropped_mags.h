#pragma once
#include "vr_interaction_geometry.h"
#include <array>
#include <cmath>
#include <cstdint>

// Ejected magazines that are not caught fall to the floor, settle and expire.
// Game units throughout (inches, gravity 800 u/s^2 by default). The ground test
// is supplied by the caller so this stays independent of the engine.
namespace l4d2vr_dropped_mags
{
    struct Params
    {
        float gravity = 800.0f;
        float restitution = 0.25f;
        float friction = 0.55f;
        float settleSpeed = 12.0f;
        float lifetimeSeconds = 8.0f;
        float maxSpeed = 900.0f;
        float maxDropDistance = 2048.0f;
    };

    struct Mag
    {
        Vector position{ 0.0f, 0.0f, 0.0f };
        Vector velocity{ 0.0f, 0.0f, 0.0f };
        Vector spawnPosition{ 0.0f, 0.0f, 0.0f };
        float age = 0.0f;
        bool settled = false;
        bool active = false;
    };

    // Ground query: segment from -> to. Returns true on a hit and fills the hit
    // point and surface normal.
    using GroundQuery = bool (*)(void* context, const Vector& from, const Vector& to, Vector& hit, Vector& normal);

    inline Vector ClampLength(const Vector& v, float maxLength)
    {
        const float length = std::sqrt(DotProduct(v, v));
        if (!std::isfinite(length) || length <= maxLength || length <= 0.0f)
            return std::isfinite(length) ? v : Vector(0.0f, 0.0f, 0.0f);
        return v * (maxLength / length);
    }

    inline Mag Spawn(const Vector& position, const Vector& velocity, const Params& params)
    {
        Mag mag{};
        if (!l4d2vr_interaction::Finite(position))
            return mag;
        mag.position = position;
        mag.spawnPosition = position;
        mag.velocity = l4d2vr_interaction::Finite(velocity) ? ClampLength(velocity, params.maxSpeed) : Vector(0, 0, 0);
        mag.active = true;
        return mag;
    }

    // Advances one magazine. Returns false once it expired or became invalid.
    inline bool Step(Mag& mag, float dt, const Params& params, GroundQuery query, void* context)
    {
        if (!mag.active)
            return false;
        if (!std::isfinite(dt) || dt <= 0.0f)
            return true;
        dt = std::fmin(dt, 0.05f);
        mag.age += dt;
        if (mag.age >= params.lifetimeSeconds)
        {
            mag.active = false;
            return false;
        }
        if (mag.settled)
            return true;

        mag.velocity.z -= params.gravity * dt;
        mag.velocity = ClampLength(mag.velocity, params.maxSpeed);
        const Vector next = mag.position + mag.velocity * dt;
        Vector hit{}, normal{};
        if (query && query(context, mag.position, next, hit, normal) && l4d2vr_interaction::Finite(hit))
        {
            float nlen = std::sqrt(DotProduct(normal, normal));
            if (!std::isfinite(nlen) || nlen < 0.0001f)
            {
                normal = Vector(0.0f, 0.0f, 1.0f);
                nlen = 1.0f;
            }
            normal = normal * (1.0f / nlen);
            const float into = DotProduct(mag.velocity, normal);
            const Vector normalPart = normal * into;
            const Vector tangentPart = mag.velocity - normalPart;
            mag.velocity = tangentPart * params.friction - normalPart * params.restitution;
            mag.position = hit + normal * 0.5f;
            if (std::sqrt(DotProduct(mag.velocity, mag.velocity)) < params.settleSpeed && normal.z > 0.6f)
            {
                mag.velocity = Vector(0.0f, 0.0f, 0.0f);
                mag.settled = true;
            }
        }
        else
        {
            mag.position = next;
        }
        const Vector travel = mag.position - mag.spawnPosition;
        if (!l4d2vr_interaction::Finite(mag.position) ||
            DotProduct(travel, travel) > params.maxDropDistance * params.maxDropDistance)
        {
            mag.active = false;
            return false;
        }
        return true;
    }

    // Fixed pool; the oldest magazine is replaced when it is full.
    template <std::size_t N>
    struct Pool
    {
        std::array<Mag, N> mags{};
        std::array<std::uint32_t, N> generation{};

        int Add(const Mag& mag)
        {
            int slot = 0;
            float oldest = -1.0f;
            for (std::size_t i = 0; i < N; ++i)
            {
                if (!mags[i].active) { slot = static_cast<int>(i); oldest = 1.0e9f; break; }
                if (mags[i].age > oldest) { oldest = mags[i].age; slot = static_cast<int>(i); }
            }
            mags[static_cast<std::size_t>(slot)] = mag;
            ++generation[static_cast<std::size_t>(slot)];
            return slot;
        }

        void Clear()
        {
            for (auto& mag : mags)
                mag = {};
        }
    };
}
