#pragma once
#include <algorithm>
#include <cmath>
#include <initializer_list>

// Per-weapon physical reload tuning taken from Pavlov VR (read from its
// shipped data; numbers only). Pavlov describes a charging handle, slide or
// pump by its hand travel (LinearRange, cm), the fraction of it that cocks the
// gun (CokingThreashold, default 0.38) and the fraction below which the round
// is chambered on the way forward (ReChamberThreashold, default 0.11). A
// magazine seats once pushed SlideGoalThreshold cm into its well.
//
// Our bolt stage has the same shape: the bolt cocks once pulled `pull` metres,
// and the round chambers when it is let go after that or pushed back to within
// `ret` metres of rest. Hand travel and slide travel are separate, as in
// Pavlov: the slide model only moves `visual` metres however far the hand goes.
namespace l4d2vr_reload_tuning
{
    struct Tuning
    {
        float pullMeters = 0.0f;        // hand pull that cocks
        float returnMeters = 0.0f;      // chamber once back within this of rest
        float insertDepthMeters = 0.0f; // magazine depth that seats it
        float visualTravelMeters = 0.0f;// how far the slide model moves
    };

    struct PavlovHandle
    {
        float rangeCm;
        float coking;
        float rechamber;
        float slideGoalCm;
        float visualCm;
    };

    // Hand travel that is too short is lost in tracking noise; longer than a
    // forearm's comfortable pull is tiring on a viewmodel-sized gun.
    // 2.0.4: 11.4 cm, Pavlov's longest handle (AutoSniper / AutoShotgun
    // 30 cm x 0.38); it was 10 cm.
    constexpr float kMinPullMeters = 0.03f;
    constexpr float kMaxPullMeters = 0.114f;
    // Our magazine depth check needs the magazine box to overlap the well this
    // much; Pavlov's deeper goals (Uzi 13 cm, 1911 9 cm) are capped here.
    constexpr float kMinInsertMeters = 0.01f;
    constexpr float kMaxInsertMeters = 0.04f;

    inline Tuning FromPavlov(const PavlovHandle& h)
    {
        Tuning t{};
        const float rawPull = h.rangeCm * h.coking * 0.01f;
        const float pull = std::clamp(rawPull, kMinPullMeters, kMaxPullMeters);
        // Keep the chamber point at the same fraction of the cock point when
        // the pull is clamped.
        const float ratio = h.coking > 0.001f ? h.rechamber / h.coking : 0.0f;
        t.pullMeters = pull;
        t.returnMeters = std::clamp(pull * ratio, 0.0f, pull - 0.005f);
        // Pavlov seats a magazine once it is 95 % of SlideGoalThreshold in.
        t.insertDepthMeters = std::clamp(h.slideGoalCm * 0.0095f, kMinInsertMeters, kMaxInsertMeters);
        t.visualTravelMeters = std::min(pull, h.visualCm * 0.01f);
        return t;
    }

    // weaponId: L4D2 C_WeaponCSBase::WeaponID. Returns false for weapons that
    // keep the global settings (launcher, M60, non-guns).
    inline bool ForWeapon(int weaponId, Tuning& out)
    {
        // { range cm, coking, rechamber, slide goal cm, visual slide travel cm }
        // Pavlov source gun in the comment. Guns whose Pavlov magazine slides
        // home by itself once it is in the well (bEasyMagSlide: M9, Desert
        // Eagle, SKS, Kar98 and the shotgun shells) take the shallowest seat
        // (1 cm): once lined up in the well it goes in.
        PavlovHandle h{};
        switch (weaponId)
        {
        case 1:  h = { 20.0f, 0.38f, 0.20f,  1.0f, 3.0f }; break; // pistol        <- M9 (easy slide)
        case 32: h = { 25.0f, 0.38f, 0.175f, 1.0f, 3.5f }; break; // magnum        <- Desert Eagle (easy slide)
        case 2:  h = { 20.0f, 0.38f, 0.11f,  2.0f, 4.0f }; break; // uzi           <- MP5 (closed-bolt SMG)
        case 7:  h = {  5.0f, 1.00f, 0.65f,  2.0f, 4.0f }; break; // mac10         <- Kriss Vector
        case 33: h = { 20.0f, 0.38f, 0.11f,  2.0f, 5.0f }; break; // mp5           <- MP5
        case 5:  h = {  7.0f, 1.00f, 0.70f,  2.0f, 6.0f }; break; // m16           <- M16
        case 26: h = { 25.0f, 0.38f, 0.11f,  1.0f, 8.0f }; break; // ak47          <- AK47
        // 2.0.14: the SCAR takes the M16's numbers, the gun its recoil uses
        // (vr_recoil_data.h); it had the SCAR20's 10 cm handle.
        case 9:  h = {  7.0f, 1.00f, 0.70f,  2.0f, 6.0f }; break; // scar          <- M16
        case 34: h = {  8.0f, 0.80f, 0.30f,  2.0f, 6.0f }; break; // sg552         <- Galil
        case 6:  h = { 10.0f, 1.00f, 0.50f,  1.0f, 7.0f }; break; // hunting rifle <- SKS (easy slide)
        case 10: h = { 30.0f, 0.38f, 0.11f,  2.0f, 7.0f }; break; // military      <- AutoSniper
        case 35: h = { 12.0f, 0.90f, 0.80f,  2.0f, 9.0f }; break; // awp           <- AWP
        case 36: h = { 12.0f, 0.90f, 0.85f,  1.0f, 9.0f }; break; // scout         <- Kar98 (easy slide)
        case 3:                                                    // pump shotgun  <- Shotgun
        case 8:  h = { 17.0f, 1.00f, 0.20f,  1.0f, 9.0f }; break; // chrome        <- Shotgun (shell: 1 cm, easy slide)
        case 4:                                                    // autoshotgun   <- AutoShotgun
        case 11: h = { 30.0f, 0.38f, 0.11f,  1.0f, 7.0f }; break; // spas          <- AutoShotgun (shell: 1 cm, easy slide)
        default: return false;
        }
        out = FromPavlov(h);
        return true;
    }

    // Pavlov bLockBolt: the slide or bolt locks open on the last round (M9,
    // Desert Eagle, M16, SCAR20, SKS). With a fresh magazine in, the locked
    // handle only has to be taken and let go (it slingshots forward and
    // chambers); a full rack works too.
    inline bool LocksOpenWhenEmpty(int weaponId)
    {
        switch (weaponId)
        {
        case 1:  // pistol        <- M9
        case 32: // magnum        <- Desert Eagle
        case 5:  // m16           <- M16
        case 9:  // scar          <- M16
        case 6:  // hunting rifle <- SKS
            return true;
        default:
            return false;
        }
    }

    // 2.0.14 Pavlov bBoltLockManual ("HK" guns: MP5, AutoSniper): racking
    // the handle with no rounds in the magazine and letting go locks it open;
    // a fresh magazine and a take-and-let-go (or the magazine / bolt release
    // button) closes it. The uzi takes the MP5's handle numbers but is an
    // open-bolt Uzi in Pavlov, so it does not lock.
    inline bool LocksOpenOnManualRack(int weaponId)
    {
        switch (weaponId)
        {
        case 33: // mp5      <- MP5
        case 10: // military <- AutoSniper
            return true;
        default:
            return false;
        }
    }

    // 2.0.14 Pavlov handle travel haptics (VRHandleComponent, SPEC 4.1): a
    // 10 ms pulse every 0.01 of handle input (hand travel / LinearRange) at
    // Min + (Max - Min) x input. rangeCm is the handle's LinearRange, the same
    // one ForWeapon cocks from. Bolt-action rifles and the pump have their
    // own event pulses instead (enabled = false).
    struct HandleHaptic
    {
        float rangeCm;
        float minAmp;
        float maxAmp;
        bool enabled;
    };

    inline HandleHaptic HandleHapticForWeapon(int weaponId)
    {
        switch (weaponId)
        {
        case 1:  return { 20.0f, 0.5f, 1.0f, true };   // pistol        <- M9
        case 32: return { 25.0f, 0.5f, 1.0f, true };   // magnum        <- Desert Eagle
        case 2:  return { 20.0f, 0.05f, 0.7f, true };  // uzi           <- MP5 handle (ForWeapon)
        case 7:  return { 5.0f, 0.05f, 0.7f, true };   // mac10         <- Kriss Vector
        case 33: return { 20.0f, 0.05f, 0.7f, true };  // mp5           <- MP5
        case 5:  return { 7.0f, 0.05f, 0.7f, true };   // m16           <- M16
        case 9:  return { 7.0f, 0.05f, 0.7f, true };   // scar          <- M16
        case 26: return { 25.0f, 0.05f, 0.7f, true };  // ak47          <- AK47
        case 34: return { 8.0f, 0.05f, 0.7f, true };   // sg552         <- Galil
        case 6:  return { 10.0f, 0.05f, 0.7f, true };  // hunting rifle <- SKS
        case 10: return { 30.0f, 0.05f, 0.7f, true };  // military      <- AutoSniper
        case 4:
        case 11: return { 30.0f, 0.05f, 0.7f, true };  // auto / spas   <- AutoShotgun
        case 37: return { 50.0f, 0.1f, 1.0f, true };   // m60           <- LMGA
        case 35: return { 12.0f, 0.0f, 0.0f, false };  // awp           <- AWP (bolt buffer)
        case 36: return { 12.0f, 0.0f, 0.0f, false };  // scout         <- Kar98 (bolt buffer)
        case 3:
        case 8:  return { 17.0f, 0.0f, 0.0f, false };  // pump / chrome <- Shotgun (pump buffers)
        default: return { 5.0f, 0.2f, 0.8f, false };   // VRHandleComponent defaults, off
        }
    }

    // Pavlov handle input for a hand travel (metres): travel / LinearRange.
    inline float HandleInput(int weaponId, float handTravelMeters)
    {
        const HandleHaptic h = HandleHapticForWeapon(weaponId);
        if (!(h.rangeCm > 0.0f) || !std::isfinite(handTravelMeters))
            return 0.0f;
        return (std::max)(0.0f, handTravelMeters * 100.0f / h.rangeCm);
    }

    // The cock point as handle input (the clamped pull / LinearRange).
    inline float CockInput(int weaponId)
    {
        Tuning t{};
        if (!ForWeapon(weaponId, t))
            return 0.0f;
        return HandleInput(weaponId, t.pullMeters);
    }

    // Bolt-action rifles (AWP_BoltHaptic on their bolt events).
    inline bool IsBoltAction(int weaponId) { return weaponId == 35 || weaponId == 36; }

    // Pavlov magazine slap (bAllowMagSlap): striking the seated magazine with a
    // fresh one at this speed, along its axis within this dot, knocks it out; the
    // fresh magazine may go in SlapReinsertTime later. VRMagazine defaults are
    // 90 cm/s and 0.68; Magazine_AKBase (AK, AK12, AKshorty) uses 80 cm/s and 0.6.
    // Galil and AutoSniper set only bAllowMagSlap, so they keep the defaults.
    struct Slap
    {
        float speedMetersPerSecond;
        float forwardDot;
        float reinsertSeconds;
    };

    constexpr float kSlapReinsertSeconds = 0.15f;

    // How a spare magazine (or shell) sits in the hand. Pavlov attaches a held
    // item at its PickRotationOffset from the motion controller: magazines at
    // pitch -75 deg, the guns that feed from them at -60 (Gun_Base: M16, AK,
    // SCAR, MP5, shotguns, whose Magazine_PumpShotgun shell is also -75) or -70
    // (M9, Desert Eagle, Glock, Uzi). So a magazine is held as its gun is held,
    // upright with its top leaning this many degrees further forward; the hand
    // comes up a little nose-high to seat it. Guns whose magazine goes into the
    // pistol grip (pistols, Uzi, MAC-10) lean 5 deg, the rest 15 deg.
    inline float MagazineHoldTiltDeg(int weaponId)
    {
        switch (weaponId)
        {
        case 1:  // pistol      <- M9
        case 32: // magnum      <- Desert Eagle
        case 2:  // uzi         <- Uzi
        case 7:  // mac10       <- Uzi (magazine in the grip)
            return 5.0f;
        default:
            return 15.0f;
        }
    }

    // 2.0.12: Pavlov VRMagazineSlotComponent per gun (numbers only): the slot
    // captures a magazine within MagSlideBounds (cm, half extents; default 10
    // per axis), the magazine follows the hand along the slot axis at up to
    // ReloadSpeedMultiplier (clamped 2.5-10) per second, seats at 0.95 of
    // SlideGoalThreshold and a release completes at <= 0.35 of it. An insert
    // or a pull aborts when the hand leaves the bounds sideways.
    struct MagSlot
    {
        float boundsCm[3];
        float slideGoalCm;
        float reloadSpeed;
    };
    constexpr float kSeatFraction = 0.95f;
    constexpr float kReleaseCompleteFraction = 0.35f;
    inline float ClampReloadSpeed(float speed) { return std::clamp(speed, 2.5f, 10.0f); }
    inline MagSlot SlotForWeapon(int weaponId)
    {
        switch (weaponId)
        {
        case 1:  return { { 17.5f, 16.0f, 10.0f }, 3.0f, 6.5f };  // pistol        <- M9
        case 32: return { { 16.5f, 15.0f, 10.0f }, 2.0f, 6.25f }; // magnum        <- Desert Eagle
        case 2:  return { { 10.0f, 10.0f, 10.0f }, 2.0f, 7.0f };  // uzi           <- MP5
        case 7:  return { { 10.0f, 10.0f, 10.0f }, 2.0f, 7.0f };  // mac10         <- Kriss Vector
        case 33: return { { 10.0f, 10.0f, 10.0f }, 2.0f, 7.0f };  // mp5           <- MP5
        case 5:  return { { 10.0f, 10.0f, 10.0f }, 2.0f, 7.0f };  // m16           <- M16
        // 2.0.14: Gun_AK47 overrides the AK base's 10 cm SlideGoalThreshold to
        // 1 cm (ForWeapon(26) already seats at 1 cm): rocked in, it is home.
        case 26: return { { 10.0f, 10.0f, 10.0f }, 1.0f, 10.0f };  // ak47         <- AK47
        case 9:  return { { 10.0f, 10.0f, 10.0f }, 2.0f, 7.0f };  // scar          <- M16 (2.0.14)
        case 34: return { { 10.0f, 10.0f, 10.0f }, 2.0f, 7.0f };  // sg552         <- Galil
        case 6:  return { { 15.0f, 15.0f, 15.0f }, 4.0f, 5.5f };  // hunting rifle <- SKS
        case 10: return { { 10.0f, 10.0f, 10.0f }, 2.0f, 7.0f };  // military      <- AutoSniper
        case 35: return { { 10.0f, 10.0f, 10.0f }, 2.0f, 7.0f };  // awp           <- AWP
        case 36: return { { 15.0f, 15.0f, 15.0f }, 4.0f, 5.5f };  // scout         <- Kar98
        case 3: case 8: return { { 20.0f, 15.0f, 15.0f }, 1.0f, 7.0f }; // pump/chrome <- Shotgun (shell port)
        case 4: case 11: return { { 20.0f, 15.0f, 15.0f }, 1.0f, 7.0f }; // auto/spas <- AutoShotgun (shell port)
        default: return { { 10.0f, 10.0f, 10.0f }, 1.0f, 7.0f };       // VRMagazineSlot defaults
        }
    }
    // The sideways bound of the slot: its narrowest half extent.
    inline float SlotLateralBoundMeters(int weaponId)
    {
        const MagSlot slot = SlotForWeapon(weaponId);
        return (std::min)({ slot.boundsCm[0], slot.boundsCm[1], slot.boundsCm[2] }) / 100.0f;
    }
    // Pavlov VRHandleComponent box half extents (cm) of the gun's reload
    // handle (slide, charging handle, bolt): for the debug view and the
    // hand-sphere test. The pump fore-end uses the two-hand grip box.
    inline void HandleBoxCm(int weaponId, float out[3])
    {
        float x = 3.0f, y = 3.0f, z = 3.0f;                                   // Gun_Base
        switch (weaponId)
        {
        case 1: case 32: x = 10.64f; y = 1.93f; z = 4.0f; break;              // M9 / Desert Eagle slide
        case 26: x = 3.0f; y = 3.0f; z = 5.0f; break;                          // AK charging handle
        case 35: x = 11.35f; y = 3.0f; z = 3.0f; break;                        // AWP bolt
        case 36: case 6: x = 11.35f; y = 3.0f; z = 7.0f; break;                // Kar98 / SKS bolt
        case 4: case 11: x = 2.0f; y = 2.0f; z = 2.0f; break;                  // AutoShotgun handle
        case 3: case 8: x = 9.92f; y = 2.0f; z = 4.11f; break;                 // Shotgun fore-end (TwoHandGrip box)
        default: break;
        }
        out[0] = x; out[1] = y; out[2] = z;
    }

    inline bool SlapForWeapon(int weaponId, Slap& out)
    {
        switch (weaponId)
        {
        case 26: out = { 0.80f, 0.60f, kSlapReinsertSeconds }; return true; // ak47     <- AK (Magazine_AKBase)
        case 34: out = { 0.90f, 0.68f, kSlapReinsertSeconds }; return true; // sg552    <- Galil
        case 10: out = { 0.90f, 0.68f, kSlapReinsertSeconds }; return true; // military <- AutoSniper
        default: return false;
        }
    }
}
