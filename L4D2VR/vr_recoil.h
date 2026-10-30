#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// Pavlov VR style weapon recoil (std-only, see docs/physical-overhaul.md
// "Recoil"). Recoil is a kinematic offset of the gun relative to the hand
// that holds it: the controller is never moved, so the muzzle (and the aim
// ray the bullets use) really kicks and the player has to counter it.
//
// The offset has three parts, in the gun's own frame:
//   backCm   translation along the gun's forward axis (negative = toward the shooter)
//   pitchDeg rotation about the gun's right axis (positive = muzzle up)
//   yawDeg   rotation about the gun's up axis (positive = muzzle right)
//
// Two paths, chosen per gun:
//  - dynamic: each shot starts a recoil instance that plays a velocity curve
//    (~0.125 s). Live instances are summed, scaled per axis and integrated
//    into the offset, clamped to per-gun limits. Once the kick velocity is
//    zero a counter-recoil curve carries the offset back to rest. A heat value
//    (0..1) builds while kicking and only scales the sideways kick, so the
//    first shot goes nearly straight up and a burst wanders more and more; the
//    side sign follows a pattern indexed by how long the trigger has been held.
//  - static (bolt-action rifles): a position curve over one second after each
//    shot, no limits or heat.
// Holding the foregrip (two-handed) changes only multipliers and limits: a
// gun that needs two hands kicks much harder held in one hand, and every gun
// held in one hand gets the wide one-handed limits.
namespace l4d2vr_recoil
{
    enum class Interp : std::uint8_t { Cubic, Linear, Constant };

    struct CurveKey
    {
        float t;
        float v;
        Interp interp;
        float arrive; // arrive tangent (value per second)
        float leave;  // leave tangent
    };

    struct Curve
    {
        const CurveKey* keys = nullptr;
        std::size_t count = 0;
    };

    // Rich-curve evaluation: constant extrapolation at both ends, cubic
    // segments as Bezier curves with unweighted tangents (control points a
    // third of the segment along each tangent).
    inline float Eval(const Curve& curve, float t)
    {
        if (!curve.keys || curve.count == 0 || !std::isfinite(t))
            return 0.0f;
        const CurveKey* k = curve.keys;
        if (t <= k[0].t) return k[0].v;
        if (t >= k[curve.count - 1].t) return k[curve.count - 1].v;
        for (std::size_t i = 0; i + 1 < curve.count; ++i)
        {
            const CurveKey& a = k[i];
            const CurveKey& b = k[i + 1];
            if (t < a.t || t >= b.t)
                continue;
            const float d = b.t - a.t;
            if (!(d > 0.0f) || a.interp == Interp::Constant)
                return a.v;
            const float s = (t - a.t) / d;
            if (a.interp == Interp::Linear)
                return a.v + (b.v - a.v) * s;
            const float p0 = a.v, p1 = a.v + a.leave * d / 3.0f, p2 = b.v - b.arrive * d / 3.0f, p3 = b.v;
            const float q0 = p0 + (p1 - p0) * s, q1 = p1 + (p2 - p1) * s, q2 = p2 + (p3 - p2) * s;
            const float r0 = q0 + (q1 - q0) * s, r1 = q1 + (q2 - q1) * s;
            return r0 + (r1 - r0) * s;
        }
        return k[curve.count - 1].v;
    }

    template <std::size_t N>
    constexpr Curve MakeCurve(const CurveKey (&keys)[N]) { return Curve{ keys, N }; }

    enum class SideCurve : std::uint8_t { None, Misc, Pistol };

    struct Profile
    {
        int weaponId;
        const char* pavlovGun;
        bool dynamic;            // bDynamicRecoil
        bool patternHeld;        // automatic/burst: the pattern clock keeps running while the trigger is held
        float fireRate;          // s
        float angleMul;          // RecoilAngleMul
        float lateralMul;        // RecoilAngleLateralMul
        float transMul;          // RecoilTraslationMul
        float recoilMul;         // RecoilMul (side)
        SideCurve sideCurve;     // RecoilCurve(RecoilRatio)
        float perBullet;         // RecoilPerBullet (heat per second of kicking)
        float normalizationRate; // RecoilNormalizationRate (heat decay per second)
        float counterDuration;   // CounterRecoilDuration (s)
        float pullback;          // CounterRecoilPullback (retention per 1/72 s at the rear limit)
        float counterThreshold;  // CounterRecoilThreashold
        float limits[3];         // CounterRecoilLimits (cm, deg, deg)
        float grippedLimitMul[3];
        float unsteadyAngleMul;
        float unsteadyTransMul;
        bool needsTwoHands;
        bool twoHandSupportOnly;
        float crouchSideMul;
        float curveScale;        // RecoilCurveScale (static path)
        float smoothLimitsExp;   // SmoothCounterRecoilLimitsExp
        bool applyPullback;
        bool singleShotDrop;     // Pavlov drops these when fired one-handed (not copied)
    };
}

#include "vr_recoil_data.h"

namespace l4d2vr_recoil
{
    inline Curve DynamicCurve(int axis)
    {
        return axis == 0 ? MakeCurve(data::kDynamicRecoilX) : axis == 1 ? MakeCurve(data::kDynamicRecoilY) : MakeCurve(data::kDynamicRecoilZ);
    }
    inline Curve GenericCurve(int axis)
    {
        return axis == 0 ? MakeCurve(data::kGenericRecoilX) : axis == 1 ? MakeCurve(data::kGenericRecoilY) : MakeCurve(data::kGenericRecoilZ);
    }
    inline Curve CounterCurve(int axis)
    {
        return axis == 0 ? MakeCurve(data::kCounterRecoilX) : axis == 1 ? MakeCurve(data::kCounterRecoilY) : MakeCurve(data::kCounterRecoilZ);
    }
    inline Curve PatternCurve() { return MakeCurve(data::kRecoilPattern); }
    inline Curve SideCurveFor(SideCurve which)
    {
        switch (which)
        {
        case SideCurve::Misc: return MakeCurve(data::kSideMisc);
        case SideCurve::Pistol: return MakeCurve(data::kSidePistol);
        default: return Curve{};
        }
    }

    inline const Profile* ProfileForWeapon(int weaponId)
    {
        for (const Profile& p : kProfiles)
            if (p.weaponId == weaponId)
                return &p;
        return nullptr;
    }

    struct Offset
    {
        float backCm = 0.0f;
        float pitchDeg = 0.0f;
        float yawDeg = 0.0f;
    };

    struct Inputs
    {
        bool twoHanded = false;   // support hand on the foregrip
        bool triggerHeld = false; // the trigger is down this tick
        bool crouched = false;    // head below the crouch height
    };

    // Small deterministic generator, so tests (and replays) are repeatable.
    struct Rng
    {
        std::uint32_t state = 0x9E3779B9u;
        float Next01()
        {
            std::uint32_t x = state ? state : 0x9E3779B9u;
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            state = x;
            return static_cast<float>(x >> 8) * (1.0f / 16777216.0f);
        }
        bool Coin() { return Next01() < 0.5f; }
    };

    class State
    {
    public:
        static constexpr float kMaxSubstep = 1.0f / 120.0f;
        static constexpr int kMaxSubsteps = 16;
        static constexpr float kInstanceLifetime = 1.0f;
        static constexpr float kPatternWrapAt = 3.0f;
        static constexpr float kPatternWrapTo = 1.5f;
        static constexpr float kPullbackRate = 72.0f;
        static constexpr float kPullbackZone = 0.3f; // cm from the rear limit

        void Reset(const Profile* profile, const Inputs& inputs = {})
        {
            m_Profile = profile;
            m_Instances.clear();
            m_D[0] = m_D[1] = m_D[2] = 0.0f;
            m_Saved[0] = m_Saved[1] = m_Saved[2] = 0.0f;
            m_Timer = 0.0f;
            m_Jitter = 0.0f;
            m_Ratio = 0.0f;
            m_PatternClock = 0.0f;
            m_Idle = 0.0f;
            m_SinceShot = 1.0e9f;
            m_StaticYawSign = 1.0f;
            m_Shots = 0;
            if (profile)
                TargetLimits(*profile, inputs, m_Lim); // held long enough: settled limits
        }
        void Seed(std::uint32_t seed) { m_Rng.state = seed ? seed : 1u; }
        const Profile* GetProfile() const { return m_Profile; }

        // One shot (one trigger pull for a shotgun, not one per pellet).
        void Fire()
        {
            if (!m_Profile)
                return;
            const bool invert = m_Rng.Coin();
            m_StaticYawSign = invert ? -1.0f : 1.0f;
            if (m_Profile->dynamic)
                m_Instances.push_back(Instance{ 0.0f, Eval(PatternCurve(), m_PatternClock) });
            m_SinceShot = 0.0f;
            ++m_Shots;
        }

        Offset Tick(float dt, const Inputs& in)
        {
            if (!m_Profile || !std::isfinite(dt) || dt <= 0.0f)
                return Current(in);
            const Profile& p = *m_Profile;
            dt = (std::min)(dt, 0.25f);

            m_PatternClock += dt;
            if (m_PatternClock >= kPatternWrapAt)
                m_PatternClock = kPatternWrapTo;
            if (in.triggerHeld && p.patternHeld)
                m_Idle = 0.0f;
            else
            {
                m_Idle += dt;
                if (m_Idle > 2.0f * p.fireRate)
                    m_PatternClock = 0.0f;
            }
            m_SinceShot += dt;
            if (!p.dynamic)
                return Current(in);

            float remaining = dt;
            for (int i = 1; remaining >= 1.0e-6f && i <= kMaxSubsteps; ++i)
            {
                float h = remaining;
                if (remaining > kMaxSubstep && i < kMaxSubsteps)
                    h = (std::min)(remaining * 0.5f, kMaxSubstep);
                h = (std::max)(h, 1.0e-6f);
                remaining -= h;
                Substep(p, h, in);
            }
            m_Ratio = std::clamp(m_Ratio, 0.0f, 1.0f);
            return Current(in);
        }

        Offset Current(const Inputs& in) const
        {
            Offset o{};
            if (!m_Profile)
                return o;
            const Profile& p = *m_Profile;
            if (p.dynamic)
            {
                o.backCm = m_D[0];
                o.pitchDeg = m_D[1];
                o.yawDeg = m_D[2];
                return o;
            }
            if (m_SinceShot > 1.0e8f)
                return o;
            const float t = m_SinceShot * p.curveScale;
            o.backCm = Eval(GenericCurve(0), t) * TransMul(p, in);
            o.pitchDeg = Eval(GenericCurve(1), t) * AngleMul(p, in);
            o.yawDeg = Eval(GenericCurve(2), t) * AngleMul(p, in) * p.lateralMul * m_StaticYawSign;
            return o;
        }

        float Heat() const { return m_Ratio; }
        float PatternClock() const { return m_PatternClock; }
        bool Kicking() const { return m_Kicking; }
        int Shots() const { return m_Shots; }
        const float* Limits() const { return m_Lim; }

        static bool Unsteady(const Profile& p, const Inputs& in) { return p.needsTwoHands && !in.twoHanded; }
        static float AngleMul(const Profile& p, const Inputs& in)
        {
            return p.angleMul * (Unsteady(p, in) ? p.unsteadyAngleMul : 1.0f);
        }
        static float TransMul(const Profile& p, const Inputs& in)
        {
            return p.transMul * (Unsteady(p, in) ? p.unsteadyTransMul : 1.0f);
        }
        float SideMul(const Profile& p, const Inputs& in) const
        {
            return Eval(SideCurveFor(p.sideCurve), m_Ratio) * p.recoilMul * (in.crouched ? p.crouchSideMul : 1.0f);
        }
        static void TargetLimits(const Profile& p, const Inputs& in, float out[3])
        {
            if (!in.twoHanded)
            {
                out[0] = 2.0f * p.limits[0];
                out[1] = 45.0f;
                out[2] = 90.0f;
                return;
            }
            for (int k = 0; k < 3; ++k)
                out[k] = p.limits[k] * p.grippedLimitMul[k];
        }

    private:
        struct Instance
        {
            float t;
            float side;
        };

        void Substep(const Profile& p, float h, const Inputs& in)
        {
            float s[3] = { 0.0f, 0.0f, 0.0f };
            const Curve cx = DynamicCurve(0), cy = DynamicCurve(1), cz = DynamicCurve(2);
            for (Instance& inst : m_Instances)
            {
                inst.t += h;
                s[0] += Eval(cx, inst.t);
                s[1] += Eval(cy, inst.t);
                s[2] += Eval(cz, inst.t) * inst.side;
            }
            m_Instances.erase(std::remove_if(m_Instances.begin(), m_Instances.end(),
                [](const Instance& i) { return i.t > kInstanceLifetime; }), m_Instances.end());
            for (float& x : s)
                x = std::clamp(x, -2.0f, 2.0f);

            const float angleMul = AngleMul(p, in);
            const float v[3] = {
                s[0] * TransMul(p, in),
                s[1] * angleMul,
                s[2] * angleMul * p.lateralMul * SideMul(p, in),
            };
            for (int k = 0; k < 3; ++k)
                m_D[k] += h * v[k];

            // The limits ease toward their target (grabbing or letting go of
            // the foregrip ramps them instead of snapping).
            float target[3];
            TargetLimits(p, in, target);
            const float d2 = (target[0] - m_Lim[0]) * (target[0] - m_Lim[0]) +
                (target[1] - m_Lim[1]) * (target[1] - m_Lim[1]) + (target[2] - m_Lim[2]) * (target[2] - m_Lim[2]);
            if (d2 < 1.0e-4f || p.smoothLimitsExp <= 0.0f)
                for (int k = 0; k < 3; ++k) m_Lim[k] = target[k];
            else
            {
                const float a = std::clamp(h * p.smoothLimitsExp, 0.0f, 1.0f);
                for (int k = 0; k < 3; ++k) m_Lim[k] += (target[k] - m_Lim[k]) * a;
            }
            for (int k = 0; k < 3; ++k)
                m_D[k] = std::clamp(m_D[k], -m_Lim[k], m_Lim[k]);

            if (p.applyPullback && std::fabs(m_D[0] + m_Lim[0]) <= kPullbackZone)
                m_D[0] *= std::pow(p.pullback, kPullbackRate * h);

            const float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            m_Kicking = speed > p.counterThreshold;
            if (m_Kicking)
            {
                for (int k = 0; k < 3; ++k) m_Saved[k] = m_D[k];
                m_Timer = 0.0f;
                const float sign = m_Rng.Coin() ? -0.025f : 0.025f;
                const int steps = (std::min)(static_cast<int>(m_Rng.Next01() * 3.0f), 2);
                m_Jitter = static_cast<float>(steps) * sign;
                m_Ratio += h * p.perBullet;
            }
            else
            {
                m_Timer += h;
                const float duration = (std::max)(0.01f, p.counterDuration + m_Jitter);
                const float a = std::clamp(m_Timer, 0.0f, duration) / duration;
                for (int k = 0; k < 3; ++k)
                    m_D[k] = (1.0f - Eval(CounterCurve(k), a)) * m_Saved[k];
                if (p.normalizationRate > 0.0f && m_Ratio * m_Ratio >= 1.0e-8f)
                    m_Ratio += std::clamp(p.normalizationRate * h, 0.0f, 1.0f) * (0.0f - m_Ratio);
                else
                    m_Ratio = 0.0f;
            }
        }

        const Profile* m_Profile = nullptr;
        std::vector<Instance> m_Instances;
        float m_D[3] = { 0.0f, 0.0f, 0.0f };
        float m_Saved[3] = { 0.0f, 0.0f, 0.0f };
        float m_Lim[3] = { 0.0f, 0.0f, 0.0f };
        float m_Timer = 0.0f;
        float m_Jitter = 0.0f;
        float m_Ratio = 0.0f;
        float m_PatternClock = 0.0f;
        float m_Idle = 0.0f;
        float m_SinceShot = 1.0e9f;
        float m_StaticYawSign = 1.0f;
        bool m_Kicking = false;
        int m_Shots = 0;
        Rng m_Rng{};
    };

    // Applies an offset to a gun basis (forward/right/up, any handedness of
    // "right" as long as up = right x forward is not assumed): pitch about the
    // gun's right axis (muzzle up), then yaw about the gun's up axis (muzzle
    // right), in the gun's own frame; the translation runs along the kicked
    // forward axis. Vec needs +, -, scalar * and is passed by value.
    template <typename Vec>
    inline void RotateAbout(Vec& v, const Vec& axis, float degrees)
    {
        // Rodrigues with a unit axis.
        const float r = degrees * 0.017453292519943295f;
        const float c = std::cos(r), s = std::sin(r);
        const float d = v.x * axis.x + v.y * axis.y + v.z * axis.z;
        const Vec cross{ axis.y * v.z - axis.z * v.y, axis.z * v.x - axis.x * v.z, axis.x * v.y - axis.y * v.x };
        v = Vec{ v.x * c + cross.x * s + axis.x * d * (1.0f - c),
                 v.y * c + cross.y * s + axis.y * d * (1.0f - c),
                 v.z * c + cross.z * s + axis.z * d * (1.0f - c) };
    }

    // forward/right/up: unit gun axes with right pointing to the gun's right
    // and up to its top. Muzzle-up pitch turns forward toward up; muzzle-right
    // yaw turns forward toward right.
    template <typename Vec>
    inline void ApplyToBasis(const Offset& o, Vec& forward, Vec& right, Vec& up)
    {
        // Pitch: rotate forward and up about right so forward moves toward up.
        // With right = forward x up, right x forward = up, so a positive
        // rotation about right lifts the muzzle.
        const Vec fxU{ forward.y * up.z - forward.z * up.y, forward.z * up.x - forward.x * up.z, forward.x * up.y - forward.y * up.x };
        const float handed = (fxU.x * right.x + fxU.y * right.y + fxU.z * right.z) >= 0.0f ? 1.0f : -1.0f;
        RotateAbout(forward, right, o.pitchDeg * handed);
        RotateAbout(up, right, o.pitchDeg * handed);
        // Yaw: rotate forward and right about up so forward moves toward right.
        // up x forward = -right, so a positive rotation about up turns the
        // muzzle left: negate.
        RotateAbout(forward, up, -o.yawDeg * handed);
        RotateAbout(right, up, -o.yawDeg * handed);
    }

    // Shot bookkeeping: one kick per real shot. Client prediction can replay a
    // command; a replay carries a command number at or below the last kicked
    // one and must not kick again.
    struct ShotGate
    {
        int lastCommand = 0;
        bool Accept(int commandNumber)
        {
            if (commandNumber <= 0)
                return true; // unknown command: count it
            if (commandNumber <= lastCommand)
                return false;
            lastCommand = commandNumber;
            return true;
        }
        void Reset() { lastCommand = 0; }
    };
}
