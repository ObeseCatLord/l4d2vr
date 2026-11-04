#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// Chest sling for long guns (ChestSlingEnabled, 2.0.4; Forefront's chest
// sling since 2.0.5) and empty hands (HolsterEmptyHands, 2.0.5).
//
// 2.0.5: the sling is Forefront's (vr_holster.h SlingPose): the slot1 gun hangs
// there whether it is the active weapon or not. Putting any item away in its
// own holster, or letting go of a sidearm near the body, stows it the same
// way: it stays L4D2's active weapon but hangs on the body, and both hands are
// empty (see vr.h m_ChestSlungWeapon / m_StowedSlot).
//
// A long gun (rifles, SMGs, shotguns, snipers, the M60 and the grenade
// launcher) put away on the sling hangs in front of the chest, still the
// active weapon (no weapon switch, nothing sent to the server), and both hands
// are free. Either hand takes it back by squeezing its grip at the slung gun's
// handle or foregrip, and that hand becomes the trigger hand: the only way to
// use it in the other hand is to put it away and take it with that hand.
// While it is slung the gun cannot fire, reload or bash.
//
// Letting go over a holster stows it there (the back for a long gun), and
// pistols, melee weapons, grenades and kits keep their holsters. Since 2.0.6
// nothing passes a held gun to the other hand (no release transfer).
//
// Where "letting go" puts the gun (ReleaseInput.toggleGrip):
// - Long guns and the chest sling (both grip modes, unchanged from 2.0.5): near
//   the body (within ChestSlingDropDistanceMeters, 0.6 m, of the sling point)
//   the gun hangs on the sling; with the arm stretched away it drops.
// - Sidearms, toggle grip (GripToggle, the default since 2.0.6, Pavlov's Click
//   mode): the owner's second squeeze puts it back on the hip only with the
//   hand within putAwayReachMeters (the hip holster's grab reach); anywhere
//   else it is dropped at the hand. A squeeze during a swing drops a melee
//   weapon with the hand's motion (that is the melee throw).
// - Hold grip (GripToggle=false, the 2.0.5 rule): letting go of the grip. Near
//   the body (within ChestSlingDropDistanceMeters, 0.6 m, of the sling point or
//   hip holster) it is put away; with the arm stretched away it drops; a fast
//   swing that lets go of a melee weapon throws it.
//
// std-only so the rules are unit-tested offline (tests/vr_chest_sling_tests.cpp).
namespace l4d2vr_chest_sling
{
    // C_WeaponCSBase::WeaponID values of the guns that use the sling.
    inline bool IsLongGun(int weaponId)
    {
        switch (weaponId)
        {
        case 2:  // smg (Uzi)
        case 3:  // pump shotgun
        case 4:  // auto shotgun
        case 5:  // M16
        case 6:  // hunting rifle
        case 7:  // MAC-10
        case 8:  // chrome shotgun
        case 9:  // SCAR (desert rifle)
        case 10: // military sniper
        case 11: // SPAS
        case 21: // grenade launcher
        case 26: // AK-47
        case 33: // MP5
        case 34: // SG552
        case 35: // AWP
        case 36: // scout
        case 37: // M60
            return true;
        default:
            return false;
        }
    }

    enum class ReleaseAction { None, Drop, Sling };

    struct ReleaseInput
    {
        bool enabled = true;            // ChestSlingEnabled
        bool dropRequested = false;     // the grip let go of the held item where it would drop
        int weaponId = 0;
        float handToSlingMeters = 0.0f; // the releasing hand to the sling point
        float dropDistanceMeters = 0.6f;// ChestSlingDropDistanceMeters
        float handSpeedMetersPerSecond = 0.0f; // sidearms: a hand this fast throws (melee throw, hold grip)
        float throwSpeedMetersPerSecond = 2.5f;
        bool toggleGrip = false;        // the owner's squeeze let go (GripToggle): put away only at the holster
        float putAwayReachMeters = 0.15f; // toggle grip: the holster's (sling's) own grab reach
    };

    // Toggle grip: put away within the holster's reach, dropped elsewhere. An
    // unknown distance (no body frame) never drops by accident.
    inline ReleaseAction PutAwayWithinReach(const ReleaseInput& in)
    {
        if (!std::isfinite(in.handToSlingMeters) || in.handToSlingMeters < 0.0f)
            return ReleaseAction::Sling;
        return in.handToSlingMeters > in.putAwayReachMeters ? ReleaseAction::Drop : ReleaseAction::Sling;
    }

    // What letting go does: hang the gun on the sling, or drop it (anything
    // that is not a long gun, the sling off, or an arm stretched away).
    inline ReleaseAction OnRelease(const ReleaseInput& in)
    {
        if (!in.dropRequested)
            return ReleaseAction::None;
        if (!in.enabled || !IsLongGun(in.weaponId))
            return ReleaseAction::Drop;
        // The chest sling keeps its 2.0.5 rule in both grip modes (user, s7):
        // near the body it hangs, at arm's length it drops.
        if (!std::isfinite(in.handToSlingMeters) || in.handToSlingMeters < 0.0f)
            return ReleaseAction::Sling;
        return in.handToSlingMeters > in.dropDistanceMeters ? ReleaseAction::Drop : ReleaseAction::Sling;
    }

    // Slot2 items that return to the hip holster when let go near the body
    // (HolsterEmptyHands, Forefront's sidearm/knife reset): pistol, magnum,
    // melee weapons and the chainsaw.
    inline bool IsSidearm(int weaponId)
    {
        return weaponId == 1 || weaponId == 32 || weaponId == 19 || weaponId == 20;
    }

    // What letting go of a sidearm does: None (not a sidearm, or the option
    // off: the old drop rules apply), Sling (back into the hip holster: the
    // hand is within dropDistanceMeters of it, or the distance is unknown) or
    // Drop (the arm is stretched away from the body).
    inline ReleaseAction OnSidearmRelease(const ReleaseInput& in)
    {
        if (!in.dropRequested || !in.enabled || !IsSidearm(in.weaponId))
            return ReleaseAction::None;
        if (in.toggleGrip)
            return PutAwayWithinReach(in);
        // A swing that lets go is a throw (melee weapons fly), not a holster.
        if (std::isfinite(in.handSpeedMetersPerSecond) && in.handSpeedMetersPerSecond > in.throwSpeedMetersPerSecond)
            return ReleaseAction::Drop;
        if (!std::isfinite(in.handToSlingMeters) || in.handToSlingMeters < 0.0f)
            return ReleaseAction::Sling;
        return in.handToSlingMeters > in.dropDistanceMeters ? ReleaseAction::Drop : ReleaseAction::Sling;
    }

    struct GrabInput
    {
        bool pressEdge[2] = { false, false };     // [0] left, [1] right controller grip just squeezed
        bool claimed[2] = { false, false };       // the squeeze went to something else (a holster, the pouch)
        float toHandleMeters[2] = { -1.0f, -1.0f };
        float toForegripMeters[2] = { -1.0f, -1.0f }; // to the gun body (handle..foregrip)
        float radiusMeters = 0.15f;               // ChestSlingGrabRadiusMeters
    };

    // The hand that takes the slung gun: -1 none, 0 left, 1 right. A hand on
    // the handle wins over one on the foregrip; with both squeezed the same
    // frame on the same part, the closer hand.
    inline int GrabHand(const GrabInput& in)
    {
        int best = -1;
        float bestScore = 1.0e9f;
        for (int hand = 0; hand < 2; ++hand)
        {
            if (!in.pressEdge[hand] || in.claimed[hand])
                continue;
            const float h = in.toHandleMeters[hand];
            const float f = in.toForegripMeters[hand];
            float score = 1.0e9f;
            if (std::isfinite(h) && h >= 0.0f && h <= in.radiusMeters)
                score = h;                      // handle: 0 .. radius
            else if (std::isfinite(f) && f >= 0.0f && f <= in.radiusMeters)
                score = in.radiusMeters + f;    // foregrip only after any handle grab
            if (score < bestScore)
            {
                bestScore = score;
                best = hand;
            }
        }
        return best;
    }

    // The slung gun: which weapon hangs on the sling. It is forgotten when the
    // active weapon changes (a holster draw, a pickup, death) or the sling is
    // switched off.
    class Sling
    {
    public:
        void Hang(uintptr_t weaponTag)
        {
            m_Weapon = weaponTag;
            ++m_Serial;
        }
        void Take()
        {
            if (m_Weapon != 0u)
                ++m_Serial;
            m_Weapon = 0u;
        }
        // Call every frame with the active weapon; returns true while it hangs.
        bool Update(bool enabled, bool alive, uintptr_t activeWeaponTag)
        {
            if (m_Weapon != 0u && (!enabled || !alive || activeWeaponTag != m_Weapon))
            {
                m_Weapon = 0u;
                ++m_Serial;
            }
            return m_Weapon != 0u;
        }
        bool Slung() const { return m_Weapon != 0u; }
        uintptr_t Weapon() const { return m_Weapon; }
        uint32_t Serial() const { return m_Serial; }
    private:
        uintptr_t m_Weapon = 0u;
        uint32_t m_Serial = 0u;
    };

    // Buttons a slung gun must not see (IN_ATTACK, IN_ATTACK2, IN_RELOAD).
    constexpr int kBlockedButtons = (1 << 0) | (1 << 11) | (1 << 13);
    inline int FilterButtons(int buttons, bool slung)
    {
        return slung ? (buttons & ~kBlockedButtons) : buttons;
    }
}
