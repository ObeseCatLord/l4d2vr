#pragma once

// Grenade throw arc for GrenadeThrowMode=arc (the original L4D2VR way): the
// held grenade shows where L4D2's own throw lands, aimed with the hand, and
// the trigger throws it natively. std-only.
//
// L4D2's native throw (CBaseCSGrenade::ThrowGrenade, server_srv.so 2.2.x, read
// from the decompile; numbers only):
//   forward = EyeVectors() forward (the eye angles: the hand's aim angles here,
//             sent as the view angles / the plugin's throw eye override)
//   src     = eye (origin + view offset) + forward * 16, trace-clamped
//   velocity = forward * 750 + the player's own velocity
//   pipe bomb, molotov, bile jar projectiles: gravity 0.4 (x sv_gravity),
//   elasticity 0.45, friction 0.2, no drag.
// The arc is that parabola; the drawn line starts at the hand and merges into
// it (the projectile itself starts at the eye), so it lands where it shows.
#include <algorithm>
#include <cmath>

namespace l4d2vr_grenade_arc
{
    struct V3
    {
        float x = 0.0f, y = 0.0f, z = 0.0f;
        V3() = default;
        V3(float ax, float ay, float az) : x(ax), y(ay), z(az) {}
        V3 operator+(const V3& o) const { return { x + o.x, y + o.y, z + o.z }; }
        V3 operator-(const V3& o) const { return { x - o.x, y - o.y, z - o.z }; }
        V3 operator*(float s) const { return { x * s, y * s, z * s }; }
        float Length() const { return std::sqrt(x * x + y * y + z * z); }
        bool Finite() const { return std::isfinite(x) && std::isfinite(y) && std::isfinite(z); }
    };

    struct NativeThrow
    {
        float speed = 750.0f;          // u/s along the eye forward
        float forwardOffset = 16.0f;   // spawn ahead of the eye
        float gravityScale = 0.4f;     // projectile gravity / sv_gravity
    };

    constexpr float kDefaultSvGravity = 800.0f;

    struct Launch
    {
        V3 origin{};
        V3 velocity{};
        float gravity = kDefaultSvGravity * 0.4f;
        bool valid = false;
    };

    // The native launch for an aim direction (any length) from the eye.
    inline Launch NativeLaunch(const V3& eye, V3 aim, const V3& playerVelocity,
        float svGravity = kDefaultSvGravity, const NativeThrow& model = NativeThrow{})
    {
        Launch l{};
        const float length = aim.Length();
        if (!eye.Finite() || !aim.Finite() || !(length > 1e-6f) || !std::isfinite(svGravity) || !(svGravity > 0.0f))
            return l;
        aim = aim * (1.0f / length);
        l.origin = eye + aim * model.forwardOffset;
        l.velocity = aim * model.speed + (playerVelocity.Finite() ? playerVelocity : V3());
        l.gravity = svGravity * model.gravityScale;
        l.valid = l.origin.Finite() && l.velocity.Finite();
        return l;
    }

    inline V3 PositionAt(const Launch& l, float t)
    {
        return l.origin + l.velocity * t + V3(0.0f, 0.0f, -0.5f * l.gravity * t * t);
    }

    // Time the arc comes down through z = groundZ (descending), or < 0.
    inline float TimeToGround(const Launch& l, float groundZ)
    {
        if (!l.valid) return -1.0f;
        const float disc = l.velocity.z * l.velocity.z + 2.0f * l.gravity * (l.origin.z - groundZ);
        if (!(disc >= 0.0f)) return -1.0f;
        const float t = (l.velocity.z + std::sqrt(disc)) / l.gravity;
        return std::isfinite(t) && t >= 0.0f ? t : -1.0f;
    }

    // The drawn point at time t: the trajectory, offset so the line starts at
    // the hand and blends into the true path by `blendSeconds`.
    inline V3 DrawnPointAt(const Launch& l, const V3& hand, float t, float blendSeconds)
    {
        const V3 p = PositionAt(l, t);
        if (!hand.Finite() || !(blendSeconds > 0.0f) || t >= blendSeconds)
            return p;
        float u = std::clamp(t / blendSeconds, 0.0f, 1.0f);
        u = u * u * (3.0f - 2.0f * u);
        return p + (hand - l.origin) * (1.0f - u);
    }
}
