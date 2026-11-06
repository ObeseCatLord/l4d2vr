#pragma once
#include <cmath>
#include <cstdint>

// 2.0.9: one light haptic tick when a free hand first touches something it can
// grab (GrabHoverHapticEnabled): an item hanging on the body (holsters, the
// chest sling, a stowed item), the held gun's foregrip, bolt or pump for the
// other hand, a spare magazine or the magazine pouch. Each grab source keeps a
// Latch per physical hand; the pulse itself goes through a per-hand Gate so
// two sources entering together (a holster next to the pouch) tick once.
//
// std-only so the rules are unit-tested offline (tests/vr_grab_hover_tests.cpp).
namespace l4d2vr_grab_hover
{
    struct Settings
    {
        bool enabled = true;              // GrabHoverHapticEnabled
        float amplitude = 0.2f;           // GrabHoverHapticAmplitude
        float durationSeconds = 0.02f;    // GrabHoverHapticDurationMs / 1000
        float frequency = 150.0f;
        float exitMarginMeters = 0.025f;  // leave only this far outside the grab volume
        std::uint32_t exitDelayMs = 60u;  // ... for this long (contact-only sources)
        std::uint32_t minIntervalMs = 120u; // per hand, across sources
    };

    // One grab source for one hand. key: what is under the hand this frame
    // (0 = nothing in reach). overMeters: how far the hand is outside the
    // grab volume of the LATCHED target (<= 0 inside; +inf for sources that
    // only know touching or not). The latched target is kept while the hand is
    // within exitMarginMeters of it, and only let go after exitDelayMs outside
    // it, so the boundary never chatters. Returns true when a pulse is due: a
    // new target entered while the hand is free. Entries while the hand is
    // busy (holding something, or that grab disabled) are tracked silently, so
    // a hand that lets go of an item inside a volume does not tick.
    class Latch
    {
    public:
        bool Update(int key, float overMeters, bool handFree, std::uint64_t nowMs, const Settings& settings)
        {
            if (m_Key != 0)
            {
                const bool withinMargin = std::isfinite(overMeters) && overMeters <= settings.exitMarginMeters;
                if (withinMargin || key == m_Key)
                {
                    m_OutsideSinceMs = 0u;
                    m_OutsideStarted = false;
                    return false;
                }
                if (!m_OutsideStarted)
                {
                    m_OutsideStarted = true;
                    m_OutsideSinceMs = nowMs;
                }
                if (nowMs - m_OutsideSinceMs < settings.exitDelayMs)
                    return false;
                m_Key = 0;
                m_OutsideStarted = false;
            }
            if (key == 0)
                return false;
            m_Key = key;
            m_OutsideStarted = false;
            return handFree;
        }

        int Key() const { return m_Key; }
        void Reset() { m_Key = 0; m_OutsideStarted = false; m_OutsideSinceMs = 0u; }

    private:
        int m_Key = 0;
        bool m_OutsideStarted = false;
        std::uint64_t m_OutsideSinceMs = 0u;
    };

    // Per-hand rate limit across sources.
    class Gate
    {
    public:
        bool Allow(std::uint64_t nowMs, const Settings& settings)
        {
            if (m_Started && nowMs - m_LastMs < settings.minIntervalMs)
                return false;
            m_Started = true;
            m_LastMs = nowMs;
            return true;
        }

    private:
        bool m_Started = false;
        std::uint64_t m_LastMs = 0u;
    };

    // Grab sources (Latch index).
    enum class Source : int
    {
        Holster = 0,   // items on the body (holsters, sling, stowed) and the magazine pouch zone
        Foregrip = 1,  // the held gun's two-hand grip target
        Bolt = 2,      // the bolt / pump / slide to cycle
        Magazine = 3,  // the spare magazine or shell to take
        Pouch = 4,     // the magazine pouch (magazine interaction)
        Count = 5
    };
    constexpr int kSourceCount = static_cast<int>(Source::Count);
}
