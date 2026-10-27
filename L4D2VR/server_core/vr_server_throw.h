#pragma once

// Manual throw velocity for the dedicated server: per-player controller pose
// ring keyed by usercmd tick and the release-velocity estimate. A std-only
// port of ManualThrowRecordPoseSample / ManualThrowEstimateReleaseVelocity /
// ManualThrowPreparePending (L4D2VR/hooks/hooks_throwables.inl).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "vr_server_limits.h"
#include "vr_server_math.h"

namespace l4d2vr_server
{
    struct ThrowPoseSample
    {
        bool valid = false;
        bool hasRelative = false;
        int tick = 0;
        Vec3 position{};
        Vec3 relative{};       // position relative to the player origin
        Angles angles{};
    };

    class ThrowPoseRing
    {
    public:
        static constexpr int kCapacity = 16;

        void Clear() { m_Samples = {}; m_Count = 0; m_LastTick = 0; }
        int Count() const { return m_Count; }
        int LastTick() const { return m_LastTick; }
        const ThrowPoseSample& At(int i) const { return m_Samples[static_cast<std::size_t>(i)]; }

        void Record(int tick, const Vec3& position, const Vec3& relative, bool hasRelative, const Angles& angles)
        {
            if (tick <= 0 || !position.Finite() || !angles.Finite())
                return;
            hasRelative = hasRelative && relative.Finite();
            if (m_LastTick > 0 && (tick > m_LastTick + 16 || tick < m_LastTick - 16))
                Clear();
            for (int i = 0; i < m_Count; ++i)
            {
                ThrowPoseSample& s = m_Samples[static_cast<std::size_t>(i)];
                if (s.valid && s.tick == tick)
                {
                    s.position = position;
                    s.hasRelative = hasRelative;
                    s.relative = hasRelative ? relative : Vec3{};
                    s.angles = angles;
                    m_LastTick = std::max(m_LastTick, tick);
                    return;
                }
            }
            ThrowPoseSample sample{};
            sample.valid = true;
            sample.hasRelative = hasRelative;
            sample.tick = tick;
            sample.position = position;
            sample.relative = hasRelative ? relative : Vec3{};
            sample.angles = angles;
            if (m_Count < kCapacity)
                m_Samples[static_cast<std::size_t>(m_Count++)] = sample;
            else
                m_Samples[0] = sample;    // oldest after sorting
            std::sort(m_Samples.begin(), m_Samples.begin() + m_Count,
                [](const ThrowPoseSample& a, const ThrowPoseSample& b) { return a.tick < b.tick; });
            m_LastTick = std::max(m_LastTick, tick);
        }

        bool EstimateReleaseVelocity(int releaseTick, int windowTicks, float peakBlend, float tickSeconds,
            Vec3& linear, Vec3& angular) const
        {
            linear = {};
            angular = {};
            if (m_Count < 2 || releaseTick <= 0 || !(tickSeconds > 0.0f) || !std::isfinite(tickSeconds))
                return false;
            windowTicks = std::clamp(windowTicks, 1, kCapacity - 1);
            peakBlend = std::clamp(peakBlend, 0.0f, 1.0f);
            float totalWeight = 0.0f;
            Vec3 peak{};
            float peakSpeedSqr = -1.0f;
            for (int i = 1; i < m_Count; ++i)
            {
                const ThrowPoseSample& prev = m_Samples[static_cast<std::size_t>(i - 1)];
                const ThrowPoseSample& cur = m_Samples[static_cast<std::size_t>(i)];
                if (!prev.valid || !cur.valid || cur.tick > releaseTick) continue;
                if (releaseTick - prev.tick > windowTicks) continue;
                const int dt = cur.tick - prev.tick;
                if (dt <= 0 || dt > windowTicks) continue;
                const float seconds = static_cast<float>(dt) * tickSeconds;
                const bool relativeSegment = prev.hasRelative && cur.hasRelative;
                const Vec3& p0 = relativeSegment ? prev.relative : prev.position;
                const Vec3& p1 = relativeSegment ? cur.relative : cur.position;
                const Vec3 v = (p1 - p0) / seconds;
                const Vec3 w(AngleDelta(cur.angles.x, prev.angles.x) / seconds,
                    AngleDelta(cur.angles.y, prev.angles.y) / seconds,
                    AngleDelta(cur.angles.z, prev.angles.z) / seconds);
                if (!v.Finite() || !w.Finite()) continue;
                const int age = std::max(0, releaseTick - cur.tick);
                const float weight = 1.0f + static_cast<float>(std::max(0, windowTicks - age));
                linear += v * weight;
                angular += w * weight;
                totalWeight += weight;
                const float speedSqr = v.LengthSqr();
                if (std::isfinite(speedSqr) && speedSqr > peakSpeedSqr)
                {
                    peakSpeedSqr = speedSqr;
                    peak = v;
                }
            }
            if (totalWeight <= 0.0f)
                return false;
            linear = linear / totalWeight;
            angular = angular / totalWeight;
            if (peakSpeedSqr >= 0.0f && peakBlend > 0.0f)
                linear = linear * (1.0f - peakBlend) + peak * peakBlend;
            return linear.Finite() && angular.Finite();
        }

    private:
        std::array<ThrowPoseSample, kCapacity> m_Samples{};
        int m_Count = 0;
        int m_LastTick = 0;
    };

    struct ThrowRelease
    {
        bool valid = false;
        bool belowMinimum = false;   // too slow: keep the native velocity
        Vec3 origin{};
        Angles angles{};
        Vec3 velocity{};
        Vec3 angularVelocity{};
    };

    // ManualThrowPreparePending's velocity shaping. `hand`/`handAngles` are the
    // release command's controller pose.
    inline ThrowRelease ComputeThrowRelease(const ThrowPoseRing& ring, int releaseTick, const Vec3& hand,
        const Angles& handAngles, float tickSeconds, const ServerConfig& c)
    {
        ThrowRelease r{};
        Vec3 velocity{}, angular{};
        ring.EstimateReleaseVelocity(releaseTick, c.throwVelocityWindowTicks, c.throwPeakVelocityBlend,
            tickSeconds, velocity, angular);
        const float horizontal = c.throwVelocityScale * c.throwHorizontalScale;
        const float vertical = c.throwVelocityScale * c.throwVerticalScale;
        velocity.x *= horizontal;
        velocity.y *= horizontal;
        velocity.z *= vertical;
        const float horizontalSpeed = std::sqrt(velocity.x * velocity.x + velocity.y * velocity.y);
        if (std::isfinite(horizontalSpeed))
            velocity.z += horizontalSpeed * c.throwArcLiftRatio;
        ClampMagnitude(velocity, c.throwMaxSpeed);
        ClampMagnitude(angular, c.throwMaxAngularSpeed);

        Vec3 direction = velocity;
        if (Normalize(direction) <= 0.0001f)
        {
            AngleVectors(handAngles, &direction, nullptr, nullptr);
            if (Normalize(direction) <= 0.0001f)
                direction = Vec3(1.0f, 0.0f, 0.0f);
        }
        r.valid = hand.Finite() && handAngles.Finite() && velocity.Finite();
        r.belowMinimum = !(velocity.Length() >= c.throwMinSpeed);
        r.origin = hand + direction * 5.0f;
        r.angles = handAngles;
        r.velocity = velocity;
        r.angularVelocity = angular;
        return r;
    }

    // Release detection for grenades: trigger held on a throwable then let go.
    // Mirrors the listen host's throwableAimPrev* bookkeeping.
    class ThrowableReleaseTracker
    {
    public:
        // Returns the released weapon id, or 0.
        int Update(std::uintptr_t weaponTag, int weaponId, bool attackDown, bool throwable)
        {
            int released = 0;
            if (weaponTag != m_WeaponTag)
            {
                // A different weapon is not a release of the old one (the listen
                // host's identity check rejects that case too).
                m_WeaponTag = weaponTag;
            }
            else if (m_PrevThrowable && m_PrevAttack && !attackDown && throwable)
            {
                released = weaponId;
            }
            m_PrevAttack = throwable && attackDown;
            m_PrevThrowable = throwable;
            if (throwable) m_WeaponId = weaponId;
            return released;
        }
        void Reset() { *this = ThrowableReleaseTracker{}; }
    private:
        std::uintptr_t m_WeaponTag = 0u;
        int m_WeaponId = 0;
        bool m_PrevAttack = false;
        bool m_PrevThrowable = false;
    };
}
