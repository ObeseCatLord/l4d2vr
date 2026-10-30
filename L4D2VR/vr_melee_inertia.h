#pragma once
#include "sdk/vector.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>

// Held melee weapons are welded to the hand at the grip; their rotation lags
// the controller through a damped angular spring tuned per weapon (modelled on
// Ancient Dungeon VR: its sword for long weapons, its dagger for short ones).
// Units: metres, seconds; angular speeds are stored in rad/s and configured in
// deg/s. Source axis convention: forward = +x,
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
    // The held weapon is welded to the hand at the grip and only its rotation
    // lags, like a weapon on a joint with locked linear axes and a damped
    // angular drive: the blade trails the controller in proportion to how fast
    // it turns (lag ~ 2 * zeta / omega * angular speed), up to a soft lag limit.
    // The drive damps the weapon's absolute angular velocity, which is what
    // makes the lag speed-proportional. Long weapons ring a little (zeta < 1),
    // short ones snap back (zeta > 1).
    struct Params
    {
        float rotationHz = 11.0f;         // natural frequency of the angular drive
        float rotationZeta = 0.8f;        // damping ratio (on the absolute angular velocity)
        float maxLagDeg = 20.0f;          // soft lag limit; 0 = none
        float limitStiffness = 0.86f;     // extra spring past the limit, fraction of the drive spring
        float limitDamping = 1.0f;        // extra damping of the lag rate past the limit, fraction of the drive damping
        float maxAngularSpeedDegPerSec = 2500.0f; // guard against tracking glitches only
        float snapAngleDeg = 90.0f;       // a larger lag (glitch, teleported controller) snaps
        bool positionLocked = true;       // grip welded to the hand; false: positionHz spring (legacy)
        float positionHz = 14.0f;         // only when !positionLocked
        float maxLinearSpeed = 8.0f;      // m/s, only when !positionLocked
        float snapDistance = 1.0f;        // m; larger hand jumps (teleport, respawn) snap
        float reach = 0.85f;              // m, grip to tip
        bool motionMelee = true;          // false: weapon is never swing-triggered (chainsaw)
    };

    inline bool ValidParams(const Params& p)
    {
        auto positive = [](float v) { return std::isfinite(v) && v > 0.0f; };
        auto nonNegative = [](float v) { return std::isfinite(v) && v >= 0.0f; };
        return positive(p.rotationHz) && positive(p.rotationZeta) && nonNegative(p.maxLagDeg) &&
            nonNegative(p.limitStiffness) && nonNegative(p.limitDamping) && positive(p.maxAngularSpeedDegPerSec) &&
            positive(p.snapAngleDeg) && positive(p.positionHz) && positive(p.maxLinearSpeed) &&
            positive(p.snapDistance) && positive(p.reach);
    }

    // Steady-state lag (degrees) at a constant angular speed below the soft limit.
    inline float SteadyLagDeg(const Params& p, float angularSpeedDegPerSec)
    {
        return 2.0f * p.rotationZeta / (2.0f * kPi * p.rotationHz) * angularSpeedDegPerSec;
    }

    inline bool NameEquals(const char* name, const char* expected)
    {
        for (; *name && *expected; ++name, ++expected)
            if (std::tolower(static_cast<unsigned char>(*name)) != *expected) return false;
        return *name == '\0' && *expected == '\0';
    }

    // Short weapons (knife, tonfa, machete, frying pan, riot shield) are
    // dagger-like; the rest sword-like. Swing gates differ by this.
    constexpr float kShortWeaponReach = 0.7f;
    inline bool ShortWeapon(const Params& p) { return p.reach < kShortWeaponReach; }

    struct MeleeProfile
    {
        const char* name;
        float reach;     // m
        float hz;
        float zeta;
        float maxLagDeg;
    };

    // Per weapon: natural frequency ~ 11.1 / effective reach (Hz; blades x1.0,
    // blunt x1.1, head-heavy x1.3, kept within 8..17 Hz) and damping ratio
    // ~ 0.072 x frequency, so damping / stiffness stays ~23 ms for every
    // weapon: every weapon lags the same amount at a given speed, heavier ones
    // build it up and settle more slowly.
    inline const MeleeProfile* MeleeProfiles(std::size_t& count)
    {
        static const MeleeProfile profiles[] = {
            { "knife", 0.30f, 17.0f, 1.20f, 15.0f },
            { "tonfa", 0.55f, 15.0f, 1.10f, 18.0f },
            { "machete", 0.65f, 15.0f, 1.10f, 18.0f },
            { "frying_pan", 0.50f, 13.0f, 0.95f, 20.0f },
            { "riotshield", 0.60f, 10.0f, 1.00f, 15.0f },
            { "crowbar", 0.75f, 13.0f, 0.95f, 20.0f },
            { "katana", 1.00f, 11.3f, 0.81f, 20.0f },
            { "baseball_bat", 0.85f, 11.0f, 0.80f, 20.0f },
            { "cricket_bat", 0.95f, 10.5f, 0.76f, 22.0f },
            { "golfclub", 1.00f, 10.0f, 0.72f, 22.0f },
            { "electric_guitar", 1.00f, 9.0f, 0.65f, 25.0f },
            { "fireaxe", 0.90f, 9.0f, 0.65f, 25.0f },
            { "shovel", 1.20f, 8.5f, 0.62f, 25.0f },
            { "sledgehammer", 0.90f, 8.0f, 0.58f, 25.0f },
            { "pitchfork", 1.40f, 8.0f, 0.58f, 25.0f },
            { "chainsaw", 0.70f, 7.0f, 1.00f, 20.0f },
        };
        count = sizeof(profiles) / sizeof(profiles[0]);
        return profiles;
    }

    // Unknown (custom) melee weapons: a sword-like medium weapon.
    inline Params DefaultParams() { return Params{}; }

    // Maps L4D2 melee script names ("fireaxe", "katana", ...) to their
    // parameters. A "weapon_" prefix is ignored, matching is case-insensitive.
    inline Params ParamsForMeleeScriptName(const char* name)
    {
        Params p = DefaultParams();
        if (!name) return p;
        const char prefix[] = "weapon_";
        if (std::strlen(name) > sizeof(prefix) - 1)
        {
            bool prefixed = true;
            for (std::size_t i = 0; i + 1 < sizeof(prefix); ++i)
                if (std::tolower(static_cast<unsigned char>(name[i])) != prefix[i]) { prefixed = false; break; }
            if (prefixed) name += sizeof(prefix) - 1;
        }
        std::size_t count = 0;
        const MeleeProfile* const profiles = MeleeProfiles(count);
        for (std::size_t i = 0; i < count; ++i)
        {
            if (!NameEquals(name, profiles[i].name)) continue;
            p.reach = profiles[i].reach;
            p.rotationHz = profiles[i].hz;
            p.rotationZeta = profiles[i].zeta;
            p.maxLagDeg = profiles[i].maxLagDeg;
            p.motionMelee = !NameEquals(name, "chainsaw"); // runs on the trigger, never swing-triggered
            break;
        }
        return p;
    }

    // Where swing speed and travel are measured: 73 % of the reach (Ancient
    // Dungeon's sword samples its tip there), at least 0.5 m: its dagger is
    // measured even further out (past its own tip), so short weapons need no
    // more arm travel than a dagger.
    inline float TipSampleDistance(const Params& p) { return std::max(0.5f, 0.73f * p.reach); }

    // Pivots a weapon pose about its grip: `viewmodel` is the unlagged pose,
    // `grip` a point on it (the hand), `rotation` the world-frame lag rotation.
    inline Pose PivotAboutGrip(const Pose& viewmodel, const Vector& grip, const Quat& rotation)
    {
        return Pose{ grip + QuatRotate(rotation, viewmodel.position - grip),
            QuatNormalize(QuatMultiply(rotation, viewmodel.orientation)) };
    }

    // ------------------------------------------------------------------- spring
    struct State
    {
        Vector position{ 0.0f, 0.0f, 0.0f };        // grip (welded: the target's)
        Vector velocity{ 0.0f, 0.0f, 0.0f };        // m/s, world
        Quat orientation{};
        Vector angularVelocity{ 0.0f, 0.0f, 0.0f }; // rad/s, world
    };

    constexpr float kMaxStepDt = 0.05f;      // longer frames (hitches) are clamped
    constexpr float kMaxSubstepDt = 1.0f / 1000.0f;

    inline Vector ClampLength(const Vector& v, float maxLength)
    {
        const float length = v.Length();
        return length > maxLength && length > 0.0f ? v * (maxLength / length) : v;
    }

    // The angular velocity is solved implicitly (backward Euler on the
    // spring-damper, stable for any step) and the orientation advanced with
    // the new velocity. Each frame is split into 1 ms substeps along which the
    // goal moves from the last frame's controller orientation to this one's,
    // so the lag does not depend on the frame rate.
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
        // Angle between the weapon and the last goal (the controller), degrees.
        float LagDeg() const { return m_Initialized ? QuatAngleBetween(m_Goal, m_State.orientation) * kRadToDeg : 0.0f; }

        // Snaps to the pose with zero velocity.
        bool Reset(const Pose& pose)
        {
            if (!PoseFinite(pose)) return false;
            m_State = State{};
            m_State.position = pose.position;
            m_State.orientation = QuatNormalize(pose.orientation);
            m_Goal = m_State.orientation;
            m_Initialized = true;
            return true;
        }
        void Clear() { m_State = State{}; m_Initialized = false; }

        // Advances toward the controller target (the grip pose). Invalid input
        // leaves the state unchanged.
        bool Step(const Pose& target, float dt)
        {
            if (!PoseFinite(target) || !std::isfinite(dt)) return false;
            if (!m_Initialized || (target.position - m_State.position).Length() > m_Params.snapDistance)
                return Reset(target);
            dt = std::clamp(dt, 0.0f, kMaxStepDt);
            if (dt <= 0.0f) return true;
            Quat goal1 = QuatNormalize(target.orientation);
            if (QuatDot(goal1, m_Goal) < 0.0f) goal1 = Quat{ -goal1.w, -goal1.x, -goal1.y, -goal1.z };
            const Quat goal0 = m_Goal;
            const int steps = std::max(1, static_cast<int>(std::ceil(dt / kMaxSubstepDt - 1e-4f)));
            const float h = dt / static_cast<float>(steps);
            const float w = 2.0f * kPi * m_Params.rotationHz;
            const float k = w * w, c = 2.0f * m_Params.rotationZeta * w;
            const float limit = m_Params.maxLagDeg * kDegToRad;
            const float kl = m_Params.limitStiffness * k, cl = m_Params.limitDamping * c;
            const float maxAngular = m_Params.maxAngularSpeedDegPerSec * kDegToRad;
            // The goal's own angular velocity over this frame (world frame).
            const Vector goalVelocity = QuatLog(QuatMultiply(goal1, QuatConjugate(goal0))) * (1.0f / dt);

            if (m_Params.positionLocked)
            {
                m_State.velocity = (target.position - m_State.position) * (1.0f / dt);
                m_State.position = target.position;
            }
            const float wp = 2.0f * kPi * m_Params.positionHz;
            const float dp = (1.0f + wp * h) * (1.0f + wp * h);
            for (int i = 0; i < steps; ++i)
            {
                if (!m_Params.positionLocked)
                {
                    const Vector error = m_State.position - target.position;
                    m_State.velocity = ClampLength((m_State.velocity - error * (h * wp * wp)) * (1.0f / dp),
                        m_Params.maxLinearSpeed);
                    m_State.position += m_State.velocity * h;
                }

                const Quat goal = QuatSlerp(goal0, goal1, static_cast<float>(i + 1) / static_cast<float>(steps));
                // Rotation vector from the goal to the weapon (the lag, world frame).
                const Vector error = QuatLog(QuatMultiply(m_State.orientation, QuatConjugate(goal)));
                const float angle = error.Length();
                float stiffness = k, damping = c;
                Vector force = error * k; // spring force (restoring: subtracted)
                if (limit > 0.0f && angle > limit)
                {
                    // Soft limit: an extra spring on the excess and extra damping
                    // of the lag rate (weapon minus goal velocity), so the limit
                    // holds the lag instead of dragging the blade further behind.
                    stiffness += kl;
                    damping += cl;
                    force += error * (kl * (angle - limit) / angle) - goalVelocity * cl;
                }
                // v' = v - h * (force + damping * v') - h^2 * stiffness * v' (implicit in v').
                m_State.angularVelocity = ClampLength((m_State.angularVelocity - force * h) *
                    (1.0f / (1.0f + damping * h + stiffness * h * h)), maxAngular);
                m_State.orientation = QuatNormalize(QuatMultiply(QuatExp(m_State.angularVelocity * h),
                    m_State.orientation));
            }
            m_Goal = goal1;
            if (!Finite(m_State.position) || !Finite(m_State.velocity) || !QuatFinite(m_State.orientation) ||
                !Finite(m_State.angularVelocity) ||
                QuatAngleBetween(m_State.orientation, goal1) > m_Params.snapAngleDeg * kDegToRad)
                return Reset(target);
            return true;
        }

        // Lagged pose in the controller's local frame, for re-application to a fresher pose.
        Pose Delta(const Pose& controller) const { return LagDelta(controller, Lagged()); }

    private:
        Params m_Params{};
        State m_State{};
        Quat m_Goal{};
        bool m_Initialized = false;
    };
}
