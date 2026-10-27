#pragma once
#include "sdk/vector.h"
#include <algorithm>
#include <cmath>
#include <limits>

// Throw ballistics for servers that only accept the native fixed-speed throw.
// Units are game units (inches) and seconds throughout; gravity defaults to
// sv_gravity (800 u/s^2) and points down -Z. Angles use the Source convention:
// pitch in degrees with negative = up, yaw = atan2(y, x) in degrees.
//
// Projectile model: drag-free parabola, or an optional linear drag
// (a = g - k*v, k in 1/s) solved in closed form. Linear drag is only an
// approximation of the game's physics, intended to be calibrated per throwable.
namespace l4d2vr_ballistics
{
    inline constexpr float kPi = 3.14159265358979323846f;
    inline constexpr float kDefaultGravity = 800.0f;
    inline constexpr float kMaxFlightTime = 30.0f;
    inline constexpr float kMaxPitchDeg = 89.0f;

    inline bool Finite(const Vector& v)
    { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
    inline float Deg(float radians) { return radians * (180.0f / kPi); }
    inline float Rad(float degrees) { return degrees * (kPi / 180.0f); }
    inline bool ValidGravity(float g) { return std::isfinite(g) && g > 0.0f; }
    inline bool ValidDrag(float k) { return std::isfinite(k) && k >= 0.0f; }

    // F(t) = (1 - e^{-kt}) / k   (-> t as k -> 0)
    // G(t) = (t - F(t)) / k       (-> t^2/2 as k -> 0)
    // Position: p0 + v0*F - g*G*z;  velocity: v0*e^{-kt} - g*F*z. Written this
    // way (no terminal-velocity term) it stays exact for tiny drag.
    inline double DragF(double t, double k)
    {
        const double kt = k * t;
        if (std::fabs(kt) < 1.0e-6) return t * (1.0 - 0.5 * kt);
        return -std::expm1(-kt) / k;
    }
    inline double DragG(double t, double k)
    {
        const double kt = k * t;
        if (std::fabs(kt) < 1.0e-3) return t * t * (0.5 - kt / 6.0 + kt * kt / 24.0);
        return (kt + std::expm1(-kt)) / (k * k);
    }

    inline Vector PositionAt(const Vector& origin, const Vector& velocity, float t,
        float gravity = kDefaultGravity, float drag = 0.0f)
    {
        if (drag <= 0.0f)
            return origin + velocity * t + Vector(0.0f, 0.0f, -0.5f * gravity * t * t);
        const double f = DragF(t, drag), g = DragG(t, drag);
        return Vector(static_cast<float>(origin.x + velocity.x * f), static_cast<float>(origin.y + velocity.y * f),
            static_cast<float>(origin.z + velocity.z * f - gravity * g));
    }

    inline Vector VelocityAt(const Vector& velocity, float t, float gravity = kDefaultGravity, float drag = 0.0f)
    {
        if (drag <= 0.0f) return Vector(velocity.x, velocity.y, velocity.z - gravity * t);
        const double e = std::exp(-static_cast<double>(drag) * t), f = DragF(t, drag);
        return Vector(static_cast<float>(velocity.x * e), static_cast<float>(velocity.y * e),
            static_cast<float>(velocity.z * e - gravity * f));
    }

    // Time of the apex (vertical velocity zero); 0 when already descending.
    inline float ApexTime(float vz, float gravity, float drag)
    {
        if (!(vz > 0.0f)) return 0.0f;
        if (drag <= 0.0f) return vz / gravity;
        return std::log1p(drag * vz / gravity) / drag;
    }

    struct Landing
    {
        Vector point{0.0f, 0.0f, 0.0f};
        Vector velocity{0.0f, 0.0f, 0.0f}; // at impact
        float time = 0.0f;
        bool valid = false;
    };

    // Where the projectile comes down through the plane z = groundZ (descending
    // crossing). Invalid when the trajectory never reaches the plane from above.
    inline Landing PredictLanding(const Vector& origin, const Vector& velocity, float groundZ,
        float gravity = kDefaultGravity, float drag = 0.0f)
    {
        Landing landing{};
        if (!Finite(origin) || !Finite(velocity) || !std::isfinite(groundZ) || !ValidGravity(gravity) ||
            !ValidDrag(drag))
            return landing;
        float t = 0.0f;
        if (drag <= 0.0f)
        {
            const float disc = velocity.z * velocity.z + 2.0f * gravity * (origin.z - groundZ);
            if (disc < 0.0f) return landing;
            t = (velocity.z + std::sqrt(disc)) / gravity;
            if (!std::isfinite(t) || t < 0.0f) return landing;
        }
        else
        {
            // z(t) is strictly decreasing after the apex: bisect there.
            float lo = ApexTime(velocity.z, gravity, drag);
            if (PositionAt(origin, velocity, lo, gravity, drag).z < groundZ) return landing;
            float hi = (std::max)(lo, 0.05f);
            while (PositionAt(origin, velocity, hi, gravity, drag).z > groundZ)
            {
                hi *= 2.0f;
                if (hi > kMaxFlightTime) return landing;
            }
            for (int i = 0; i < 60; ++i)
            {
                const float mid = 0.5f * (lo + hi);
                (PositionAt(origin, velocity, mid, gravity, drag).z > groundZ ? lo : hi) = mid;
            }
            t = 0.5f * (lo + hi);
        }
        if (t > kMaxFlightTime) return landing;
        landing.point = PositionAt(origin, velocity, t, gravity, drag);
        landing.point.z = groundZ;
        landing.velocity = VelocityAt(velocity, t, gravity, drag);
        landing.time = t;
        landing.valid = Finite(landing.point) && Finite(landing.velocity);
        return landing;
    }

    inline Vector VelocityFromAngles(float pitchDeg, float yawDeg, float speed)
    {
        const float cp = std::cos(Rad(pitchDeg)), sp = std::sin(Rad(pitchDeg));
        const float cy = std::cos(Rad(yawDeg)), sy = std::sin(Rad(yawDeg));
        return Vector(cp * cy * speed, cp * sy * speed, -sp * speed);
    }

    struct LaunchSolution
    {
        float pitchDeg = 0.0f; // Source convention: negative = up
        float yawDeg = 0.0f;
        float time = 0.0f;     // flight time to the target
        Vector velocity{0.0f, 0.0f, 0.0f};
        bool valid = false;
    };

    namespace detail
    {
        inline LaunchSolution Finish(float elevationRad, float yawDeg, float speed, float time)
        {
            LaunchSolution s{};
            s.pitchDeg = -Deg(elevationRad);
            s.yawDeg = yawDeg;
            s.time = time;
            s.velocity = VelocityFromAngles(s.pitchDeg, yawDeg, speed);
            s.valid = std::isfinite(s.pitchDeg) && std::isfinite(time) && time >= 0.0f && Finite(s.velocity);
            return s;
        }

        // Height reached at horizontal distance d for launch elevation theta
        // (minus target height h); -inf if d is never reached.
        inline float HeightError(float theta, float d, float h, float speed, float gravity, float drag, float& time)
        {
            const float vx = speed * std::cos(theta), vz = speed * std::sin(theta);
            if (!(vx > 0.0f)) return -std::numeric_limits<float>::infinity();
            float t = 0.0f;
            if (drag <= 0.0f) t = d / vx;
            else
            {
                const float ratio = drag * d / vx;
                if (!(ratio < 1.0f)) return -std::numeric_limits<float>::infinity();
                t = -std::log1p(-ratio) / drag;
            }
            if (!std::isfinite(t) || t > kMaxFlightTime) return -std::numeric_limits<float>::infinity();
            time = t;
            return PositionAt(Vector(0.0f, 0.0f, 0.0f), Vector(vx, 0.0f, vz), t, gravity, drag).z - h;
        }
    }

    // Launch angles that hit `target` from `origin` at a fixed launch speed.
    // preferLowArc picks the flatter of the two solutions. Fails (valid=false)
    // when the target is out of range; the caller then falls back to the raw
    // throw direction. drag > 0 uses the linear-drag model (numeric solve).
    inline LaunchSolution SolveLaunchPitch(const Vector& origin, const Vector& target, float fixedSpeed,
        float gravity = kDefaultGravity, bool preferLowArc = true, float drag = 0.0f)
    {
        if (!Finite(origin) || !Finite(target) || !std::isfinite(fixedSpeed) || !(fixedSpeed > 0.0f) ||
            !ValidGravity(gravity) || !ValidDrag(drag))
            return LaunchSolution{};
        const float dx = target.x - origin.x, dy = target.y - origin.y, h = target.z - origin.z;
        const float d = std::sqrt(dx * dx + dy * dy);
        const float v = fixedSpeed;
        if (d < 0.01f)
        {
            // Straight up or down. Target above needs enough speed; the "high arc"
            // for a target below goes up first and falls back down onto it.
            const Landing up = PredictLanding(origin, Vector(0.0f, 0.0f, v), target.z, gravity, drag);
            if (h > 0.0f || !preferLowArc)
            {
                if (!up.valid) return LaunchSolution{};
                // Exact hit on the way up when the target is above.
                float t = up.time;
                if (h > 0.0f && drag <= 0.0f) t = (v - std::sqrt((std::max)(0.0f, v * v - 2.0f * gravity * h))) / gravity;
                return detail::Finish(kPi * 0.5f, 0.0f, v, t);
            }
            const Landing down = PredictLanding(origin, Vector(0.0f, 0.0f, -v), target.z, gravity, drag);
            return down.valid ? detail::Finish(-kPi * 0.5f, 0.0f, v, down.time) : LaunchSolution{};
        }
        const float yaw = Deg(std::atan2(dy, dx));
        if (drag <= 0.0f)
        {
            const float v2 = v * v;
            const float disc = v2 * v2 - gravity * (gravity * d * d + 2.0f * h * v2);
            if (!std::isfinite(disc) || disc < 0.0f) return LaunchSolution{};
            const float root = std::sqrt(disc);
            const float theta = std::atan2(preferLowArc ? v2 - root : v2 + root, gravity * d);
            const float time = d / (v * std::cos(theta));
            return detail::Finish(theta, yaw, v, time);
        }
        // Linear drag: the height error at distance d is unimodal in elevation.
        // Coarse scan for the maximum, refine it, then bisect the chosen side.
        const float lowLimit = -Rad(kMaxPitchDeg), highLimit = Rad(kMaxPitchDeg);
        float time = 0.0f;
        float best = lowLimit, bestError = -std::numeric_limits<float>::infinity();
        constexpr int kSamples = 178;
        for (int i = 0; i <= kSamples; ++i)
        {
            const float theta = lowLimit + (highLimit - lowLimit) * static_cast<float>(i) / kSamples;
            const float e = detail::HeightError(theta, d, h, v, gravity, drag, time);
            if (e > bestError) { bestError = e; best = theta; }
        }
        const float step = (highLimit - lowLimit) / kSamples;
        float a = (std::max)(lowLimit, best - step), b = (std::min)(highLimit, best + step);
        for (int i = 0; i < 60; ++i)
        {
            const float m1 = a + (b - a) / 3.0f, m2 = b - (b - a) / 3.0f;
            if (detail::HeightError(m1, d, h, v, gravity, drag, time) <
                detail::HeightError(m2, d, h, v, gravity, drag, time)) a = m1;
            else b = m2;
        }
        const float peak = 0.5f * (a + b);
        if (!(detail::HeightError(peak, d, h, v, gravity, drag, time) >= 0.0f)) return LaunchSolution{};
        float lo = preferLowArc ? lowLimit : peak, hi = preferLowArc ? peak : highLimit;
        // f(lo) < 0 <= f(hi) on the low side; mirrored on the high side.
        const float edge = detail::HeightError(preferLowArc ? lowLimit : highLimit, d, h, v, gravity, drag, time);
        if (edge >= 0.0f) return LaunchSolution{}; // root lies beyond the pitch limit
        for (int i = 0; i < 60; ++i)
        {
            const float mid = 0.5f * (lo + hi);
            const bool above = detail::HeightError(mid, d, h, v, gravity, drag, time) >= 0.0f;
            if (preferLowArc) (above ? hi : lo) = mid;
            else (above ? lo : hi) = mid;
        }
        const float theta = 0.5f * (lo + hi);
        if (!std::isfinite(detail::HeightError(theta, d, h, v, gravity, drag, time))) return LaunchSolution{};
        return detail::Finish(theta, yaw, v, time);
    }

    enum class Throwable { Molotov, PipeBomb, VomitJar };

    // How the game launches a throwable natively: fixed speed along the aim
    // angles with an upward pitch bias. Defaults are placeholders pending
    // in-game calibration.
    struct NativeThrowModel
    {
        float speed = 700.0f;          // u/s
        float upwardBiasDeg = 0.0f;    // launch pitch = aim pitch - upwardBiasDeg
        float gravityScale = 1.0f;     // projectile gravity = gravity * gravityScale
        float drag = 0.0f;             // linear drag (1/s)
    };

    inline NativeThrowModel DefaultNativeThrowModel(Throwable /*type*/)
    {
        // All three share the defaults until measured; kept per type so
        // calibration can diverge without changing callers.
        return NativeThrowModel{};
    }

    inline bool Valid(const NativeThrowModel& m)
    {
        return std::isfinite(m.speed) && m.speed > 0.0f && std::isfinite(m.upwardBiasDeg) &&
            std::fabs(m.upwardBiasDeg) < 90.0f && std::isfinite(m.gravityScale) && m.gravityScale > 0.0f &&
            ValidDrag(m.drag);
    }

    // Launch pitch -> the aim pitch to hand the game (undoes the bias).
    inline float AimPitchForLaunchPitch(float launchPitchDeg, const NativeThrowModel& m)
    { return launchPitchDeg + m.upwardBiasDeg; }
    inline float LaunchPitchForAimPitch(float aimPitchDeg, const NativeThrowModel& m)
    { return aimPitchDeg - m.upwardBiasDeg; }

    inline bool DirectionAngles(const Vector& direction, float& pitchDeg, float& yawDeg)
    {
        if (!Finite(direction)) return false;
        const float horizontal = std::sqrt(direction.x * direction.x + direction.y * direction.y);
        if (!(horizontal > 1.0e-6f) && !(std::fabs(direction.z) > 1.0e-6f)) return false;
        pitchDeg = Deg(std::atan2(-direction.z, horizontal));
        yawDeg = horizontal > 1.0e-6f ? Deg(std::atan2(direction.y, direction.x)) : 0.0f;
        return true;
    }

    enum class ArcChoice { Low, High, MatchHand };

    struct NativeThrowAim
    {
        float pitchDeg = 0.0f;   // aim angles to give the native throw
        float yawDeg = 0.0f;
        Vector landing{0.0f, 0.0f, 0.0f}; // where the hand throw would land
        bool solved = false;     // true: native throw is aimed to land at `landing`
        bool fallback = false;   // true: angles are the raw hand direction (unreachable etc.)
    };

    // Maps a VR hand release (position + velocity) to aim angles for the
    // native fixed-speed throw launched from nativeOrigin, such that it lands
    // where the hand throw would land on the plane z = groundZ. When the
    // native throw cannot reach that point the raw hand direction is returned
    // with fallback=true (solved=false); when nothing is usable (bad model or
    // origin, zero hand velocity) both are false.
    inline NativeThrowAim MapHandThrowToNative(const Vector& handOrigin, const Vector& handVelocity,
        const Vector& nativeOrigin, float groundZ, const NativeThrowModel& model,
        float gravity = kDefaultGravity, ArcChoice arc = ArcChoice::MatchHand)
    {
        NativeThrowAim aim{};
        if (!Valid(model) || !ValidGravity(gravity) || !Finite(nativeOrigin)) return aim;
        float rawPitch = 0.0f, rawYaw = 0.0f;
        if (!DirectionAngles(handVelocity, rawPitch, rawYaw)) return aim; // no throw direction at all
        auto fallBack = [&]() {
            aim.pitchDeg = std::clamp(AimPitchForLaunchPitch(rawPitch, model), -kMaxPitchDeg, kMaxPitchDeg);
            aim.yawDeg = rawYaw;
            aim.fallback = true;
            return aim;
        };
        const float g = gravity * model.gravityScale;
        const Landing landing = PredictLanding(handOrigin, handVelocity, groundZ, g, model.drag);
        if (!landing.valid) return fallBack();
        aim.landing = landing.point;
        const LaunchSolution low = SolveLaunchPitch(nativeOrigin, landing.point, model.speed, g, true, model.drag);
        const LaunchSolution high = SolveLaunchPitch(nativeOrigin, landing.point, model.speed, g, false, model.drag);
        LaunchSolution pick = arc == ArcChoice::High ? high : low;
        if (arc == ArcChoice::MatchHand && low.valid && high.valid)
            pick = std::fabs(high.pitchDeg - rawPitch) < std::fabs(low.pitchDeg - rawPitch) ? high : low;
        if (!pick.valid) return fallBack();
        const float aimPitch = AimPitchForLaunchPitch(pick.pitchDeg, model);
        if (!(std::fabs(aimPitch) <= kMaxPitchDeg)) return fallBack(); // bias pushes it past the view clamp
        aim.pitchDeg = aimPitch;
        aim.yawDeg = pick.yawDeg;
        aim.solved = true;
        return aim;
    }
}
