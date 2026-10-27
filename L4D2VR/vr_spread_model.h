#pragma once
#include "sdk/vector.h"
#include <algorithm>
#include <cmath>
#include <limits>

// Dynamic weapon spread estimate for a desktop-style VR crosshair.
//
// All spread values are degrees, matching the L4D2 weapon script keys
// (MinStandingSpread, MinDuckingSpread, MinInAirSpread, MaxMovementSpread,
// SpreadPerShot, MaxSpread, SpreadDecay [deg/s]). A spread value is treated as
// the cone half-angle: the largest angular deviation of a pellet from the aim
// axis. The per-shot spread reported by the game's bullet function is ground
// truth and always wins over the estimate (see SpreadModel::OnShot).
namespace l4d2vr_spread
{
    inline constexpr float kPi = 3.14159265358979323846f;
    inline constexpr float kMaxGapDeg = 89.0f;

    struct WeaponSpreadData
    {
        float minStandingSpread = 0.0f;
        float minDuckingSpread = 0.0f;
        float minInAirSpread = 0.0f;
        float maxMovementSpread = 0.0f;
        float spreadPerShot = 0.0f;
        float maxSpread = 0.0f;
        float spreadDecay = 0.0f; // deg/s
    };

    struct Stance
    {
        bool onGround = true;
        bool ducking = false;
        float horizontalSpeed = 0.0f; // any unit, same as maxSpeed
        float maxSpeed = 0.0f;
    };

    inline bool NonNegativeFinite(float v) { return std::isfinite(v) && v >= 0.0f; }

    inline bool Valid(const WeaponSpreadData& d)
    {
        return NonNegativeFinite(d.minStandingSpread) && NonNegativeFinite(d.minDuckingSpread) &&
            NonNegativeFinite(d.minInAirSpread) && NonNegativeFinite(d.maxMovementSpread) &&
            NonNegativeFinite(d.spreadPerShot) && NonNegativeFinite(d.spreadDecay) &&
            std::isfinite(d.maxSpread) && d.maxSpread > 0.0f;
    }

    // Stance floor: ducking uses MinDuckingSpread, standing MinStandingSpread,
    // and being airborne overrides both with MinInAirSpread.
    inline float StanceMin(const WeaponSpreadData& d, const Stance& s)
    {
        if (!s.onGround) return d.minInAirSpread;
        return s.ducking ? d.minDuckingSpread : d.minStandingSpread;
    }

    // lerp(0, MaxMovementSpread, horizontalSpeed / maxSpeed), fraction clamped to [0,1].
    inline float MovementTerm(const WeaponSpreadData& d, const Stance& s)
    {
        if (!std::isfinite(s.horizontalSpeed) || !std::isfinite(s.maxSpeed) || !(s.maxSpeed > 0.0f))
            return 0.0f;
        const float fraction = std::clamp(s.horizontalSpeed / s.maxSpeed, 0.0f, 1.0f);
        return d.maxMovementSpread * fraction;
    }

    class SpreadModel
    {
    public:
        // Returns false (and disables the model) when the script data is unusable.
        bool SetWeapon(const WeaponSpreadData& data)
        {
            m_Valid = Valid(data);
            m_Data = m_Valid ? data : WeaponSpreadData{};
            m_Accumulated = m_Valid ? StanceMin(m_Data, m_Stance) : 0.0f;
            return m_Valid;
        }
        void Reset() { m_Valid = false; m_Data = {}; m_Stance = {}; m_Accumulated = 0.0f; }
        bool HasWeapon() const { return m_Valid; }
        const WeaponSpreadData& Data() const { return m_Data; }
        const Stance& CurrentStance() const { return m_Stance; }

        // Per frame. The accumulated (recoil) spread decays by SpreadDecay*dt
        // toward the stance floor; if the floor rises above it (e.g. jumping)
        // it follows the floor up immediately, then recovers at the decay rate.
        void Update(const Stance& stance, float dt)
        {
            m_Stance = stance;
            if (!std::isfinite(m_Stance.horizontalSpeed)) m_Stance.horizontalSpeed = 0.0f;
            if (!m_Valid) return;
            if (!std::isfinite(dt) || dt < 0.0f) dt = 0.0f;
            const float floor = StanceMin(m_Data, m_Stance);
            if (m_Accumulated <= floor)
            {
                m_Accumulated = floor;
                return;
            }
            m_Accumulated = (std::max)(floor, m_Accumulated - m_Data.spreadDecay * dt);
        }

        // Call once per fired shot. nativeSpreadDeg is the spread the game's bullet
        // function used for this shot (pass NaN when unavailable); it is ground
        // truth, so the estimate is raised to it before the per-shot kick.
        void OnShot(float nativeSpreadDeg = std::numeric_limits<float>::quiet_NaN())
        {
            if (!m_Valid) return;
            float base = (std::max)(m_Accumulated, StanceMin(m_Data, m_Stance));
            if (NonNegativeFinite(nativeSpreadDeg)) base = (std::max)(base, nativeSpreadDeg);
            m_Accumulated = (std::min)(base + m_Data.spreadPerShot, m_Data.maxSpread);
        }

        float Accumulated() const { return m_Valid ? m_Accumulated : 0.0f; }

        // max(stanceMin + movement, accumulated), clamped to MaxSpread.
        float Current() const
        {
            if (!m_Valid) return 0.0f;
            const float stance = StanceMin(m_Data, m_Stance) + MovementTerm(m_Data, m_Stance);
            return (std::min)((std::max)(stance, m_Accumulated), m_Data.maxSpread);
        }

    private:
        WeaponSpreadData m_Data{};
        Stance m_Stance{};
        float m_Accumulated = 0.0f;
        bool m_Valid = false;
    };

    // Angular radius of the crosshair gap (spread half-angle) in radians.
    // Invalid input yields 0; values are clamped below 90 degrees.
    inline float CrosshairGapRadians(float spreadDeg)
    {
        if (!NonNegativeFinite(spreadDeg)) return 0.0f;
        return (std::min)(spreadDeg, kMaxGapDeg) * (kPi / 180.0f);
    }

    // Gap radius in pixels on a symmetric per-eye projection whose full
    // horizontal field of view is eyeFovDeg across eyeWidthPx pixels.
    inline float ProjectGapPixels(float spreadDeg, float eyeFovDeg, float eyeWidthPx)
    {
        if (!std::isfinite(eyeFovDeg) || !(eyeFovDeg > 0.0f) || !(eyeFovDeg < 179.0f) ||
            !std::isfinite(eyeWidthPx) || !(eyeWidthPx > 0.0f))
            return 0.0f;
        const float halfTan = std::tan(eyeFovDeg * 0.5f * (kPi / 180.0f));
        const float px = std::tan(CrosshairGapRadians(spreadDeg)) / halfTan * (eyeWidthPx * 0.5f);
        return std::isfinite(px) ? px : 0.0f;
    }

    // Same, for an asymmetric HMD eye frustum given as tangent extents
    // (tanLeft < 0 < tanRight, e.g. from IVRSystem::GetProjectionRaw).
    inline float ProjectGapPixelsAsymmetric(float spreadDeg, float tanLeft, float tanRight, float eyeWidthPx)
    {
        if (!std::isfinite(tanLeft) || !std::isfinite(tanRight) || !(tanRight - tanLeft > 0.0001f) ||
            !std::isfinite(eyeWidthPx) || !(eyeWidthPx > 0.0f))
            return 0.0f;
        const float px = std::tan(CrosshairGapRadians(spreadDeg)) * eyeWidthPx / (tanRight - tanLeft);
        return std::isfinite(px) ? px : 0.0f;
    }

    // Radius of the spread cone at a given distance (same unit as distance).
    inline float WorldRadiusAtDistance(float spreadDeg, float distance)
    {
        if (!NonNegativeFinite(distance)) return 0.0f;
        const float r = distance * std::tan(CrosshairGapRadians(spreadDeg));
        return std::isfinite(r) ? r : 0.0f;
    }
}
