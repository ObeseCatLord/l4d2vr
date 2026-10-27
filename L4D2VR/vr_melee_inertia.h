#pragma once
#include "sdk/vector.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>

// Held melee weapons lag the controller through a critically damped (zeta = 1)
// spring so heavy weapons feel heavy. Units: metres, seconds; angular speeds are
// stored in rad/s and configured in deg/s. Source axis convention: forward = +x,
// left = +y (right = -y), up = +z. Callers convert game units with m_VRScale.
namespace l4d2vr_melee_inertia
{
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kDegToRad = kPi / 180.0f;
    constexpr float kRadToDeg = 180.0f / kPi;

    inline bool Finite(const Vector& v)
    {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    // ---------------------------------------------------------------- quaternion
    struct Quat
    {
        float w = 1.0f, x = 0.0f, y = 0.0f, z = 0.0f;
    };

    inline bool QuatFinite(const Quat& q)
    {
        return std::isfinite(q.w) && std::isfinite(q.x) && std::isfinite(q.y) && std::isfinite(q.z);
    }
    inline float QuatDot(const Quat& a, const Quat& b) { return a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z; }
    inline Quat QuatConjugate(const Quat& q) { return Quat{ q.w, -q.x, -q.y, -q.z }; }
    inline Quat QuatMultiply(const Quat& a, const Quat& b)
    {
        return Quat{
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w };
    }
    inline Quat QuatNormalize(const Quat& q)
    {
        const float length = std::sqrt(QuatDot(q, q));
        if (!std::isfinite(length) || length < 1e-8f) return Quat{};
        const float inv = 1.0f / length;
        return Quat{ q.w * inv, q.x * inv, q.y * inv, q.z * inv };
    }
    inline Vector QuatRotate(const Quat& q, const Vector& v)
    {
        const Vector u(q.x, q.y, q.z);
        const Vector t = CrossProduct(u, v) * 2.0f;
        return v + t * q.w + CrossProduct(u, t);
    }
    // Rotation vector (axis * angle, radians) -> unit quaternion.
    inline Quat QuatExp(const Vector& rotation)
    {
        const float angle = rotation.Length();
        if (!std::isfinite(angle)) return Quat{};
        if (angle < 1e-6f)
            return QuatNormalize(Quat{ 1.0f, rotation.x * 0.5f, rotation.y * 0.5f, rotation.z * 0.5f });
        const float s = std::sin(angle * 0.5f) / angle;
        return Quat{ std::cos(angle * 0.5f), rotation.x * s, rotation.y * s, rotation.z * s };
    }
    // Unit quaternion -> shortest-path rotation vector (axis * angle, radians, |angle| <= pi).
    inline Vector QuatLog(const Quat& input)
    {
        Quat q = QuatNormalize(input);
        if (q.w < 0.0f) q = Quat{ -q.w, -q.x, -q.y, -q.z };
        const Vector v(q.x, q.y, q.z);
        const float s = v.Length();
        if (s < 1e-6f) return v * 2.0f;
        const float angle = 2.0f * std::atan2(s, q.w);
        return v * (angle / s);
    }
    inline Quat QuatFromAxisAngle(const Vector& axis, float radians)
    {
        const float length = axis.Length();
        if (!std::isfinite(length) || length < 1e-8f || !std::isfinite(radians)) return Quat{};
        return QuatExp(axis * (radians / length));
    }
    // Angle in radians between two orientations (0..pi).
    inline float QuatAngleBetween(const Quat& a, const Quat& b)
    {
        return QuatLog(QuatMultiply(b, QuatConjugate(a))).Length();
    }
    inline Quat QuatSlerp(const Quat& a, const Quat& input, float t)
    {
        t = std::clamp(std::isfinite(t) ? t : 0.0f, 0.0f, 1.0f);
        Quat b = input;
        float cosine = QuatDot(a, b);
        if (cosine < 0.0f) { b = Quat{ -b.w, -b.x, -b.y, -b.z }; cosine = -cosine; }
        if (cosine > 0.9995f)
            return QuatNormalize(Quat{ a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t,
                a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t });
        const float theta = std::acos(std::min(cosine, 1.0f));
        const float sinTheta = std::sin(theta);
        const float wa = std::sin((1.0f - t) * theta) / sinTheta, wb = std::sin(t * theta) / sinTheta;
        return QuatNormalize(Quat{ a.w * wa + b.w * wb, a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb });
    }
    // Builds the rotation whose +x is forward, +y is left (-right) and +z is up.
    // Forward is kept exactly; up is orthogonalised (right is used when up is degenerate).
    inline bool QuatFromAxes(const Vector& forward, const Vector& right, const Vector& up, Quat& result)
    {
        if (!Finite(forward) || !Finite(right) || !Finite(up)) return false;
        const float forwardLength = forward.Length();
        if (forwardLength < 1e-6f) return false;
        const Vector f = forward * (1.0f / forwardLength);
        Vector u = up - f * DotProduct(f, up);
        float upLength = u.Length();
        if (upLength < 1e-4f)
        {
            const Vector left = -right - f * DotProduct(f, -right);
            const float leftLength = left.Length();
            if (leftLength < 1e-4f) return false;
            u = CrossProduct(f, left * (1.0f / leftLength));
            upLength = u.Length();
        }
        u = u * (1.0f / upLength);
        const Vector l = CrossProduct(u, f);
        // Rotation matrix columns: f, l, u.
        const float m00 = f.x, m01 = l.x, m02 = u.x;
        const float m10 = f.y, m11 = l.y, m12 = u.y;
        const float m20 = f.z, m21 = l.z, m22 = u.z;
        const float trace = m00 + m11 + m22;
        Quat q{};
        if (trace > 0.0f)
        {
            const float s = std::sqrt(trace + 1.0f) * 2.0f;
            q = Quat{ 0.25f * s, (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s };
        }
        else if (m00 > m11 && m00 > m22)
        {
            const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
            q = Quat{ (m21 - m12) / s, 0.25f * s, (m01 + m10) / s, (m02 + m20) / s };
        }
        else if (m11 > m22)
        {
            const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
            q = Quat{ (m02 - m20) / s, (m01 + m10) / s, 0.25f * s, (m12 + m21) / s };
        }
        else
        {
            const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
            q = Quat{ (m10 - m01) / s, (m02 + m20) / s, (m12 + m21) / s, 0.25f * s };
        }
        result = QuatNormalize(q);
        return QuatFinite(result);
    }
    inline void QuatToAxes(const Quat& q, Vector& forward, Vector& right, Vector& up)
    {
        forward = QuatRotate(q, Vector(1.0f, 0.0f, 0.0f));
        right = -QuatRotate(q, Vector(0.0f, 1.0f, 0.0f));
        up = QuatRotate(q, Vector(0.0f, 0.0f, 1.0f));
    }

    // ---------------------------------------------------------------------- pose
    struct Pose
    {
        Vector position{ 0.0f, 0.0f, 0.0f };
        Quat orientation{};
    };

    inline bool PoseFinite(const Pose& pose) { return Finite(pose.position) && QuatFinite(pose.orientation); }
    inline bool PoseFromAxes(const Vector& origin, const Vector& forward, const Vector& right, const Vector& up, Pose& result)
    {
        Quat q{};
        if (!Finite(origin) || !QuatFromAxes(forward, right, up, q)) return false;
        result.position = origin;
        result.orientation = q;
        return true;
    }
    // a * b: b expressed in a's frame, mapped to world.
    inline Pose PoseCompose(const Pose& a, const Pose& b)
    {
        return Pose{ a.position + QuatRotate(a.orientation, b.position),
            QuatNormalize(QuatMultiply(a.orientation, b.orientation)) };
    }
    inline Pose PoseInverse(const Pose& pose)
    {
        const Quat inv = QuatConjugate(QuatNormalize(pose.orientation));
        return Pose{ -QuatRotate(inv, pose.position), inv };
    }
    // delta = inverse(controller) * lagged, i.e. the lagged pose in the controller's local frame.
    inline Pose LagDelta(const Pose& controller, const Pose& lagged)
    {
        return PoseCompose(PoseInverse(controller), lagged);
    }
    // Re-applies a lag delta to a (possibly fresher) controller pose: controller * delta.
    inline Pose ApplyLagDelta(const Pose& controller, const Pose& delta)
    {
        return PoseCompose(controller, delta);
    }

    // ------------------------------------------------------------------- params
    enum class WeightClass { Light, Medium, Heavy, Chainsaw };

    struct Params
    {
        float positionHz = 14.0f;
        float rotationHz = 10.0f;
        float maxAngularSpeedDegPerSec = 520.0f;
        float maxLinearSpeed = 8.0f;      // m/s
        float snapDistance = 1.0f;        // m; larger target jumps (teleport, respawn) snap
        bool motionMelee = true;          // false: weapon is never swing-triggered (chainsaw)
    };

    inline bool ValidParams(const Params& p)
    {
        return std::isfinite(p.positionHz) && p.positionHz > 0.0f && std::isfinite(p.rotationHz) &&
            p.rotationHz > 0.0f && std::isfinite(p.maxAngularSpeedDegPerSec) && p.maxAngularSpeedDegPerSec > 0.0f &&
            std::isfinite(p.maxLinearSpeed) && p.maxLinearSpeed > 0.0f && std::isfinite(p.snapDistance) &&
            p.snapDistance > 0.0f;
    }

    inline Params ParamsForClass(WeightClass weight)
    {
        Params p{};
        switch (weight)
        {
        case WeightClass::Heavy:
            p.positionHz = 11.0f; p.rotationHz = 7.0f; p.maxAngularSpeedDegPerSec = 420.0f; p.maxLinearSpeed = 6.0f;
            break;
        case WeightClass::Light:
            p.positionHz = 20.0f; p.rotationHz = 14.0f; p.maxAngularSpeedDegPerSec = 700.0f; p.maxLinearSpeed = 10.0f;
            break;
        case WeightClass::Chainsaw:
            p.positionHz = 10.0f; p.rotationHz = 7.0f; p.maxAngularSpeedDegPerSec = 360.0f; p.maxLinearSpeed = 5.0f;
            p.motionMelee = false;
            break;
        case WeightClass::Medium:
        default:
            p.positionHz = 14.0f; p.rotationHz = 10.0f; p.maxAngularSpeedDegPerSec = 520.0f; p.maxLinearSpeed = 8.0f;
            break;
        }
        return p;
    }

    inline bool NameEquals(const char* name, const char* expected)
    {
        for (; *name && *expected; ++name, ++expected)
            if (std::tolower(static_cast<unsigned char>(*name)) != *expected) return false;
        return *name == '\0' && *expected == '\0';
    }

    // Maps L4D2 melee script names ("fireaxe", "katana", ...) to a weight class.
    // A "weapon_" prefix is ignored, matching is case-insensitive, unknown -> Medium.
    inline WeightClass ClassForMeleeScriptName(const char* name)
    {
        if (!name) return WeightClass::Medium;
        const char prefix[] = "weapon_";
        if (std::strlen(name) > sizeof(prefix) - 1)
        {
            bool prefixed = true;
            for (std::size_t i = 0; i + 1 < sizeof(prefix); ++i)
                if (std::tolower(static_cast<unsigned char>(name[i])) != prefix[i]) { prefixed = false; break; }
            if (prefixed) name += sizeof(prefix) - 1;
        }
        static const char* const heavy[] = { "fireaxe", "sledgehammer", "electric_guitar", "pitchfork", "shovel", "riotshield" };
        static const char* const light[] = { "knife" };
        for (const char* candidate : heavy)
            if (NameEquals(name, candidate)) return WeightClass::Heavy;
        for (const char* candidate : light)
            if (NameEquals(name, candidate)) return WeightClass::Light;
        if (NameEquals(name, "chainsaw")) return WeightClass::Chainsaw;
        // baseball_bat, cricket_bat, crowbar, katana, machete, golfclub, frying_pan, tonfa, unknown.
        return WeightClass::Medium;
    }

    inline Params ParamsForMeleeScriptName(const char* name)
    {
        return ParamsForClass(ClassForMeleeScriptName(name));
    }

    // ------------------------------------------------------------------- spring
    struct State
    {
        Vector position{ 0.0f, 0.0f, 0.0f };
        Vector velocity{ 0.0f, 0.0f, 0.0f };        // m/s, world
        Quat orientation{};
        Vector angularVelocity{ 0.0f, 0.0f, 0.0f }; // rad/s, world
    };

    constexpr float kMaxStepDt = 0.05f;      // longer frames (hitches) are clamped
    constexpr float kMaxSubstepDt = 1.0f / 240.0f;

    inline Vector ClampLength(const Vector& v, float maxLength)
    {
        const float length = v.Length();
        return length > maxLength && length > 0.0f ? v * (maxLength / length) : v;
    }

    // Velocity is solved implicitly (backward Euler on the spring-damper) and the
    // position/orientation is advanced with the new velocity. For zeta = 1 this
    // has a repeated real pole 1/(1 + omega*h): never overshoots, stable for any h.
    class Spring
    {
    public:
        explicit Spring(const Params& params = Params{}) { SetParams(params); }

        void SetParams(const Params& params) { m_Params = ValidParams(params) ? params : Params{}; }
        const Params& GetParams() const { return m_Params; }
        bool Initialized() const { return m_Initialized; }
        const State& GetState() const { return m_State; }
        Pose Lagged() const { return Pose{ m_State.position, m_State.orientation }; }
        float AngularSpeedDegPerSec() const { return m_State.angularVelocity.Length() * kRadToDeg; }

        // Snaps to the pose with zero velocity.
        bool Reset(const Pose& pose)
        {
            if (!PoseFinite(pose)) return false;
            m_State = State{};
            m_State.position = pose.position;
            m_State.orientation = QuatNormalize(pose.orientation);
            m_Initialized = true;
            return true;
        }
        void Clear() { m_State = State{}; m_Initialized = false; }

        // Advances toward the controller target. Invalid input leaves the state unchanged.
        bool Step(const Pose& target, float dt)
        {
            if (!PoseFinite(target) || !std::isfinite(dt)) return false;
            if (!m_Initialized || (target.position - m_State.position).Length() > m_Params.snapDistance)
                return Reset(target);
            dt = std::clamp(dt, 0.0f, kMaxStepDt);
            if (dt <= 0.0f) return true;
            const Quat goal = QuatNormalize(target.orientation);
            const int steps = std::max(1, static_cast<int>(std::ceil(dt / kMaxSubstepDt - 1e-4f)));
            const float h = dt / static_cast<float>(steps);
            const float wp = 2.0f * kPi * m_Params.positionHz, wr = 2.0f * kPi * m_Params.rotationHz;
            const float dp = (1.0f + wp * h) * (1.0f + wp * h), dr = (1.0f + wr * h) * (1.0f + wr * h);
            const float maxAngular = m_Params.maxAngularSpeedDegPerSec * kDegToRad;
            for (int i = 0; i < steps; ++i)
            {
                const Vector error = m_State.position - target.position;
                m_State.velocity = ClampLength((m_State.velocity - error * (h * wp * wp)) * (1.0f / dp),
                    m_Params.maxLinearSpeed);
                m_State.position += m_State.velocity * h;

                const Vector angularError = QuatLog(QuatMultiply(m_State.orientation, QuatConjugate(goal)));
                m_State.angularVelocity = ClampLength((m_State.angularVelocity - angularError * (h * wr * wr)) *
                    (1.0f / dr), maxAngular);
                m_State.orientation = QuatNormalize(QuatMultiply(QuatExp(m_State.angularVelocity * h),
                    m_State.orientation));
            }
            if (!Finite(m_State.position) || !Finite(m_State.velocity) || !QuatFinite(m_State.orientation) ||
                !Finite(m_State.angularVelocity))
                return Reset(target);
            return true;
        }

        // Lagged pose in the controller's local frame, for re-application to a fresher pose.
        Pose Delta(const Pose& controller) const { return LagDelta(controller, Lagged()); }

    private:
        Params m_Params{};
        State m_State{};
        bool m_Initialized = false;
    };
}
