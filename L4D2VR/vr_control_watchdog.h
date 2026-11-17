#pragma once
// 2.0.12 (s12 review E4 / issue 3 and every other way to end up without hands
// or control): a self-healing watchdog. Every frame the client samples the
// states that hide the hands or block the VR input; a condition that holds
// longer than its limit while the player is alive, in game and in first
// person (and not in a state where the base game itself takes the weapons
// away: ledge, pinned, mounted gun, a use action, third person) is reported
// once with the name of the stuck gate and a recovery is requested; while it
// stays stuck the recovery repeats with a back-off.
//
// std-only (tests/vr_control_watchdog_tests.cpp).
#include <cstdint>

namespace l4d2vr_control_watchdog
{
    enum Stuck : std::uint32_t
    {
        kNoActiveWeapon = 1u << 0,   // nothing in the hands at all (all items gone or refused)
        kNoViewmodel = 1u << 1,      // a weapon is held but no viewmodel (hands) was drawn
        kInputGate = 1u << 2,        // the VR grip/holster input is gated by an unexplained bit
        kControlNotReady = 1u << 3,  // first-person control never became ready
        kReloadStuck = 1u << 4,      // a reload state that waits for nothing
        kDrawRefused = 1u << 5,      // a holster draw the game did not perform
        kAll = (1u << 6) - 1u,
    };
    inline const char* Name(std::uint32_t bit)
    {
        switch (bit)
        {
        case kNoActiveWeapon: return "no-active-weapon";
        case kNoViewmodel: return "no-viewmodel(hands)";
        case kInputGate: return "input-gate";
        case kControlNotReady: return "first-person-control-not-ready";
        case kReloadStuck: return "reload-state-stuck";
        case kDrawRefused: return "holster-draw-refused";
        default: return "?";
        }
    }

    // ReadGripGameplaySession bits (hooks.cpp) that a live player in first
    // person can only have when something is stuck: no local-player state
    // (4), input suppressed (1024), control not ready (2048), an exception
    // (0x10000). The others are real states (dead/observer, incapacitated,
    // frozen, paused, a menu cursor, mouse mode, teleport scouting, pinned,
    // mounted gun, no weapon: handled as its own condition).
    constexpr std::uint32_t kUnexplainedGateBits = 4u | 1024u | 2048u | 0x10000u;

    // Limits (ms).
    constexpr std::uint64_t kNoWeaponMs = 2500u;
    constexpr std::uint64_t kNoViewmodelMs = 4000u;
    constexpr std::uint64_t kInputGateMs = 6000u;
    constexpr std::uint64_t kControlNotReadyMs = 8000u;
    constexpr std::uint64_t kReloadServerWaitMs = 6000u;  // a transaction answers within 2.5 s
    constexpr std::uint64_t kReloadIdleStateMs = 120000u; // any other non-idle state, unchanged
    constexpr std::uint64_t kDrawRefusedMs = 300u;
    constexpr std::uint64_t kRetryMs = 10000u;            // repeat a recovery while still stuck

    struct Sample
    {
        bool inGame = false;
        bool alive = false;            // live survivor, not observing
        bool firstPerson = false;      // no third-person state, no view entity
        bool paused = false;
        bool menuOpen = false;         // a UI cursor owns the input
        bool incapacitated = false;
        bool ledge = false;
        bool controlledBySI = false;
        bool mounted = false;
        bool useAction = false;        // healing, reviving, defib, using a device
        bool hasActiveWeapon = false;
        bool handsStowed = false;      // the item is put away on purpose (empty hands)
        std::uint64_t msSinceViewmodelDrawn = 0u;
        std::uint32_t gateBits = 0u;   // ReadGripGameplaySession().blocked
        bool controlReady = true;
        int reloadState = 0;           // MagazineInteractionManualState (0 = idle)
        bool reloadWaitsForServer = false;
        bool drawPending = false;      // a holster draw was requested ...
        std::uint64_t msSinceDrawRequest = 0u;
        bool drawArrived = false;      // ... and the requested item is now active
    };

    struct Result
    {
        std::uint32_t stuck = 0u;      // conditions over their limit now
        std::uint32_t recover = 0u;    // conditions to recover this frame (first time, then every kRetryMs)
        std::uint32_t cleared = 0u;    // conditions that recovered since the last frame
    };

    class Watchdog
    {
    public:
        void Reset() { *this = Watchdog{}; }
        Result Update(const Sample& s, std::uint64_t nowMs)
        {
            Result r{};
            // Only a live first-person player in game is watched; anything
            // else resets the timers (map change, death, spectating).
            const bool watched = s.inGame && s.alive && !s.paused;
            std::uint32_t holding = 0u;
            if (watched)
            {
                const bool gameTakesWeapons = s.ledge || s.controlledBySI || s.mounted || s.useAction;
                if (!s.hasActiveWeapon && !gameTakesWeapons && !s.handsStowed)
                    holding |= kNoActiveWeapon;
                if (s.hasActiveWeapon && s.firstPerson && !gameTakesWeapons && !s.handsStowed && !s.incapacitated &&
                    !s.menuOpen && s.msSinceViewmodelDrawn >= kNoViewmodelMs)
                    holding |= kNoViewmodel;
                if (s.firstPerson && !s.menuOpen && (s.gateBits & kUnexplainedGateBits) != 0u)
                    holding |= kInputGate;
                if (s.firstPerson && !s.controlReady)
                    holding |= kControlNotReady;
                if (s.drawPending && !s.drawArrived && s.msSinceDrawRequest >= kDrawRefusedMs)
                    holding |= kDrawRefused;
            }
            // Reload state: how long it has not changed.
            if (s.reloadState != m_ReloadState || !watched)
            {
                m_ReloadState = s.reloadState;
                m_ReloadSince = nowMs;
            }
            if (watched && s.reloadState != 0)
            {
                const std::uint64_t held = nowMs >= m_ReloadSince ? nowMs - m_ReloadSince : 0u;
                if (held >= (s.reloadWaitsForServer ? kReloadServerWaitMs : kReloadIdleStateMs))
                    holding |= kReloadStuck;
            }
            for (std::uint32_t bit = 1u; bit < kAll; bit <<= 1u)
            {
                Timer& t = m_Timers[Index(bit)];
                if ((holding & bit) == 0u)
                {
                    if (t.reported) r.cleared |= bit;
                    t = {};
                    continue;
                }
                if (!t.active) { t.active = true; t.since = nowMs; }
                const std::uint64_t held = nowMs >= t.since ? nowMs - t.since : 0u;
                // Draw refusals and reload states have their own clocks above.
                const bool over = bit == kDrawRefused || bit == kReloadStuck || held >= Limit(bit);
                if (!over) continue;
                r.stuck |= bit;
                if (!t.reported || (nowMs >= t.lastRecover && nowMs - t.lastRecover >= kRetryMs))
                {
                    r.recover |= bit;
                    t.reported = true;
                    t.lastRecover = nowMs;
                    ++m_Recoveries[Index(bit)];
                }
            }
            return r;
        }
        unsigned Recoveries(std::uint32_t bit) const { return m_Recoveries[Index(bit)]; }
        // After a recovery for a reload state, its clock restarts.
        void RestartReloadClock(std::uint64_t nowMs) { m_ReloadSince = nowMs; }
    private:
        struct Timer
        {
            bool active = false, reported = false;
            std::uint64_t since = 0u, lastRecover = 0u;
        };
        static int Index(std::uint32_t bit)
        {
            int i = 0;
            while (bit > 1u && i < 31) { bit >>= 1u; ++i; }
            return i < 6 ? i : 0;
        }
        static std::uint64_t Limit(std::uint32_t bit)
        {
            switch (bit)
            {
            case kNoActiveWeapon: return kNoWeaponMs;
            case kNoViewmodel: return kNoViewmodelMs;
            case kInputGate: return kInputGateMs;
            case kControlNotReady: return kControlNotReadyMs;
            default: return 0u;
            }
        }
        Timer m_Timers[6]{};
        unsigned m_Recoveries[6]{};
        int m_ReloadState = 0;
        std::uint64_t m_ReloadSince = 0u;
    };

    // Which inventory slot to deploy when nothing is in the hands: the
    // secondary first (the pistol has infinite ammo; melee always deploys),
    // then the primary, then the rest. occupiedMask: bit per slot (0..4).
    inline int PickRecoverySlot(unsigned occupiedMask, unsigned triedMask)
    {
        static const int order[] = { 1, 0, 2, 3, 4 };
        for (int slot : order)
            if ((occupiedMask & (1u << slot)) != 0u && (triedMask & (1u << slot)) == 0u) return slot;
        for (int slot : order)
            if ((occupiedMask & (1u << slot)) != 0u) return slot;
        return -1;
    }
}
