#pragma once
#include <cstdint>

// Always-on reload diagnostics. Hand-driven reload state changes are logged
// whatever VrHandsDebugLog says, so a headset session shows what the hands
// did; a limiter keeps a flapping state from flooding the log. Std-only so
// the tests can include it.
namespace l4d2vr_reload_log
{
    // Names for MagazineInteractionManualState, in declaration order.
    inline const char* StateName(int state)
    {
        static const char* const kNames[] = {
            "Idle", "HoldingOldMagazine", "WaitingForFreshMagazine", "HoldingFreshMagazine",
            "WaitingForBackendReload", "WaitingForBoltGrab", "HoldingBolt", "AutoBolting",
            "WaitingForServerShell", "WaitingForServerMagazine", "WaitingForServerPistolCycle",
            "HoldingFreshMagazineForSlap" };
        constexpr int kCount = static_cast<int>(sizeof(kNames) / sizeof(kNames[0]));
        return state >= 0 && state < kCount ? kNames[state] : "Unknown";
    }

    // At most `limit` lines per window. A line let through after a quiet
    // spell reports how many were dropped in between.
    class RateLimiter
    {
    public:
        explicit RateLimiter(unsigned limit = 12u, std::uint64_t windowMs = 1000u)
            : m_Limit(limit), m_WindowMs(windowMs) {}

        bool Allow(std::uint64_t nowMs, unsigned& droppedBefore)
        {
            droppedBefore = 0u;
            if (!m_Started || nowMs < m_WindowStart || nowMs - m_WindowStart >= m_WindowMs)
            {
                m_Started = true;
                m_WindowStart = nowMs;
                m_Count = 0u;
            }
            if (m_Count >= m_Limit)
            {
                ++m_Dropped;
                return false;
            }
            ++m_Count;
            droppedBefore = m_Dropped;
            m_Dropped = 0u;
            return true;
        }

    private:
        unsigned m_Limit;
        std::uint64_t m_WindowMs;
        bool m_Started = false;
        std::uint64_t m_WindowStart = 0u;
        unsigned m_Count = 0u;
        unsigned m_Dropped = 0u;
    };
}
