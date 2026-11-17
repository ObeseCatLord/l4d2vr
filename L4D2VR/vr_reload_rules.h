#pragma once
// 2.0.12: pure reload rules shared by the VR hand bridge, CreateMove and the
// tests (no game types). Numbers from the Pavlov VR research (mechanics only):
// one shell per squeeze, at most one every 0.15 s on shotguns (0.25 s other
// guns), nothing from the pouch with an empty reserve or without the gun in
// the other hand, no glow on handles unless the gun cannot fire until the
// handle is worked.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace l4d2vr_reload_rules
{
    // ---- Weapon groups (C_WeaponCSBase::WeaponID) -------------------------
    inline constexpr bool IsShellFed(int id) { return id == 3 || id == 4 || id == 8 || id == 11; }
    inline constexpr bool IsManualPump(int id) { return id == 3 || id == 8; }
    inline constexpr bool IsSidearm(int id) { return id == 1 || id == 32; }
    // Detachable magazine guns (vr_hand_vr_bridge MagazineInteractionWeaponUsesDetachableMagazine).
    inline constexpr bool HasDetachableMagazine(int id)
    {
        return id == 1 || id == 2 || id == 5 || id == 6 || id == 7 || id == 9 || id == 10 || id == 26 ||
            (id >= 32 && id <= 36);
    }
    // Physical reload guns (shells or a detachable magazine). The grenade
    // launcher (21), M60 (37) and chainsaw (20) keep the stock reload.
    inline constexpr bool UsesPhysicalReload(int id) { return IsShellFed(id) || HasDetachableMagazine(id); }

    // ---- Issue 2a: "finished loading" is a fresh press -------------------------
    // The bolt/pump stage of a shell gun is armed only by a new press of the
    // interaction input (or the pump fore-end grip), never by the input that
    // is still held from carrying the last shell: not within 250 ms of a shell
    // confirm and not before the input was released once after it.
    constexpr std::uint64_t kFinishAfterShellConfirmMs = 250u;
    inline bool MayFinishShotgunLoading(bool shotgun, bool chamberEmpty, int clip, int shellsLoaded,
        bool inputJustPressed, bool pumpForegripHeld, bool requiresRelease, std::uint64_t msSinceShellConfirm)
    {
        if (!shotgun || !chamberEmpty || (clip <= 0 && shellsLoaded <= 0)) return false;
        if (pumpForegripHeld) return true; // the pump's own fore-end grip, not the shell input
        return inputJustPressed && !requiresRelease && msSinceShellConfirm >= kFinishAfterShellConfirmMs;
    }

    // ---- Pouch: one spare per squeeze (issues 2, 3b, E14, G8) -----------------
    enum class PouchResult : std::uint8_t
    {
        Take = 0,
        NotPressed,
        NoGunInOtherHand,   // Pavlov: the pouch only serves the gun in the other hand
        TubeFull,
        NoReserve,          // Pavlov OutOfAmmoSound: nothing spawns
        RateLimited,        // Pavlov MagGrabRateMax
    };
    inline const char* PouchResultName(PouchResult r)
    {
        switch (r)
        {
        case PouchResult::Take: return "take";
        case PouchResult::NotPressed: return "not-pressed";
        case PouchResult::NoGunInOtherHand: return "no-gun-in-other-hand";
        case PouchResult::TubeFull: return "tube-full";
        case PouchResult::NoReserve: return "reserve-empty";
        case PouchResult::RateLimited: return "rate-limited";
        }
        return "?";
    }
    // Pavlov VRGun MagGrabRateMax: 0.25 s; shotguns use 0.15 s.
    inline float PouchGrabIntervalSeconds(int weaponId) { return IsShellFed(weaponId) ? 0.15f : 0.25f; }
    struct PouchInput
    {
        bool pressed = false;           // a fresh press (or the quick-reload auto grab)
        bool gunInOtherHand = true;
        bool shellMode = false;
        int clip = 0;
        int maxClip = 0;
        int reserve = 0;                // rounds the pouch can give (reserve, plus rounds still owed back)
        bool infiniteReserve = false;   // pistol, magnum, sv_infinite_ammo
        float secondsSinceLastTake = 1e9f;
        int weaponId = 0;
    };
    inline PouchResult EvaluatePouchTake(const PouchInput& in)
    {
        if (!in.pressed) return PouchResult::NotPressed;
        if (!in.gunInOtherHand) return PouchResult::NoGunInOtherHand;
        if (in.shellMode && in.maxClip > 0 && in.clip >= in.maxClip) return PouchResult::TubeFull;
        if (!in.infiniteReserve && in.reserve <= 0) return PouchResult::NoReserve;
        const float interval = PouchGrabIntervalSeconds(in.weaponId);
        if (std::isfinite(in.secondsSinceLastTake) && in.secondsSinceLastTake >= 0.0f &&
            in.secondsSinceLastTake < interval)
            return PouchResult::RateLimited;
        return PouchResult::Take;
    }
    // The spare drawn at the pouch is shown only when a take could succeed
    // (no phantom shell at reserve 0, none with a full tube).
    inline bool ShowPouchSpare(bool shellMode, int clip, int maxClip, int reserve, bool infiniteReserve)
    {
        if (shellMode && maxClip > 0 && clip >= maxClip) return false;
        return infiniteReserve || reserve > 0;
    }

    // ---- Held spare: sticky like Pavlov (issue 2d, G8) ------------------------
    // A taken shell or magazine stays in the hand until it goes in, until the
    // input is pressed again away from the port (drop / put back), or until it
    // has been let go for kHeldSpareReleaseDropSeconds. A tap no longer loses it.
    // A caught *old* magazine still falls when let go (that is the drop).
    constexpr float kHeldSpareSecondPressMinAge = 0.15f;
    constexpr float kHeldSpareReleaseDropSeconds = 3.0f;
    enum class HeldSpare : std::uint8_t { Keep = 0, Drop, DropSecondPress, DropReleasedTooLong };
    inline HeldSpare EvaluateHeldSpare(bool retainedOldMagazine, bool inputDown, bool inputJustPressed,
        float ageSeconds, float releasedSeconds, bool atPort)
    {
        if (retainedOldMagazine) return inputDown ? HeldSpare::Keep : HeldSpare::Drop;
        if (atPort) return HeldSpare::Keep;
        if (inputJustPressed && ageSeconds >= kHeldSpareSecondPressMinAge) return HeldSpare::DropSecondPress;
        if (!inputDown && std::isfinite(releasedSeconds) && releasedSeconds >= kHeldSpareReleaseDropSeconds)
            return HeldSpare::DropReleasedTooLong;
        return HeldSpare::Keep;
    }

    // ---- Reload cue mask (issues 4, 8, G7) ------------------------------------
    // Bits (VR::m_ReloadHintMask): 1 empty magazine, 2 bolt/slide/pump of the
    // anchor gun, 4 spare at the pouch, 8 spare within reach, 16 seated
    // magazine to slap, 32 slide of the dual pistols' other gun.
    constexpr std::uint32_t kHintEmptyMagazine = 1u, kHintBolt = 2u, kHintPouch = 4u, kHintPouchNear = 8u,
        kHintSlapTarget = 16u, kHintOtherSlide = 32u;
    struct HintInput
    {
        bool enabled = false;           // MagazineEmptyGlowEnabled
        bool interactionAvailable = false; // the reload update ran for a live, held gun
        bool emptyMagazine = false;
        // Single gun session state.
        bool waitingForBoltGrab = false;
        bool waitingForFresh = false;
        bool holdingSpare = false;
        bool holdingForSlap = false;
        bool spareNear = false;
        bool pouchHasSpare = true;
        bool manualPumpBlocksFire = false;  // pump/chrome fired, fore-end not cycled
        bool shellMode = false;
        bool chamberEmpty = false;          // the gun cannot fire until the action is worked
        int clip = -1;
        // Dual pistols (physical dual reload): per gun "needs rack".
        bool dualPistols = false;
        bool dualNeedsRackAnchor = false;
        bool dualNeedsRackOther = false;
    };
    inline std::uint32_t BuildReloadHintMask(const HintInput& in)
    {
        if (!in.enabled || !in.interactionAvailable) return 0u;
        if (in.dualPistols)
            return (in.dualNeedsRackAnchor ? kHintBolt : 0u) | (in.dualNeedsRackOther ? kHintOtherSlide : 0u);
        std::uint32_t hint = in.emptyMagazine ? kHintEmptyMagazine : 0u;
        // The action glows only when the gun cannot fire until it is worked:
        // the bolt stage is armed, a pump waits for its stroke, or a round is
        // in the tube/magazine with nothing in the chamber (SPAS/auto after a
        // dry reload, any gun drawn again with an empty chamber). Never while
        // a spare is in the hand (the next step is the port).
        const bool loadedEmptyChamber = in.chamberEmpty && in.clip > 0 && !in.holdingSpare && !in.holdingForSlap;
        if (in.waitingForBoltGrab || in.manualPumpBlocksFire || loadedEmptyChamber)
            hint |= kHintBolt;
        if (in.waitingForFresh && in.pouchHasSpare)
        {
            hint |= kHintPouch;
            if (in.spareNear) hint |= kHintPouchNear;
        }
        if (in.holdingForSlap) hint |= kHintSlapTarget;
        return hint;
    }

    // ---- Incapacitated (issues 9, 10, G6) --------------------------------------
    // L4D2 lets a downed survivor fire and reload pistols (single, dual) and
    // the magnum; nothing else is held while down.
    inline constexpr bool UsableWhileIncapacitated(int weaponId) { return IsSidearm(weaponId); }
    // Physical reloading is available for this state: alive, in first person,
    // not observing, not pinned/pulled/hanging; downed only with a sidearm.
    inline bool PhysicalReloadStateAllowed(bool alive, bool observer, bool controlledBySI, bool ledgeHang,
        bool incapacitated, int weaponId)
    {
        if (!alive || observer || controlledBySI || ledgeHang) return false;
        return !incapacitated || UsableWhileIncapacitated(weaponId);
    }

    // ---- Stock reload gate (issue 14, E6, G3) -----------------------------------
    // CreateMove gap bits (hooks_createmove.inl): why a command does not carry
    // the block impulse.
    constexpr std::uint32_t kGapImpulseBusy = 1u, kGapNoProtocol = 2u, kGapPhysicalOff = 4u, kGapNoHands = 8u,
        kGapOwnReloadCommand = 16u, kGapDualNativeFallback = 32u, kGapIncapacitated = 64u,
        kGapNotFirearm = 128u, kGapNoBackend = 256u;
    enum class GateImpulse : std::uint8_t { None = 0, Block, StickyBlock, Allow };
    // Without the sticky gate (old servers): Block or nothing, as in 2.0.11.
    // With it: StickyBlock blocks until an explicit Allow; commands that carry
    // nothing (impulse busy, the client still holds a grenade or a heal item
    // while the server already deployed the gun) keep the block standing.
    inline GateImpulse ResolveGateImpulse(std::uint32_t gap, bool stickySupported)
    {
        if (gap == 0u) return stickySupported ? GateImpulse::StickyBlock : GateImpulse::Block;
        if (!stickySupported) return GateImpulse::None;
        if ((gap & (kGapImpulseBusy | kGapNotFirearm)) != 0u) return GateImpulse::None;
        // Physical reload off, no backend for this gun, the client's own
        // native reload, the dual-pistol native fallback: allow it explicitly.
        return GateImpulse::Allow;
    }

    // ---- Pull the seated magazine out by hand (MagazineHandPullEnabled) ---------
    // Pavlov VRMagazineSlot: the magazine follows the hand along the slot axis;
    // the insert/extract aborts when the hand leaves the slide bounds sideways.
    // Here: an empty off hand grips the seated magazine at the well; moving it
    // out along the well axis past `pullMeters` ejects it into that hand; a
    // sideways excursion beyond `lateralMeters` (or letting go first) gives
    // it back to the well.
    class MagazineHandPull
    {
    public:
        enum class State : std::uint8_t { Idle = 0, Gripping, Pulled, Aborted };
        void Reset() { m_State = State::Idle; m_MaxOut = 0.0f; }
        State Current() const { return m_State; }
        float MaxOutMeters() const { return m_MaxOut; }
        // Capture: an empty off hand pressing within `reach` of the seated magazine.
        bool Begin(bool handEmpty, bool justPressed, float distanceMeters, float reachMeters, bool magazineSeated)
        {
            if (m_State == State::Gripping) return false;
            if (!handEmpty || !justPressed || !magazineSeated || !std::isfinite(distanceMeters) ||
                !std::isfinite(reachMeters) || distanceMeters < 0.0f || distanceMeters > reachMeters)
                return false;
            m_State = State::Gripping;
            m_MaxOut = 0.0f;
            return true;
        }
        // alongOut: displacement out of the well along its axis (m), lateral:
        // sideways distance from the axis (m).
        State Update(bool inputDown, float alongOut, float lateral, float pullMeters, float lateralMeters)
        {
            if (m_State != State::Gripping) return m_State;
            if (!inputDown || !std::isfinite(alongOut) || !std::isfinite(lateral))
            {
                m_State = State::Aborted;
                return m_State;
            }
            m_MaxOut = std::max(m_MaxOut, alongOut);
            if (lateral > std::max(0.01f, lateralMeters) && alongOut < pullMeters)
            {
                m_State = State::Aborted;
                return m_State;
            }
            if (alongOut >= std::max(0.005f, pullMeters)) m_State = State::Pulled;
            return m_State;
        }
    private:
        State m_State = State::Idle;
        float m_MaxOut = 0.0f;
    };
}
