#pragma once
#include <algorithm>
#include <cmath>

// One Euro filter (Casiez et al.) for unit direction vectors: strong smoothing
// while the direction is nearly still, little lag while it moves quickly.
// Used to steady two-handed weapon aim the way Half-Life 2 VR does.
namespace l4d2vr_filter
{
    struct Dir3
    {
        float x = 1.0f, y = 0.0f, z = 0.0f;
    };

    struct OneEuroParams
    {
        float minCutoffHz = 1.2f;        // cutoff while still
        float beta = 1.5f;               // extra Hz per rad/s of angular speed
        float derivativeCutoffHz = 1.0f; // smoothing of the speed estimate
    };

    inline float OneEuroAlpha(float cutoffHz, float dt)
    {
        if (!(dt > 0.0f) || !(cutoffHz > 0.0f))
            return 1.0f;
        const float tau = 1.0f / (2.0f * 3.14159265358979f * cutoffHz);
        return 1.0f / (1.0f + tau / dt);
    }

    inline bool NormalizeDir(Dir3& d)
    {
        const float length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
        if (!std::isfinite(length) || length < 1e-6f)
            return false;
        d.x /= length;
        d.y /= length;
        d.z /= length;
        return true;
    }

    class OneEuroDirection
    {
    public:
        void Reset() { m_Valid = false; m_Speed = 0.0f; }
        bool Valid() const { return m_Valid; }
        float LastAlpha() const { return m_LastAlpha; }

        // Returns the filtered unit direction; `alphaOut` is the blend factor used,
        // so companion vectors (e.g. up) can be smoothed consistently.
        Dir3 Update(Dir3 input, float dt, const OneEuroParams& params)
        {
            if (!NormalizeDir(input))
                return m_Valid ? m_Value : input;
            if (!m_Valid || !(dt > 0.0f) || dt > 0.25f)
            {
                m_Value = input;
                m_Valid = true;
                m_Speed = 0.0f;
                m_LastAlpha = 1.0f;
                return m_Value;
            }
            const float dot = (std::max)(-1.0f, (std::min)(1.0f, input.x * m_Value.x + input.y * m_Value.y + input.z * m_Value.z));
            const float rawSpeed = std::acos(dot) / dt; // rad/s
            const float dAlpha = OneEuroAlpha(params.derivativeCutoffHz, dt);
            m_Speed += (rawSpeed - m_Speed) * dAlpha;
            const float cutoff = (std::max)(0.01f, params.minCutoffHz) + (std::max)(0.0f, params.beta) * m_Speed;
            m_LastAlpha = OneEuroAlpha(cutoff, dt);
            Dir3 next{ m_Value.x + (input.x - m_Value.x) * m_LastAlpha,
                       m_Value.y + (input.y - m_Value.y) * m_LastAlpha,
                       m_Value.z + (input.z - m_Value.z) * m_LastAlpha };
            if (NormalizeDir(next))
                m_Value = next;
            return m_Value;
        }

    private:
        Dir3 m_Value{};
        bool m_Valid = false;
        float m_Speed = 0.0f;
        float m_LastAlpha = 1.0f;
    };
}
